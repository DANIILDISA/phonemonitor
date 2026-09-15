#pragma once

/*
 * =============================================================================
 *  Бэкенд Media Foundation: аппаратный H.264 MFT.
 * =============================================================================
 *
 *  Через этот бэкенд работают:
 *    * AMD  - блок VCE/VCN. Это тот же кремний, который использует AMF SDK;
 *             MFT AMD является официальной обёрткой над AMF и поставляется
 *             вместе с драйвером Adrenalin.
 *    * Intel- QuickSync Video. MFT входит в состав графического драйвера и
 *             является рекомендованным Intel способом доступа к аппаратному
 *             кодировщику из приложений Windows.
 *    * NVIDIA- NVENC (используется только если прямой путь через nvEncodeAPI
 *             почему-то недоступен).
 *
 *  Почему не линкуемся с AMF и oneVPL напрямую:
 *  каждая из этих библиотек тянет собственный SDK, собственный рантайм и
 *  собственный жизненный цикл устройства. MFT даёт единый интерфейс к тем же
 *  аппаратным блокам, сохраняет конвейер без копий в системную память
 *  (текстура D3D11 передаётся через IMFDXGIDeviceManager) и не требует
 *  ничего, кроме Windows SDK.
 *
 *  ОГРАНИЧЕНИЕ, о котором нужно знать:
 *  Intra-Refresh на этом пути недоступен - ICodecAPI не содержит переносимого
 *  свойства для управления волной обновления. Устойчивость к повреждению
 *  потока здесь обеспечивается иначе:
 *    * TCP гарантирует доставку, поэтому потери пакетов как таковой нет;
 *    * клиент может запросить ключевой кадр сообщением PM_MSG_KEYFRAME_REQ;
 *    * дополнительно раз в keyFrameIntervalSec секунд выдаётся IDR.
 *  Полноценный Intra-Refresh реализован в бэкенде NVENC.
 *
 *  Конвейер внутри:
 *      BGRA-текстура -> ID3D11VideoProcessor -> NV12-текстура -> MFT -> Annex B
 *  Преобразование цвета выполняет видеопроцессор GPU: это отдельный аппаратный
 *  блок, он не занимает ни шейдерные ядра, ни процессор.
 */

#include "IEncoder.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <string>

namespace pm
{
    /*
     * Аппаратный преобразователь цветового пространства BGRA -> NV12.
     * Вынесен в отдельный класс: им пользуется только MFT-бэкенд, NVENC умеет
     * принимать BGRA напрямую.
     */
    class VideoColorConverter
    {
    public:
        VideoColorConverter() noexcept = default;
        ~VideoColorConverter() noexcept;

        HRESULT Initialize(ID3D11Device* device, uint32_t width, uint32_t height) noexcept;
        void Shutdown() noexcept;

        /* Преобразует кадр. Результат остаётся во внутренней NV12-текстуре. */
        HRESULT Convert(ID3D11Texture2D* source) noexcept;

        ID3D11Texture2D* Output() const noexcept { return m_nv12Texture.Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
        Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12Texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_outputView;

        /* Кеш входного представления: пересоздаётся только при смене текстуры. */
        ID3D11Texture2D* m_cachedInput = nullptr;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inputView;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };

    class MediaFoundationEncoder final : public IVideoEncoder
    {
    public:
        MediaFoundationEncoder() noexcept = default;
        ~MediaFoundationEncoder() noexcept override;

        const char* Name() const noexcept override { return m_backendName.c_str(); }

        HRESULT Initialize(const EncoderConfig& config) noexcept override;
        HRESULT EncodeFrame(ID3D11Texture2D* frame, bool forceKeyFrame,
                            uint64_t timestampQpc, EncodedFrame& out) noexcept override;
        HRESULT Reconfigure(uint32_t width, uint32_t height, uint32_t bitrateBps) noexcept override;
        void Shutdown() noexcept override;
        void GetDimensions(uint32_t& width, uint32_t& height) const noexcept override
        {
            width = m_config.width;
            height = m_config.height;
        }

    private:
        /* Перебор аппаратных MFT и выбор лучшего доступного. */
        HRESULT SelectTransform() noexcept;

        /* Настройка типов входа/выхода. */
        HRESULT ConfigureMediaTypes() noexcept;

        /* Настройка низкой задержки через ICodecAPI. */
        void ConfigureCodecApi() noexcept;

        /* Запрос ключевого кадра на следующий вход. */
        void RequestKeyFrame() noexcept;

        /* Подача кадра в MFT. */
        HRESULT SubmitFrame(ID3D11Texture2D* nv12, uint64_t sampleTime100ns) noexcept;

        /* Получение готового пакета. */
        HRESULT DrainOutput(EncodedFrame& out, uint32_t timeoutMs) noexcept;

        /* Обработчик асинхронных событий MFT. */
        HRESULT PumpAsyncEvents(bool wantInput, bool wantOutput,
                                ID3D11Texture2D* nv12, uint64_t sampleTime100ns,
                                EncodedFrame& out, uint32_t timeoutMs) noexcept;

    private:
        EncoderConfig m_config{};
        std::string m_backendName = "Media Foundation H.264";

        Microsoft::WRL::ComPtr<IMFTransform> m_transform;
        Microsoft::WRL::ComPtr<ICodecAPI> m_codecApi;
        Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_eventGenerator;
        Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_deviceManager;
        Microsoft::WRL::ComPtr<IMFMediaType> m_outputType;

        VideoColorConverter m_converter;

        /* SPS/PPS из типа выхода. Дописывается перед каждым ключевым кадром:
           MediaCodec на телефоне обязан увидеть их до первого слайса. */
        std::vector<uint8_t> m_sequenceHeader;

        UINT m_resetToken = 0;
        bool m_asyncTransform = false;
        bool m_streaming = false;
        bool m_mfStarted = false;

        uint64_t m_frameCounter = 0;
        uint64_t m_lastKeyFrameQpc = 0;
    };
}
