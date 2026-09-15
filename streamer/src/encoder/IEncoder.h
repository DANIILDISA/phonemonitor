#pragma once

/*
 * =============================================================================
 *  Единый интерфейс аппаратного видеокодировщика.
 * =============================================================================
 *
 *  Реализации:
 *    * NvencEncoder            - NVIDIA NVENC (nvEncodeAPI64.dll), основной путь;
 *    * MediaFoundationEncoder  - аппаратный MFT H.264. Через него работают
 *                                AMD (VCE/VCN, он же бэкенд AMF) и Intel
 *                                (QuickSync). Это штатный и поддерживаемый
 *                                производителями путь доступа к тем же блокам
 *                                кодирования, но без привязки к SDK вендора:
 *                                конвейер остаётся нулевого копирования
 *                                (текстура D3D11 -> MFT), а настройки задержки
 *                                выставляются через ICodecAPI.
 *
 *  Общие требования ко всем реализациям:
 *    1. Вход - ID3D11Texture2D в формате B8G8R8A8_UNORM на устройстве,
 *       переданном в EncoderConfig::device. Никаких копий через системную
 *       память быть не должно.
 *    2. Выход - Annex B поток H.264 (стартовые коды 00 00 00 01), готовый
 *       к отправке в сокет без дополнительной упаковки.
 *    3. EncodeFrame синхронна: вернулась - значит пакет готов. Асинхронный
 *       режим NVENC намеренно не используется: он добавляет кадр задержки.
 */

#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <memory>

namespace pm
{
    /* Параметры инициализации кодировщика. */
    struct EncoderConfig
    {
        ID3D11Device* device = nullptr;   /* устройство, где живут кадры       */
        uint32_t width = 1920;
        uint32_t height = 1080;
        uint32_t fps = 60;
        uint32_t bitrateBps = 15'000'000; /* 15 Мбит/с по умолчанию            */

        /* Intra-Refresh вместо периодических IDR.
           Убирает всплески битрейта от ключевых кадров и даёт устойчивость
           к потерям: повреждённая область «зарастает» за один проход волны. */
        bool intraRefreshEnabled = true;

        /* Доля ширины кадра, обновляемая за один кадр, в процентах.
           5% -> волна проходит экран за 20 кадров (треть секунды при 60 Гц). */
        uint32_t intraRefreshBandPercent = 5;

        /* Низкоуровневые флаги. */
        bool lowLatencyPreset = true;     /* LOW_LATENCY_HP                    */
        bool cbrRateControl = true;       /* постоянный битрейт                */
        bool repeatSpsPps = true;         /* SPS/PPS перед каждым IDR          */
    };

    /* Результат кодирования одного кадра. */
    struct EncodedFrame
    {
        std::vector<uint8_t> data;        /* Annex B поток                     */
        bool isKeyFrame = false;
        uint64_t timestampQpc = 0;        /* QPC исходного кадра               */
        uint32_t encodeTimeUs = 0;        /* чистое время кодирования          */
    };

    class IVideoEncoder
    {
    public:
        virtual ~IVideoEncoder() = default;

        /* Человекочитаемое имя бэкенда - попадает в журнал. */
        virtual const char* Name() const noexcept = 0;

        /* Инициализация. Возвращает ошибку, если оборудование не поддерживается -
           фабрика по этому признаку переходит к следующему бэкенду. */
        virtual HRESULT Initialize(const EncoderConfig& config) noexcept = 0;

        /*
         * Кодирование одного кадра.
         * forceKeyFrame используется при подключении нового клиента: без IDR
         * декодер на телефоне не сможет начать показ.
         */
        virtual HRESULT EncodeFrame(ID3D11Texture2D* frame,
                                    bool forceKeyFrame,
                                    uint64_t timestampQpc,
                                    EncodedFrame& out) noexcept = 0;

        /*
         * Изменение параметров на лету, без пересоздания сессии.
         * Используется при тепловом троттлинге: снижаем битрейт и разрешение.
         * Возвращает S_FALSE, если бэкенд требует полного пересоздания.
         */
        virtual HRESULT Reconfigure(uint32_t width, uint32_t height, uint32_t bitrateBps) noexcept = 0;

        /* Освобождение ресурсов. Должна быть идемпотентной. */
        virtual void Shutdown() noexcept = 0;

        /* Текущая геометрия кодировщика. */
        virtual void GetDimensions(uint32_t& width, uint32_t& height) const noexcept = 0;
    };

    /*
     * Фабрика. Пробует бэкенды в порядке предпочтения и возвращает первый
     * успешно инициализированный. preferredBackend позволяет принудительно
     * выбрать конкретный ("nvenc", "mf", "auto").
     */
    std::unique_ptr<IVideoEncoder> CreateVideoEncoder(const EncoderConfig& config,
                                                      const char* preferredBackend) noexcept;
}
