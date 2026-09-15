#include "MonitorModes.h"
#include "Trace.h"

#include <algorithm>
#include <cmath>

namespace pm
{
    namespace
    {
        /* --- Константы компоновки EDID ------------------------------------- */
        constexpr size_t kEdidSize = 128;
        constexpr size_t kOffsetDtd1 = 54;   /* первый подробный дескриптор     */
        constexpr size_t kOffsetDesc2 = 72;  /* пределы частот                  */
        constexpr size_t kOffsetDesc3 = 90;  /* имя монитора                    */
        constexpr size_t kOffsetDesc4 = 108; /* заглушка                        */

        /*
         * Упрощённая модель гашения по CVT Reduced Blanking v1.
         * Для виртуального монитора реальные тайминги не управляют железом -
         * они нужны лишь для внутренней согласованности EDID и расчёта
         * пиксельной частоты. Поэтому берём фиксированные значения RB:
         *   горизонтальное гашение - 160 пикселей,
         *   вертикальное гашение   - 30 строк.
         */
        constexpr uint32_t kHBlank = 160;
        constexpr uint32_t kVBlank = 30;

        /* Кодирует трёхбуквенный идентификатор производителя в два байта EDID.
           Каждая буква - 5 бит, где 'A' == 1. Порядок байт - big-endian. */
        inline void EncodeManufacturerId(uint8_t* out, char a, char b, char c) noexcept
        {
            const uint16_t packed = static_cast<uint16_t>(
                ((a - 'A' + 1) & 0x1F) << 10 |
                ((b - 'A' + 1) & 0x1F) << 5 |
                ((c - 'A' + 1) & 0x1F));
            out[0] = static_cast<uint8_t>(packed >> 8);
            out[1] = static_cast<uint8_t>(packed & 0xFF);
        }

        /* Записывает 18-байтовый текстовый дескриптор (тип 0xFC/0xFE/0xFF). */
        void WriteTextDescriptor(uint8_t* dst, uint8_t tag, const char* text) noexcept
        {
            dst[0] = 0x00; dst[1] = 0x00; dst[2] = 0x00;
            dst[3] = tag;
            dst[4] = 0x00;
            // Тело - ровно 13 символов, добивается 0x0A и пробелами 0x20.
            size_t i = 0;
            for (; i < 13 && text[i] != '\0'; ++i)
            {
                dst[5 + i] = static_cast<uint8_t>(text[i]);
            }
            if (i < 13)
            {
                dst[5 + i] = 0x0A;
                ++i;
            }
            for (; i < 13; ++i)
            {
                dst[5 + i] = 0x20;
            }
        }
    }

    // -------------------------------------------------------------------------
    std::vector<uint8_t> BuildEdid(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept
    {
        std::vector<uint8_t> edid(kEdidSize, 0);

        // --- Заголовок: фиксированная сигнатура 00 FF FF FF FF FF FF 00 ------
        edid[0] = 0x00;
        for (size_t i = 1; i <= 6; ++i) edid[i] = 0xFF;
        edid[7] = 0x00;

        // --- Идентификация устройства ----------------------------------------
        EncodeManufacturerId(&edid[8], 'P', 'M', 'N');   // PMN = PhoneMonitor
        edid[10] = 0x01; edid[11] = 0x00;                 // код изделия 0x0001
        edid[12] = 0x01; edid[13] = 0x00;                 // серийный номер
        edid[14] = 0x00; edid[15] = 0x00;
        edid[16] = 0x01;                                  // неделя изготовления
        edid[17] = 35;                                    // год: 1990 + 35 = 2025

        edid[18] = 0x01;                                  // версия EDID 1
        edid[19] = 0x04;                                  // ревизия 4

        // --- Параметры видеовхода --------------------------------------------
        // Бит 7 = 1 - цифровой вход; биты 6..4 = 001 - 8 бит на компоненту;
        // биты 3..0 = 0101 - интерфейс DisplayPort.
        edid[20] = 0xA5;

        // Физический размер в сантиметрах. Считаем по диагонали ~6.5 дюйма
        // (типичный телефон), сохраняя пропорции запрошенного разрешения.
        // Это напрямую влияет на DPI, который Windows предложит пользователю.
        {
            const double aspect = (height != 0)
                ? static_cast<double>(width) / static_cast<double>(height)
                : 1.0;
            const double diagonalCm = 16.5;                     // 6.5 дюйма
            const double heightCm = diagonalCm / std::sqrt(1.0 + aspect * aspect);
            const double widthCm = heightCm * aspect;
            edid[21] = static_cast<uint8_t>(std::clamp(widthCm, 1.0, 255.0));
            edid[22] = static_cast<uint8_t>(std::clamp(heightCm, 1.0, 255.0));
        }

        edid[23] = 120;   // гамма: (2.20 * 100) - 100 = 120
        // Бит 2 - стандартное пространство sRGB, бит 1 - первый DTD является
        // предпочтительным режимом, бит 0 - поддержка непрерывных частот.
        edid[24] = 0x06;

        // --- Координаты цветности (типовые значения sRGB) ---------------------
        static const uint8_t kSrgbChroma[10] = {
            0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54
        };
        std::copy(std::begin(kSrgbChroma), std::end(kSrgbChroma), edid.begin() + 25);

        // --- Установленные и стандартные тайминги ------------------------------
        // Обнуляем: виртуальный монитор не поддерживает legacy-режимы VESA,
        // весь список режимов приходит через IddCx, а не через EDID.
        edid[35] = 0x00; edid[36] = 0x00; edid[37] = 0x00;
        for (size_t i = 38; i < 54; i += 2)
        {
            edid[i] = 0x01; edid[i + 1] = 0x01;   // 0x0101 - «слот не используется»
        }

        // --- DTD #1: предпочтительный режим ------------------------------------
        {
            const uint32_t hActive = width;
            const uint32_t vActive = height;
            const uint32_t hTotal = hActive + kHBlank;
            const uint32_t vTotal = vActive + kVBlank;

            // Пиксельная частота в единицах 10 кГц; поле 16-битное, поэтому
            // ограничиваем сверху - иначе переполнение даст невалидный EDID.
            uint64_t pixelClock10kHz =
                (static_cast<uint64_t>(hTotal) * vTotal * refreshHz) / 10000ull;
            if (pixelClock10kHz > 65535ull) pixelClock10kHz = 65535ull;
            if (pixelClock10kHz == 0) pixelClock10kHz = 1;

            uint8_t* d = &edid[kOffsetDtd1];
            d[0] = static_cast<uint8_t>(pixelClock10kHz & 0xFF);
            d[1] = static_cast<uint8_t>((pixelClock10kHz >> 8) & 0xFF);

            // Байты 2..4 упаковывают младшие 8 бит hActive и hBlank,
            // а старшие 4 бита каждого значения - в общий байт 4.
            d[2] = static_cast<uint8_t>(hActive & 0xFF);
            d[3] = static_cast<uint8_t>(kHBlank & 0xFF);
            d[4] = static_cast<uint8_t>(((hActive >> 8) & 0x0F) << 4 | ((kHBlank >> 8) & 0x0F));

            d[5] = static_cast<uint8_t>(vActive & 0xFF);
            d[6] = static_cast<uint8_t>(kVBlank & 0xFF);
            d[7] = static_cast<uint8_t>(((vActive >> 8) & 0x0F) << 4 | ((kVBlank >> 8) & 0x0F));

            // Смещение и длительность синхроимпульсов. Значения взяты из
            // профиля reduced blanking: hsync offset 48, hsync width 32,
            // vsync offset 3, vsync width 10.
            const uint32_t hSyncOffset = 48, hSyncWidth = 32;
            const uint32_t vSyncOffset = 3, vSyncWidth = 10;

            d[8] = static_cast<uint8_t>(hSyncOffset & 0xFF);
            d[9] = static_cast<uint8_t>(hSyncWidth & 0xFF);
            d[10] = static_cast<uint8_t>(((vSyncOffset & 0x0F) << 4) | (vSyncWidth & 0x0F));
            // Байт 11 собирает старшие биты всех четырёх величин.
            d[11] = static_cast<uint8_t>(
                ((hSyncOffset >> 8) & 0x03) << 6 |
                ((hSyncWidth >> 8) & 0x03) << 4 |
                ((vSyncOffset >> 4) & 0x03) << 2 |
                ((vSyncWidth >> 4) & 0x03));

            d[12] = edid[21] * 10 % 256;                  // ширина изображения, мм
            d[13] = edid[22] * 10 % 256;                  // высота изображения, мм
            d[14] = 0x00;                                 // старшие биты размеров
            d[15] = 0x00;                                 // горизонтальная кайма
            d[16] = 0x00;                                 // вертикальная кайма
            // Бит 7 = 0 (не чересстрочный), биты 4..3 = 11 - цифровая
            // раздельная синхронизация, биты 2..1 - положительная полярность.
            d[17] = 0x1E;
        }

        // --- Дескриптор пределов частот (0xFD) ---------------------------------
        {
            uint8_t* d = &edid[kOffsetDesc2];
            d[0] = 0x00; d[1] = 0x00; d[2] = 0x00; d[3] = 0xFD; d[4] = 0x00;
            d[5] = 24;                                    // мин. вертикальная, Гц
            d[6] = static_cast<uint8_t>(std::min<uint32_t>(refreshHz + 15, 240));
            d[7] = 20;                                    // мин. горизонтальная, кГц
            d[8] = 200;                                   // макс. горизонтальная, кГц
            d[9] = 80;                                    // макс. пикс. частота / 10 МГц
            d[10] = 0x00;                                 // без GTF
            d[11] = 0x0A;
            for (size_t i = 12; i < 18; ++i) d[i] = 0x20;
        }

        // --- Имя монитора (0xFC) и заглушка (0x10) -----------------------------
        WriteTextDescriptor(&edid[kOffsetDesc3], 0xFC, "PhoneMonitor");
        WriteTextDescriptor(&edid[kOffsetDesc4], 0x10, "USB Display");

        edid[126] = 0x00;   // блоков-расширений нет

        // --- Контрольная сумма --------------------------------------------------
        // Сумма всех 128 байт по модулю 256 обязана быть нулевой.
        uint32_t sum = 0;
        for (size_t i = 0; i < kEdidSize - 1; ++i) sum += edid[i];
        edid[127] = static_cast<uint8_t>((256 - (sum % 256)) % 256);

        PM_LOG_INFO(L"Сгенерирован EDID для режима %ux%u@%u Гц, контрольная сумма 0x%02X",
            width, height, refreshHz, edid[127]);

        return edid;
    }

    // -------------------------------------------------------------------------
    void AlignForEncoder(uint32_t& width, uint32_t& height) noexcept
    {
        // Округление ВНИЗ до кратного 16. Вниз, а не вверх: расширение кадра
        // за пределы физического экрана телефона привело бы к обрезке картинки
        // на стороне клиента, а потеря нескольких строк незаметна.
        const uint32_t alignedW = width & ~15u;
        const uint32_t alignedH = height & ~15u;

        if (alignedW != width || alignedH != height)
        {
            PM_LOG_INFO(L"Разрешение выровнено под макроблоки H.264: %ux%u -> %ux%u",
                width, height, alignedW, alignedH);
        }

        // Нижняя граница: кодировщики отказываются работать с кадром мельче 64x64.
        width = (alignedW >= 64) ? alignedW : 64;
        height = (alignedH >= 64) ? alignedH : 64;
    }

    // -------------------------------------------------------------------------
    std::vector<MonitorMode> BuildModeTable(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept
    {
        std::vector<MonitorMode> modes;
        modes.reserve(8);

        if (refreshHz == 0) refreshHz = 60;

        // Первым - запрошенный режим: IddCx трактует первый элемент списка
        // как предпочтительный и именно его предложит менеджеру дисплеев.
        if (width >= 64 && height >= 64)
        {
            modes.push_back({ width, height, refreshHz });

            // Тот же кадр на 60 Гц - страховка на случай, если композитор
            // не примет нестандартную частоту телефона (90/120 Гц).
            if (refreshHz != 60)
            {
                modes.push_back({ width, height, 60 });
            }

            // Режим теплового троттлинга: 720p с сохранением пропорций.
            // Считаем ширину от высоты 720 и выравниваем по 16.
            uint32_t thermalH = 720;
            uint32_t thermalW = static_cast<uint32_t>(
                (static_cast<uint64_t>(width) * thermalH) / (height != 0 ? height : 1));
            AlignForEncoder(thermalW, thermalH);
            if (thermalW != width || thermalH != height)
            {
                modes.push_back({ thermalW, thermalH, 60 });
            }
        }

        // Запасные общеупотребимые режимы. Нужны, чтобы монитор оставался
        // работоспособным, даже если клиент прислал мусорную геометрию.
        static const MonitorMode kFallback[] = {
            { 1920, 1080, 60 },
            { 1280,  720, 60 },
            { 1024,  768, 60 },
        };

        for (const auto& fb : kFallback)
        {
            const bool duplicate = std::any_of(modes.begin(), modes.end(),
                [&fb](const MonitorMode& m) {
                    return m.width == fb.width && m.height == fb.height && m.refreshHz == fb.refreshHz;
                });
            if (!duplicate)
            {
                modes.push_back(fb);
            }
        }

        PM_LOG_INFO(L"Сформирована таблица из %zu режимов, предпочтительный %ux%u@%u Гц",
            modes.size(), modes[0].width, modes[0].height, modes[0].refreshHz);

        return modes;
    }

    // -------------------------------------------------------------------------
    void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& info,
                        uint32_t width, uint32_t height, uint32_t refreshHz,
                        bool isMonitorMode) noexcept
    {
        info.totalSize.cx = info.activeSize.cx = static_cast<LONG>(width);
        info.totalSize.cy = info.activeSize.cy = static_cast<LONG>(height);

        // Требование IddCx: у режима МОНИТОРА делитель vsync обязан быть 0,
        // у режима ЦЕЛИ - 1. Несоблюдение приводит к отказу CommitModes
        // с невнятным STATUS_INVALID_PARAMETER.
        info.AdditionalSignalInfo.vSyncFreqDivider = isMonitorMode ? 0 : 1;
        info.AdditionalSignalInfo.videoStandard = 255;   // «прочий» стандарт

        info.vSyncFreq.Numerator = refreshHz;
        info.vSyncFreq.Denominator = 1;
        info.hSyncFreq.Numerator = refreshHz * height;
        info.hSyncFreq.Denominator = 1;

        info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
        info.pixelRate = static_cast<UINT64>(refreshHz) *
                         static_cast<UINT64>(width) *
                         static_cast<UINT64>(height);
    }

    IDDCX_MONITOR_MODE CreateMonitorMode(uint32_t width, uint32_t height, uint32_t refreshHz,
                                         IDDCX_MONITOR_MODE_ORIGIN origin) noexcept
    {
        IDDCX_MONITOR_MODE mode{};
        mode.Size = sizeof(mode);
        mode.Origin = origin;
        FillSignalInfo(mode.MonitorVideoSignalInfo, width, height, refreshHz, true);
        return mode;
    }

    IDDCX_TARGET_MODE CreateTargetMode(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept
    {
        IDDCX_TARGET_MODE mode{};
        mode.Size = sizeof(mode);
        FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo,
                       width, height, refreshHz, false);
        return mode;
    }
}
