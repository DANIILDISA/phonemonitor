#include "TcpTransport.h"
#include "../Log.h"

#include <cstring>

namespace pm
{
    bool TcpTransport::s_winsockReady = false;

    namespace
    {
        /* Преобразование big-endian значений протокола в порядок хоста.
           Пишем вручную, а не через ntohl: так явно видно, где именно
           происходит смена порядка байт. */
        inline uint32_t ReadBe32(const uint8_t* p) noexcept
        {
            return (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                   static_cast<uint32_t>(p[3]);
        }

        inline uint16_t ReadBe16(const uint8_t* p) noexcept
        {
            return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
        }

        inline int32_t ReadBe32s(const uint8_t* p) noexcept
        {
            return static_cast<int32_t>(ReadBe32(p));
        }

        const char* ThermalName(int32_t status) noexcept
        {
            switch (status)
            {
            case PM_THERMAL_NONE:      return "норма";
            case PM_THERMAL_LIGHT:     return "лёгкий нагрев";
            case PM_THERMAL_MODERATE:  return "умеренный нагрев";
            case PM_THERMAL_SEVERE:    return "сильный нагрев";
            case PM_THERMAL_CRITICAL:  return "критический нагрев";
            case PM_THERMAL_EMERGENCY: return "аварийный нагрев";
            case PM_THERMAL_SHUTDOWN:  return "перегрев, выключение";
            default:                   return "неизвестно";
            }
        }
    }

    // -------------------------------------------------------------------------
    TcpTransport::TcpTransport() noexcept = default;

    TcpTransport::~TcpTransport() noexcept
    {
        Disconnect();
    }

    bool TcpTransport::GlobalInit() noexcept
    {
        if (s_winsockReady)
        {
            return true;
        }

        WSADATA wsaData{};
        const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0)
        {
            PM_ERROR("WSAStartup не удался, код %d", result);
            return false;
        }

        s_winsockReady = true;
        PM_TRACE("Winsock 2.2 инициализирован");
        return true;
    }

    void TcpTransport::GlobalShutdown() noexcept
    {
        if (s_winsockReady)
        {
            WSACleanup();
            s_winsockReady = false;
        }
    }

    // -------------------------------------------------------------------------
    void TcpTransport::ApplySocketOptions() noexcept
    {
        // --- Отключение алгоритма Нейгла ---------------------------------------
        // Самая важная настройка. Нейгл придерживает мелкие сегменты, пока не
        // придёт подтверждение предыдущих; для видеопотока с мелкими NAL это
        // означает до 200 мс задержки на ровном месте.
        BOOL noDelay = TRUE;
        if (setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) != 0)
        {
            PM_WARN("Не удалось отключить алгоритм Нейгла, код %d", WSAGetLastError());
        }
        else
        {
            PM_TRACE("TCP_NODELAY включён");
        }

        // --- Размер буфера отправки ---------------------------------------------
        // Примерно два кадра при 15 Мбит/с и 60 Гц: 15e6/60/8 * 2 ≈ 62 КиБ.
        // Намеренно небольшой: большой буфер прячет затор и превращает его
        // в скрытую задержку вместо честной блокировки отправки.
        int sendBuffer = 64 * 1024;
        if (setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF,
                reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer)) != 0)
        {
            PM_WARN("Не удалось задать размер буфера отправки, код %d", WSAGetLastError());
        }

        // --- Таймаут отправки ----------------------------------------------------
        // Без него зависший телефон заблокировал бы поток кодирования навсегда.
        DWORD sendTimeout = 2000;
        setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));

        DWORD receiveTimeout = 1000;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));

        // --- Keep-alive -----------------------------------------------------------
        // Обрыв USB-кабеля не всегда приводит к немедленному закрытию сокета:
        // adb может держать соединение открытым. Keep-alive позволяет заметить
        // мёртвое соединение за разумное время.
        BOOL keepAlive = TRUE;
        setsockopt(m_socket, SOL_SOCKET, SO_KEEPALIVE,
            reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
    }

    bool TcpTransport::Connect(uint16_t port, uint32_t totalTimeoutMs) noexcept
    {
        if (!GlobalInit())
        {
            return false;
        }

        Disconnect();
        m_stopping.store(false, std::memory_order_release);

        const ULONGLONG deadline = GetTickCount64() + totalTimeoutMs;
        bool reportedWaiting = false;

        for (;;)
        {
            m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_socket == INVALID_SOCKET)
            {
                PM_ERROR("Не удалось создать сокет, код %d", WSAGetLastError());
                return false;
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            // Только петлевой интерфейс: соединение наружу не выходит,
            // его подхватывает adb.
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

            if (connect(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
            {
                ApplySocketOptions();
                m_connected.store(true, std::memory_order_release);
                m_bytesSent.store(0, std::memory_order_relaxed);
                m_helloReceived.store(false, std::memory_order_release);

                PM_INFO("Соединение с клиентом установлено через 127.0.0.1:%u", port);
                return true;
            }

            const int error = WSAGetLastError();
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;

            if (GetTickCount64() >= deadline)
            {
                PM_ERROR("Не удалось подключиться к 127.0.0.1:%u за %u мс (последняя ошибка %d). "
                         "Убедитесь, что приложение PhoneMonitor запущено на телефоне",
                    port, totalTimeoutMs, error);
                return false;
            }

            if (!reportedWaiting)
            {
                PM_INFO("Ожидание запуска приложения на телефоне (порт %u)...", port);
                reportedWaiting = true;
            }

            // Пауза между попытками. 400 мс - компромисс: быстрее реагируем на
            // появление клиента, но не забиваем журнал adb отказами.
            Sleep(400);
        }
    }

    void TcpTransport::Disconnect() noexcept
    {
        m_stopping.store(true, std::memory_order_release);

        if (m_socket != INVALID_SOCKET)
        {
            // shutdown до closesocket: он разблокирует recv в потоке чтения,
            // иначе join повиснет до истечения SO_RCVTIMEO.
            shutdown(m_socket, SD_BOTH);
        }

        if (m_receiverThread.joinable())
        {
            m_receiverThread.join();
        }

        if (m_socket != INVALID_SOCKET)
        {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }

        if (m_connected.exchange(false, std::memory_order_acq_rel))
        {
            PM_INFO("Соединение закрыто. Отправлено %llu КиБ",
                static_cast<unsigned long long>(m_bytesSent.load() / 1024));
        }
    }

    void TcpTransport::SetCallbacks(TransportCallbacks callbacks) noexcept
    {
        m_callbacks = std::move(callbacks);
    }

    void TcpTransport::StartReceiver() noexcept
    {
        if (m_receiverThread.joinable())
        {
            return;
        }
        m_receiverThread = std::thread(&TcpTransport::ReceiverLoop, this);
    }

    // -------------------------------------------------------------------------
    bool TcpTransport::SendAll(const uint8_t* data, size_t size) noexcept
    {
        if (!IsConnected() || data == nullptr || size == 0)
        {
            return false;
        }

        size_t offset = 0;
        while (offset < size)
        {
            // send может принять меньше запрошенного - это нормальное поведение
            // TCP при заполненном буфере отправки. Поэтому цикл обязателен.
            const int chunk = static_cast<int>(
                (size - offset > 1 << 20) ? (1 << 20) : (size - offset));

            const int sent = send(m_socket,
                reinterpret_cast<const char*>(data + offset), chunk, 0);

            if (sent == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();

                if (error == WSAEWOULDBLOCK)
                {
                    // Сокет блокирующий, так что сюда попасть не должны, но
                    // если случилось - уступаем квант и повторяем.
                    SwitchToThread();
                    continue;
                }

                if (error == WSAETIMEDOUT)
                {
                    PM_WARN("Таймаут отправки: телефон не успевает принимать поток. Кадр отброшен");
                    return false;
                }

                PM_ERROR("Ошибка отправки в сокет, код %d - соединение потеряно", error);
                m_connected.store(false, std::memory_order_release);
                return false;
            }

            if (sent == 0)
            {
                PM_WARN("Удалённая сторона закрыла соединение при отправке");
                m_connected.store(false, std::memory_order_release);
                return false;
            }

            offset += static_cast<size_t>(sent);
        }

        m_bytesSent.fetch_add(size, std::memory_order_relaxed);
        return true;
    }

    // -------------------------------------------------------------------------
    bool TcpTransport::ReceiveExact(uint8_t* buffer, size_t size) noexcept
    {
        size_t offset = 0;
        while (offset < size)
        {
            if (m_stopping.load(std::memory_order_acquire))
            {
                return false;
            }

            const int received = recv(m_socket,
                reinterpret_cast<char*>(buffer + offset),
                static_cast<int>(size - offset), 0);

            if (received == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                if (error == WSAETIMEDOUT)
                {
                    // Истёк SO_RCVTIMEO - управляющих сообщений просто нет.
                    // Продолжаем ждать: это штатная ситуация.
                    continue;
                }
                if (!m_stopping.load(std::memory_order_acquire))
                {
                    PM_WARN("Ошибка чтения управляющего канала, код %d", error);
                }
                return false;
            }

            if (received == 0)
            {
                // Корректное закрытие соединения удалённой стороной.
                return false;
            }

            offset += static_cast<size_t>(received);
        }
        return true;
    }

    void TcpTransport::ReceiverLoop() noexcept
    {
        PM_INFO("Поток чтения управляющего канала запущен");

        std::vector<uint8_t> payload;

        while (!m_stopping.load(std::memory_order_acquire))
        {
            uint8_t headerBytes[sizeof(PmCtrlHeader)];
            if (!ReceiveExact(headerBytes, sizeof(headerBytes)))
            {
                break;
            }

            PmCtrlHeader header{};
            header.magic = ReadBe32(&headerBytes[0]);
            header.version = ReadBe16(&headerBytes[4]);
            header.type = ReadBe16(&headerBytes[6]);
            header.payloadLen = ReadBe32(&headerBytes[8]);

            // ---------------------------------------------------------------
            // Защита от рассинхронизации.
            //
            // Если сигнатура не совпала, поток управляющих сообщений сбит.
            // Восстановиться корректно нельзя - в отличие от видеопотока,
            // здесь нет стартовых кодов для повторной синхронизации.
            // Поэтому рвём соединение: клиент переподключится и начнёт с HELLO.
            // ---------------------------------------------------------------
            if (header.magic != PM_CTRL_MAGIC)
            {
                PM_ERROR("Управляющий канал рассинхронизирован: сигнатура 0x%08X вместо 0x%08X",
                    header.magic, PM_CTRL_MAGIC);
                break;
            }

            if (header.version != PM_PROTO_VERSION)
            {
                PM_ERROR("Версия протокола клиента (%u) не совпадает с версией стримера (%u)",
                    header.version, PM_PROTO_VERSION);
                break;
            }

            if (header.payloadLen > PM_CTRL_MAX_PAYLOAD)
            {
                PM_ERROR("Недопустимый размер полезной нагрузки: %u байт", header.payloadLen);
                break;
            }

            payload.resize(header.payloadLen);
            if (header.payloadLen > 0 && !ReceiveExact(payload.data(), header.payloadLen))
            {
                break;
            }

            if (!ProcessControlMessage(header, payload))
            {
                break;
            }
        }

        m_connected.store(false, std::memory_order_release);

        if (m_callbacks.onDisconnect)
        {
            m_callbacks.onDisconnect();
        }

        PM_INFO("Поток чтения управляющего канала завершён");
    }

    bool TcpTransport::ProcessControlMessage(const PmCtrlHeader& header,
                                             const std::vector<uint8_t>& payload) noexcept
    {
        switch (header.type)
        {
        case PM_MSG_HELLO:
        {
            if (payload.size() < sizeof(PmMsgHello))
            {
                PM_ERROR("Пакет HELLO слишком короткий: %zu байт вместо %zu",
                    payload.size(), sizeof(PmMsgHello));
                return false;
            }

            PmMsgHello hello{};
            hello.width = ReadBe32(&payload[0]);
            hello.height = ReadBe32(&payload[4]);
            hello.refreshMilliHz = ReadBe32(&payload[8]);
            hello.densityDpi = ReadBe32(&payload[12]);
            hello.maxDecoderW = ReadBe32(&payload[16]);
            hello.maxDecoderH = ReadBe32(&payload[20]);
            hello.flags = ReadBe32(&payload[24]);

            // Санитарная проверка: мусорная геометрия развалила бы и драйвер,
            // и кодировщик.
            if (hello.width < 64 || hello.height < 64 ||
                hello.width > 16384 || hello.height > 16384)
            {
                PM_ERROR("Клиент прислал недопустимое разрешение %ux%u",
                    hello.width, hello.height);
                return false;
            }

            PM_INFO("Клиент представился: экран %ux%u, %.3f Гц, плотность %u dpi, "
                    "предел декодера %ux%u",
                hello.width, hello.height,
                static_cast<double>(hello.refreshMilliHz) / 1000.0,
                hello.densityDpi, hello.maxDecoderW, hello.maxDecoderH);

            m_helloReceived.store(true, std::memory_order_release);

            if (m_callbacks.onHello)
            {
                m_callbacks.onHello(hello);
            }
            return true;
        }

        case PM_MSG_THERMAL:
        {
            if (payload.size() < sizeof(PmMsgThermal))
            {
                PM_WARN("Пакет THERMAL слишком короткий - игнорируем");
                return true;
            }

            PmMsgThermal thermal{};
            thermal.thermalStatus = ReadBe32s(&payload[0]);
            thermal.batteryPercent = ReadBe32s(&payload[4]);
            thermal.isCharging = ReadBe32s(&payload[8]);

            PM_INFO("Тепловое состояние телефона: %s (уровень %d), батарея %d%%, зарядка: %s",
                ThermalName(thermal.thermalStatus), thermal.thermalStatus,
                thermal.batteryPercent, thermal.isCharging ? "да" : "нет");

            if (m_callbacks.onThermal)
            {
                m_callbacks.onThermal(thermal);
            }
            return true;
        }

        case PM_MSG_STATS:
        {
            if (payload.size() < sizeof(PmMsgStats))
            {
                return true;
            }

            PmMsgStats stats{};
            stats.framesDecoded = (static_cast<uint64_t>(ReadBe32(&payload[0])) << 32) |
                                   ReadBe32(&payload[4]);
            stats.framesDropped = (static_cast<uint64_t>(ReadBe32(&payload[8])) << 32) |
                                   ReadBe32(&payload[12]);
            stats.decodeLatencyUs = ReadBe32(&payload[16]);
            stats.queueDepth = ReadBe32(&payload[20]);

            PM_INFO("Телеметрия клиента: декодировано %llu, отброшено %llu, "
                    "задержка декодера %.2f мс, глубина очереди %u",
                static_cast<unsigned long long>(stats.framesDecoded),
                static_cast<unsigned long long>(stats.framesDropped),
                static_cast<double>(stats.decodeLatencyUs) / 1000.0,
                stats.queueDepth);

            if (m_callbacks.onStats)
            {
                m_callbacks.onStats(stats);
            }
            return true;
        }

        case PM_MSG_KEYFRAME_REQ:
            PM_INFO("Клиент запросил ключевой кадр");
            if (m_callbacks.onKeyFrameRequest)
            {
                m_callbacks.onKeyFrameRequest();
            }
            return true;

        case PM_MSG_BYE:
            PM_INFO("Клиент корректно завершает сессию");
            return false;

        default:
            // Неизвестный тип с корректной длиной - просто пропускаем.
            // Это позволяет расширять протокол без ломки старых стримеров.
            PM_WARN("Получено неизвестное управляющее сообщение типа %u (%u байт) - пропущено",
                header.type, header.payloadLen);
            return true;
        }
    }
}
