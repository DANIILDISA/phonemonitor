#include "IEncoder.h"
#include "MediaFoundationEncoder.h"
#include "../Log.h"

#if defined(PM_ENABLE_NVENC)
#include "NvencEncoder.h"
#endif

#include <cstring>

namespace pm
{
    namespace
    {
        bool NameMatches(const char* requested, const char* candidate) noexcept
        {
            return requested != nullptr && _stricmp(requested, candidate) == 0;
        }
    }

    std::unique_ptr<IVideoEncoder> CreateVideoEncoder(const EncoderConfig& config,
                                                      const char* preferredBackend) noexcept
    {
        /*
         * Порядок перебора задан требованиями к задержке:
         *
         *   1. NVENC напрямую через nvEncodeAPI. Самый короткий путь: кадр BGRA
         *      уходит в кодировщик без цветовой конверсии на нашей стороне,
         *      доступен Intra-Refresh и точная настройка VBV.
         *
         *   2. Media Foundation MFT. Универсальный путь, покрывающий AMD (AMF)
         *      и Intel (QuickSync). Требует конверсии BGRA -> NV12 на
         *      видеопроцессоре, что добавляет доли миллисекунды.
         *
         * Если явно запрошен конкретный бэкенд, отката на другие не происходит:
         * молчаливая подмена кодировщика запутала бы диагностику.
         */

        const bool auto_ = (preferredBackend == nullptr) ||
                           (std::strlen(preferredBackend) == 0) ||
                           NameMatches(preferredBackend, "auto");

#if defined(PM_ENABLE_NVENC)
        if (auto_ || NameMatches(preferredBackend, "nvenc"))
        {
            auto nvenc = std::make_unique<NvencEncoder>();
            const HRESULT hr = nvenc->Initialize(config);
            if (SUCCEEDED(hr))
            {
                PM_INFO("Выбран бэкенд кодирования: %s", nvenc->Name());
                return nvenc;
            }

            if (!auto_)
            {
                PM_ERROR("Принудительно запрошен NVENC, но он недоступен: %s", HResultToText(hr));
                return nullptr;
            }
            PM_INFO("NVENC недоступен, переходим к Media Foundation");
        }
#else
        if (NameMatches(preferredBackend, "nvenc"))
        {
            PM_ERROR("Сборка выполнена без поддержки NVENC. "
                     "Пересоберите с -DNVENC_SDK_DIR=<путь к Video Codec SDK>");
            return nullptr;
        }
#endif

        if (auto_ || NameMatches(preferredBackend, "mf") ||
            NameMatches(preferredBackend, "amf") || NameMatches(preferredBackend, "qsv"))
        {
            auto mft = std::make_unique<MediaFoundationEncoder>();
            const HRESULT hr = mft->Initialize(config);
            if (SUCCEEDED(hr))
            {
                PM_INFO("Выбран бэкенд кодирования: %s", mft->Name());
                return mft;
            }
            PM_ERROR("Media Foundation-кодировщик недоступен: %s", HResultToText(hr));
        }

        PM_ERROR("Не удалось инициализировать ни один аппаратный кодировщик H.264. "
                 "Проверьте, что драйвер видеокарты установлен полностью "
                 "(не через Центр обновления Windows) и поддерживает аппаратное кодирование");
        return nullptr;
    }
}
