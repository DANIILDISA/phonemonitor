#pragma once

/*
 * =============================================================================
 *  Бэкенд NVIDIA NVENC.
 * =============================================================================
 *
 *  Библиотека nvEncodeAPI64.dll загружается ДИНАМИЧЕСКИ. Причина принципиальная:
 *  стример должен запускаться на машинах без NVIDIA. Статическая линковка с
 *  nvencodeapi.lib сделала бы невозможным сам старт процесса на системах AMD
 *  или Intel - загрузчик отказал бы ещё до входа в main.
 *
 *  Заголовок nvEncodeAPI.h входит в NVIDIA Video Codec SDK и НЕ распространяется
 *  вместе с проектом (лицензия NVIDIA). Путь к нему задаётся переменной CMake
 *  NVENC_SDK_DIR; при её отсутствии этот бэкенд исключается из сборки, и
 *  стример работает через Media Foundation.
 *
 *  Настройка под минимальную задержку:
 *    * пресет NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
 *    * CBR, GOP бесконечной длины, B-кадры отключены (frameIntervalP = 1);
 *    * Intra-Refresh: вертикальная полоса шириной 5% кадра, проходящая экран
 *      за 20 кадров, вместо периодических IDR;
 *    * синхронный режим (enableEncodeAsync = 0): EncodePicture возвращается
 *      уже с готовым пакетом, без дополнительного кадра буферизации;
 *    * VBV-буфер размером в один кадр - кодировщик не имеет права «копить»
 *      биты, каждый кадр укладывается в свою долю битрейта.
 */

#include "IEncoder.h"

#if defined(PM_ENABLE_NVENC)

#include <d3d11.h>
#include <nvEncodeAPI.h>

namespace pm
{
    class NvencEncoder final : public IVideoEncoder
    {
    public:
        NvencEncoder() noexcept;
        ~NvencEncoder() noexcept override;

        const char* Name() const noexcept override { return "NVIDIA NVENC"; }

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
        /* Загрузка DLL и получение таблицы функций. */
        HRESULT LoadApi() noexcept;

        /* Формирование NV_ENC_CONFIG под низкую задержку. */
        HRESULT BuildEncodeConfig(NV_ENC_INITIALIZE_PARAMS& initParams,
                                  NV_ENC_CONFIG& encodeConfig) noexcept;

        /* Регистрация входной текстуры в NVENC (делается один раз на текстуру). */
        HRESULT RegisterInputTexture(ID3D11Texture2D* texture) noexcept;

        /* Освобождение зарегистрированного ресурса. */
        void UnregisterInputTexture() noexcept;

    private:
        HMODULE m_nvencLib = nullptr;
        NV_ENCODE_API_FUNCTION_LIST m_api{};
        void* m_encoder = nullptr;           /* непрозрачный указатель сессии   */

        EncoderConfig m_config{};

        /* Текстура, которая сейчас зарегистрирована как вход. NVENC требует
           регистрировать каждый конкретный ресурс, поэтому кешируем указатель
           и перерегистрируем только при его смене. */
        ID3D11Texture2D* m_registeredTexture = nullptr;
        NV_ENC_REGISTERED_PTR m_registeredResource = nullptr;

        /* Выходной буфер битстрима. Один - потому что режим синхронный. */
        NV_ENC_OUTPUT_PTR m_bitstreamBuffer = nullptr;

        uint64_t m_frameCounter = 0;
        bool m_initialized = false;
    };
}

#endif /* PM_ENABLE_NVENC */
