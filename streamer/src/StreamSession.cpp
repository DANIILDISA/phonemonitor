#include "StreamSession.h"
#include "Log.h"

#include <avrt.h>
#include <algorithm>

namespace pm
{
    namespace
    {
        inline uint64_t Qpc() noexcept
        {
            LARGE_INTEGER c{};
            QueryPerformanceCounter(&c);
            return static_cast<uint64_t>(c.QuadPart);
        }

        inline uint64_t QpcFrequency() noexcept
        {
            static const uint64_t freq = [] {
                LARGE_INTEGER f{};
                QueryPerformanceFrequency(&f);
                return static_cast<uint64_t>(f.QuadPart);
            }();
            return freq;
        }

        inline uint64_t QpcToMicroseconds(uint64_t ticks) noexcept
        {
            return (ticks * 1'000'000ull) / QpcFrequency();
        }

        /* Выравнивание вниз до кратного 16 (макроблок H.264). */
        inline uint32_t AlignDown16(uint32_t value) noexcept
        {
            const uint32_t aligned = value & ~15u;
            return (aligned >= 64) ? aligned : 64;
        }
    }

    // -------------------------------------------------------------------------
    StreamSession::StreamSession(const AppConfig& config) noexcept
        : m_config(config)
    {
    }

    StreamSession::~StreamSession() noexcept
    {
        TeardownSession();
    }

    void StreamSession::RequestStop() noexcept
    {
        m_stopRequested.store(true, std::memory_order_release);
    }

    void StreamSession::EmergencyCleanup() noexcept
    {
        // Вызывается из обработчика консольных сигналов, где времени мало
        // и большинство операций небезопасно. Делаем только то, что обязано
        // быть сделано: снимаем форвард, иначе он останется в adb навсегда.
        m_adb.RemoveForward();
    }

    // -------------------------------------------------------------------------
    //  Инициализация
    // -------------------------------------------------------------------------
    bool StreamSession::SetupAdb() noexcept
    {
        if (!m_config.manageAdbForward)
        {
            PM_INFO("Управление adb отключено ключом --no-adb. "
                    "Форвард tcp:%u должен быть создан заранее", m_config.port);
            return true;
        }

        if (!m_adb.Locate(m_config.adbPath))
        {
            return false;
        }

        m_adb.StartServer();

        const std::wstring serial = m_adb.WaitForDevice(m_config.deviceWaitMs);
        if (serial.empty())
        {
            PM_ERROR("Устройство не готово. Проверьте кабель, включите отладку по USB "
                     "и подтвердите доверие к этому компьютеру на экране телефона");
            return false;
        }

        // Порты одинаковы по обе стороны: так проще диагностировать, и это
        // прямо соответствует команде из спецификации проекта.
        return m_adb.CreateForward(m_config.port, m_config.port, serial);
    }

    bool StreamSession::ConnectToDriver() noexcept
    {
        PM_INFO("Подключение к драйверу виртуального дисплея...");

        const HRESULT hr = m_ipc.Connect(m_config.driverWaitMs);
        if (FAILED(hr))
        {
            PM_ERROR("Драйвер недоступен: %s", HResultToText(hr));
            PM_ERROR("Установите драйвер PhoneMonitor IDD (см. docs/BUILD.md) "
                     "и убедитесь, что устройство отображается в Диспетчере устройств");
            return false;
        }

        uint32_t width = 0, height = 0, refresh = 0;
        m_ipc.GetActiveMode(width, height, refresh);
        PM_INFO("Драйвер подключён. Текущий режим виртуального монитора: %ux%u@%u Гц",
            width, height, refresh);

        return true;
    }

    bool StreamSession::ConnectToClient() noexcept
    {
        // Колбэки ставим до подключения: HELLO может прийти сразу же после
        // установления соединения, и потерять его нельзя.
        TransportCallbacks callbacks;

        callbacks.onHello = [this](const PmMsgHello& hello)
        {
            m_clientWidth.store(hello.width, std::memory_order_release);
            m_clientHeight.store(hello.height, std::memory_order_release);

            // Частота приходит в миллигерцах; телефоны с 90/120 Гц округляем
            // вниз до целого - дробная частота драйверу не нужна.
            const uint32_t hz = hello.refreshMilliHz / 1000;
            m_clientRefreshHz.store((hz >= 24 && hz <= 240) ? hz : 60, std::memory_order_release);

            m_maxDecoderWidth.store(hello.maxDecoderW, std::memory_order_release);
            m_maxDecoderHeight.store(hello.maxDecoderH, std::memory_order_release);

            m_helloArrived.store(true, std::memory_order_release);
        };

        callbacks.onThermal = [this](const PmMsgThermal& thermal)
        {
            const int32_t previous = m_thermalStatus.exchange(
                thermal.thermalStatus, std::memory_order_acq_rel);
            if (previous != thermal.thermalStatus)
            {
                m_thermalChanged.store(true, std::memory_order_release);
            }
        };

        callbacks.onKeyFrameRequest = [this]()
        {
            m_keyFrameRequested.store(true, std::memory_order_release);
        };

        callbacks.onDisconnect = [this]()
        {
            m_clientDisconnected.store(true, std::memory_order_release);
        };

        m_transport.SetCallbacks(std::move(callbacks));

        if (!m_transport.Connect(m_config.port, m_config.connectTimeoutMs))
        {
            return false;
        }

        m_transport.StartReceiver();
        return true;
    }

    bool StreamSession::WaitForHello(uint32_t timeoutMs) noexcept
    {
        PM_INFO("Ожидание пакета HELLO с геометрией экрана телефона...");

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!m_helloArrived.load(std::memory_order_acquire))
        {
            if (m_stopRequested.load(std::memory_order_acquire))
            {
                return false;
            }
            if (m_clientDisconnected.load(std::memory_order_acquire))
            {
                PM_ERROR("Клиент отключился, не прислав HELLO");
                return false;
            }
            if (GetTickCount64() >= deadline)
            {
                PM_ERROR("Клиент не прислал HELLO за %u мс. "
                         "Проверьте версию приложения на телефоне", timeoutMs);
                return false;
            }
            Sleep(20);
        }
        return true;
    }

    // -------------------------------------------------------------------------
    //  Согласование разрешения
    // -------------------------------------------------------------------------
    bool StreamSession::NegotiateResolution() noexcept
    {
        uint32_t width = m_clientWidth.load(std::memory_order_acquire);
        uint32_t height = m_clientHeight.load(std::memory_order_acquire);
        const uint32_t refresh = m_clientRefreshHz.load(std::memory_order_acquire);

        // ---------------------------------------------------------------------
        // Шаг 1. Ограничение возможностями аппаратного декодера телефона.
        //
        // Физическое разрешение экрана и предел декодера - разные вещи. Дешёвые
        // устройства с экраном 2400x1080 нередко имеют декодер, ограниченный
        // 1920x1080. Превышение предела даёт либо отказ MediaCodec, либо
        // молчаливый переход на программный декодер с задержкой в сотни мс.
        // ---------------------------------------------------------------------
        const uint32_t maxW = m_maxDecoderWidth.load(std::memory_order_acquire);
        const uint32_t maxH = m_maxDecoderHeight.load(std::memory_order_acquire);

        if (maxW > 0 && maxH > 0 && (width > maxW || height > maxH))
        {
            // Масштабируем с сохранением пропорций: искажённая картинка на
            // вторичном мониторе хуже, чем слегка пониженное разрешение.
            const double scaleX = static_cast<double>(maxW) / width;
            const double scaleY = static_cast<double>(maxH) / height;
            const double scale = (std::min)(scaleX, scaleY);

            const uint32_t scaledW = static_cast<uint32_t>(width * scale);
            const uint32_t scaledH = static_cast<uint32_t>(height * scale);

            PM_WARN("Экран %ux%u превышает возможности декодера (%ux%u). "
                    "Понижаем до %ux%u с сохранением пропорций",
                width, height, maxW, maxH, scaledW, scaledH);

            width = scaledW;
            height = scaledH;
        }

        // Шаг 2. Выравнивание под макроблоки.
        const uint32_t alignedW = AlignDown16(width);
        const uint32_t alignedH = AlignDown16(height);

        if (alignedW != width || alignedH != height)
        {
            PM_INFO("Разрешение выровнено под макроблоки H.264: %ux%u -> %ux%u",
                width, height, alignedW, alignedH);
        }

        // Шаг 3. Запрос режима у драйвера и ожидание подтверждения.
        // 8 секунд с запасом: путь departure -> arrival занимает до секунды,
        // плюс время на перестроение рабочего стола менеджером дисплеев.
        const HRESULT hr = m_ipc.RequestMode(alignedW, alignedH, refresh, 8000);
        if (FAILED(hr))
        {
            PM_ERROR("Драйвер не смог применить режим %ux%u@%u Гц: %s",
                alignedW, alignedH, refresh, HResultToText(hr));
            return false;
        }

        // Шаг 4. Сверка с тем, что реально установила система.
        uint32_t activeW = 0, activeH = 0, activeHz = 0;
        m_ipc.GetActiveMode(activeW, activeH, activeHz);

        if (activeW != alignedW || activeH != alignedH)
        {
            PM_WARN("Драйвер установил %ux%u вместо запрошенных %ux%u - "
                    "кодируем фактический размер",
                activeW, activeH, alignedW, alignedH);
        }

        m_encodeWidth = (activeW != 0) ? activeW : alignedW;
        m_encodeHeight = (activeH != 0) ? activeH : alignedH;
        m_encodeBitrate = m_config.bitrateBps;

        PM_INFO("Согласованный режим передачи: %ux%u@%u Гц, %u кбит/с",
            m_encodeWidth, m_encodeHeight, refresh, m_encodeBitrate / 1000);

        return true;
    }

    // -------------------------------------------------------------------------
    bool StreamSession::CreateEncoder(uint32_t width, uint32_t height, uint32_t bitrateBps) noexcept
    {
        // Кодировщик должен работать на ТОМ ЖЕ устройстве D3D11, где лежат
        // принятые кадры: иначе потребуется копия через системную память.
        ID3D11Device* device = m_ipc.Device();
        if (device == nullptr)
        {
            PM_ERROR("Устройство D3D11 ещё не готово - кодировщик создать нельзя");
            return false;
        }

        EncoderConfig encoderConfig;
        encoderConfig.device = device;
        encoderConfig.width = width;
        encoderConfig.height = height;
        encoderConfig.fps = m_config.fps;
        encoderConfig.bitrateBps = bitrateBps;
        encoderConfig.intraRefreshEnabled = m_config.intraRefresh;
        encoderConfig.intraRefreshBandPercent = m_config.intraRefreshPercent;
        encoderConfig.lowLatencyPreset = true;
        encoderConfig.cbrRateControl = true;
        encoderConfig.repeatSpsPps = true;

        m_encoder = CreateVideoEncoder(encoderConfig, m_config.encoderBackend.c_str());
        if (!m_encoder)
        {
            return false;
        }

        m_encodeWidth = width;
        m_encodeHeight = height;
        m_encodeBitrate = bitrateBps;

        // Первый кадр после создания кодировщика обязан быть ключевым:
        // декодер на телефоне не имеет опорных кадров.
        m_keyFrameRequested.store(true, std::memory_order_release);

        return true;
    }

    // -------------------------------------------------------------------------
    //  Главный цикл
    // -------------------------------------------------------------------------
    int StreamSession::Run() noexcept
    {
        if (!ConnectToDriver())
        {
            return 2;
        }

        int exitCode = 0;

        do
        {
            if (!SetupAdb())
            {
                return 3;
            }

            if (!ConnectToClient())
            {
                return 4;
            }

            if (!WaitForHello(30000))
            {
                TeardownSession();
                if (!m_config.autoReconnect) return 5;
                continue;
            }

            if (!NegotiateResolution())
            {
                TeardownSession();
                if (!m_config.autoReconnect) return 6;
                continue;
            }

            // Кодировщик создаём после первого принятого кадра: только тогда
            // устройство D3D11 гарантированно существует (оно определяется по
            // общим текстурам драйвера).
            exitCode = StreamLoop();

            TeardownSession();

            if (m_stopRequested.load(std::memory_order_acquire) || !m_config.autoReconnect)
            {
                break;
            }

            PM_INFO("Соединение разорвано. Переподключение через 2 секунды...");
            Sleep(2000);

        } while (!m_stopRequested.load(std::memory_order_acquire));

        return exitCode;
    }

    int StreamSession::StreamLoop() noexcept
    {
        PM_INFO("Запуск конвейера передачи");

        // -------------------------------------------------------------------
        // Приоритет потока.
        //
        // Конвейер «кадр -> кодирование -> сокет» чувствителен к вытеснению:
        // каждый пропущенный квант - это пропущенный кадр. Класс MMCSS
        // Distribution даёт гарантированное время процессора, не требуя прав
        // администратора (в отличие от REALTIME_PRIORITY_CLASS).
        // -------------------------------------------------------------------
        DWORD taskIndex = 0;
        HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);
        if (avrtHandle != nullptr)
        {
            AvSetMmThreadPriority(avrtHandle, AVRT_PRIORITY_HIGH);
            PM_INFO("Поток передачи переведён в класс MMCSS Distribution");
        }
        else
        {
            PM_WARN("MMCSS недоступен (код %lu) - возможны колебания задержки", GetLastError());
        }

        m_statsWindowStartQpc = Qpc();
        m_framesAtWindowStart = 0;
        m_framesEncoded = m_framesSent = m_framesSkipped = 0;
        m_bytesEncoded = m_totalLatencyUs = m_maxLatencyUs = m_totalEncodeUs = 0;

        EncodedFrame encoded;
        int result = 0;

        while (!m_stopRequested.load(std::memory_order_acquire))
        {
            if (m_clientDisconnected.load(std::memory_order_acquire))
            {
                PM_INFO("Клиент отключился - завершаем конвейер");
                break;
            }

            ProcessPendingEvents();

            // ---------------------------------------------------------------
            // Шаг 1. Получение кадра от драйвера.
            //
            // Таймаут 250 мс: при статичном рабочем столе кадров нет вовсе,
            // и цикл просто периодически просыпается, чтобы проверить события.
            // ---------------------------------------------------------------
            AcquiredFrame frame;
            const HRESULT acquireResult = m_ipc.AcquireFrame(250, frame);

            if (acquireResult == S_FALSE)
            {
                continue;   // новых кадров нет - это норма
            }
            if (FAILED(acquireResult))
            {
                PM_ERROR("Получение кадра от драйвера не удалось: %s",
                    HResultToText(acquireResult));
                result = 7;
                break;
            }

            // ---------------------------------------------------------------
            // Шаг 2. Ленивое создание кодировщика и реакция на смену геометрии.
            //
            // Драйвер мог переключить режим (например, пользователь изменил
            // разрешение в параметрах Windows). Размер кадра - источник истины.
            // ---------------------------------------------------------------
            if (!m_encoder || frame.width != m_encodeWidth || frame.height != m_encodeHeight)
            {
                if (m_encoder)
                {
                    PM_INFO("Геометрия кадра изменилась: %ux%u -> %ux%u, пересоздаём кодировщик",
                        m_encodeWidth, m_encodeHeight, frame.width, frame.height);

                    // Сначала пробуем переконфигурировать без пересоздания -
                    // это на порядок быстрее и не прерывает поток.
                    const HRESULT reconfigured = m_encoder->Reconfigure(
                        frame.width, frame.height, m_encodeBitrate);

                    if (reconfigured == S_OK)
                    {
                        m_encodeWidth = frame.width;
                        m_encodeHeight = frame.height;
                        m_keyFrameRequested.store(true, std::memory_order_release);
                        continue;
                    }

                    m_encoder->Shutdown();
                    m_encoder.reset();
                }

                if (!CreateEncoder(frame.width, frame.height, m_encodeBitrate))
                {
                    result = 8;
                    break;
                }
                // Первый кадр после создания пропускаем: кодировщик уже получил
                // корректный размер, но кадр в руках может быть от старого режима.
                continue;
            }

            // ---------------------------------------------------------------
            // Шаг 3. Кодирование.
            // ---------------------------------------------------------------
            const bool forceKeyFrame = m_keyFrameRequested.exchange(false, std::memory_order_acq_rel);

            const HRESULT encodeResult = m_encoder->EncodeFrame(
                frame.texture, forceKeyFrame, frame.presentQpc, encoded);

            if (FAILED(encodeResult))
            {
                PM_ERROR("Кодирование кадра не удалось: %s", HResultToText(encodeResult));
                result = 9;
                break;
            }

            m_ipc.ReleaseFrame(frame.frameIndex);

            if (encodeResult == S_FALSE || encoded.data.empty())
            {
                // Кодировщик не отдал пакет на этом кадре - штатно для первых
                // кадров некоторых MFT.
                ++m_framesSkipped;
                continue;
            }

            ++m_framesEncoded;
            m_bytesEncoded += encoded.data.size();
            m_totalEncodeUs += encoded.encodeTimeUs;

            // ---------------------------------------------------------------
            // Шаг 4. Отправка в сокет.
            //
            // Поток H.264 уходит как есть, в формате Annex B. Никакой обёртки:
            // клиент синхронизируется по стартовым кодам.
            // ---------------------------------------------------------------
            if (!m_transport.SendAll(encoded.data.data(), encoded.data.size()))
            {
                if (!m_transport.IsConnected())
                {
                    PM_WARN("Соединение с клиентом потеряно при отправке кадра");
                    break;
                }
                // Таймаут отправки при живом соединении: телефон не успевает.
                // Кадр теряем и запрашиваем ключевой, чтобы декодер не остался
                // с повреждённой цепочкой опорных кадров.
                ++m_framesSkipped;
                m_keyFrameRequested.store(true, std::memory_order_release);
                continue;
            }

            ++m_framesSent;

            // ---------------------------------------------------------------
            // Шаг 5. Измерение сквозной задержки.
            //
            // presentQpc - момент, когда драйвер опубликовал кадр. Разница с
            // текущим временем показывает всё, что произошло с кадром на
            // стороне Windows: IPC, ожидание фенса, копирование, кодирование
            // и передачу в сокет. Не учитывается только время декодирования
            // на телефоне - его клиент присылает отдельно в PM_MSG_STATS.
            // ---------------------------------------------------------------
            if (frame.presentQpc != 0)
            {
                const uint64_t latencyUs = QpcToMicroseconds(Qpc() - frame.presentQpc);
                m_totalLatencyUs += latencyUs;
                m_maxLatencyUs = (std::max)(m_maxLatencyUs, latencyUs);
            }

            if (m_framesEncoded % m_config.statsIntervalFrames == 0)
            {
                LogStatistics(m_framesEncoded);
            }
        }

        if (avrtHandle != nullptr)
        {
            AvRevertMmThreadCharacteristics(avrtHandle);
        }

        LogStatistics(m_framesEncoded);
        PM_INFO("Конвейер передачи остановлен");
        return result;
    }

    // -------------------------------------------------------------------------
    void StreamSession::ProcessPendingEvents() noexcept
    {
        if (m_thermalChanged.exchange(false, std::memory_order_acq_rel))
        {
            ApplyThermalState();
        }
    }

    void StreamSession::ApplyThermalState() noexcept
    {
        if (!m_config.thermalMitigation)
        {
            return;
        }

        const int32_t status = m_thermalStatus.load(std::memory_order_acquire);

        // ---------------------------------------------------------------------
        // Порог срабатывания - THERMAL_STATUS_LIGHT.
        //
        // Это самый ранний сигнал Android о том, что устройство начало греться.
        // Реагировать на него выгодно: снижение нагрузки на раннем этапе
        // предотвращает жёсткий троттлинг, при котором декодер начинает
        // пропускать кадры и задержка растёт лавинообразно.
        // ---------------------------------------------------------------------
        const bool shouldThrottle = (status >= PM_THERMAL_LIGHT);
        const bool currentlyThrottled = m_thermalActive.load(std::memory_order_acquire);

        if (shouldThrottle == currentlyThrottled)
        {
            return;   // состояние не изменилось
        }

        if (shouldThrottle)
        {
            // Целевая высота из конфигурации (по умолчанию 720), ширина -
            // из пропорций исходного экрана.
            const uint32_t sourceW = m_clientWidth.load(std::memory_order_acquire);
            const uint32_t sourceH = m_clientHeight.load(std::memory_order_acquire);

            uint32_t targetH = m_config.thermalHeight;
            uint32_t targetW = (sourceH != 0)
                ? static_cast<uint32_t>((static_cast<uint64_t>(sourceW) * targetH) / sourceH)
                : 1280;

            targetW = AlignDown16(targetW);
            targetH = AlignDown16(targetH);

            PM_WARN("Телефон сообщил о нагреве (уровень %d). "
                    "Переходим в режим экономии: %ux%u, %u кбит/с",
                status, targetW, targetH, m_config.thermalBitrateBps / 1000);

            // Флаг для драйвера - он может использовать его для собственных
            // оптимизаций (например, снизить частоту публикации кадров).
            m_ipc.SetQualityFlags(PM_QF_THERMAL_SAVE);

            const HRESULT hr = m_ipc.RequestMode(
                targetW, targetH, m_clientRefreshHz.load(std::memory_order_acquire), 8000);

            if (FAILED(hr))
            {
                PM_ERROR("Не удалось переключить драйвер в режим экономии: %s", HResultToText(hr));
                return;
            }

            m_encodeBitrate = m_config.thermalBitrateBps;
            m_thermalActive.store(true, std::memory_order_release);
        }
        else
        {
            PM_INFO("Тепловое состояние телефона нормализовалось - возвращаем полное разрешение");

            m_ipc.SetQualityFlags(0);

            const HRESULT hr = m_ipc.RequestMode(
                AlignDown16(m_clientWidth.load(std::memory_order_acquire)),
                AlignDown16(m_clientHeight.load(std::memory_order_acquire)),
                m_clientRefreshHz.load(std::memory_order_acquire), 8000);

            if (FAILED(hr))
            {
                PM_ERROR("Не удалось вернуть полное разрешение: %s", HResultToText(hr));
                return;
            }

            m_encodeBitrate = m_config.bitrateBps;
            m_thermalActive.store(false, std::memory_order_release);
        }

        // Кодировщик будет пересоздан в главном цикле, когда придёт кадр
        // нового размера - отдельного действия здесь не требуется.
        m_keyFrameRequested.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    void StreamSession::LogStatistics(uint64_t frameCount) noexcept
    {
        if (frameCount == 0)
        {
            return;
        }

        const uint64_t now = Qpc();
        const uint64_t windowUs = QpcToMicroseconds(now - m_statsWindowStartQpc);
        const double windowSeconds = static_cast<double>(windowUs) / 1'000'000.0;

        const double averageLatencyMs = (m_framesSent > 0)
            ? static_cast<double>(m_totalLatencyUs) / m_framesSent / 1000.0
            : 0.0;
        const double averageEncodeMs = static_cast<double>(m_totalEncodeUs) / frameCount / 1000.0;
        const double megabitsPerSecond = (windowSeconds > 0.0)
            ? (static_cast<double>(m_bytesEncoded) * 8.0) / windowSeconds / 1'000'000.0
            : 0.0;
        // Частота считается по кадрам ЗА ОКНО, а не по накопительному счётчику.
        const uint64_t framesInWindow = (frameCount >= m_framesAtWindowStart)
            ? (frameCount - m_framesAtWindowStart)
            : frameCount;
        const double actualFps = (windowSeconds > 0.0)
            ? static_cast<double>(framesInWindow) / windowSeconds
            : 0.0;

        PM_INFO("Сводка: кадров %llu (отправлено %llu, пропущено %llu), %.1f к/с, "
                "%.2f Мбит/с | задержка Windows: средняя %.2f мс, пик %.2f мс | "
                "кодирование %.2f мс | потери в драйвере: %u",
            static_cast<unsigned long long>(frameCount),
            static_cast<unsigned long long>(m_framesSent),
            static_cast<unsigned long long>(m_framesSkipped),
            actualFps, megabitsPerSecond,
            averageLatencyMs,
            static_cast<double>(m_maxLatencyUs) / 1000.0,
            averageEncodeMs,
            m_ipc.DroppedFrames());

        // Сбрасываем окно, но не общие счётчики кадров: так видно и текущую
        // динамику, и суммарный объём работы.
        m_statsWindowStartQpc = now;
        m_framesAtWindowStart = frameCount;
        m_bytesEncoded = 0;
        m_maxLatencyUs = 0;
    }

    // -------------------------------------------------------------------------
    void StreamSession::TeardownSession() noexcept
    {
        if (m_encoder)
        {
            m_encoder->Shutdown();
            m_encoder.reset();
        }

        m_transport.Disconnect();

        // Сброс флагов сессии: при переподключении всё начинается заново.
        m_helloArrived.store(false, std::memory_order_release);
        m_clientDisconnected.store(false, std::memory_order_release);
        m_keyFrameRequested.store(false, std::memory_order_release);
        m_thermalChanged.store(false, std::memory_order_release);
        m_thermalStatus.store(PM_THERMAL_NONE, std::memory_order_release);
        m_thermalActive.store(false, std::memory_order_release);

        m_encodeWidth = m_encodeHeight = 0;
    }
}
