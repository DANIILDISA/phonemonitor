#pragma once

/*
 * =============================================================================
 *  TCP-транспорт стримера.
 * =============================================================================
 *
 *  Стример - КЛИЕНТ. Он подключается к 127.0.0.1:6100, откуда adb протаскивает
 *  соединение на телефон, где слушает ServerSocket.
 *
 *  Соединение двунаправленное, но форматы разные:
 *    ПК -> телефон : сырой Annex B поток H.264, без обёртки;
 *    телефон -> ПК : управляющие кадры PmCtrlHeader (см. common/pm_protocol.h).
 *
 *  Чтение управляющих сообщений вынесено в отдельный поток: блокировать им
 *  конвейер кодирования нельзя, а опрашивать сокет неблокирующим recv на
 *  каждом кадре - лишние системные вызовы в горячем пути.
 *
 *  Настройки сокета, критичные для задержки:
 *    * TCP_NODELAY - отключение алгоритма Нейгла. Без него мелкие NAL-юниты
 *      (SPS, PPS, SEI) будут накапливаться до 200 мс перед отправкой.
 *    * SO_SNDBUF в размере ~2 кадров. Больший буфер означает, что при
 *      кратковременной просадке USB накопится очередь устаревших кадров,
 *      и задержка вырастет скачком - лучше заблокировать отправку и
 *      пропустить кадр.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "pm_protocol.h"

namespace pm
{
    /* Колбэки управляющего канала. Вызываются из потока чтения. */
    struct TransportCallbacks
    {
        std::function<void(const PmMsgHello&)> onHello;
        std::function<void(const PmMsgThermal&)> onThermal;
        std::function<void(const PmMsgStats&)> onStats;
        std::function<void()> onKeyFrameRequest;
        std::function<void()> onDisconnect;
    };

    class TcpTransport
    {
    public:
        TcpTransport() noexcept;
        ~TcpTransport() noexcept;

        TcpTransport(const TcpTransport&) = delete;
        TcpTransport& operator=(const TcpTransport&) = delete;

        /* Инициализация Winsock. Вызывается один раз за процесс. */
        static bool GlobalInit() noexcept;
        static void GlobalShutdown() noexcept;

        /*
         * Подключение с повторами. Телефонное приложение может быть ещё не
         * запущено, поэтому connect регулярно отдаёт ECONNREFUSED - это норма,
         * а не ошибка.
         */
        bool Connect(uint16_t port, uint32_t totalTimeoutMs) noexcept;

        /* Разрыв соединения и остановка потока чтения. */
        void Disconnect() noexcept;

        bool IsConnected() const noexcept { return m_connected.load(std::memory_order_acquire); }

        /* Установка колбэков. Должна вызываться до StartReceiver. */
        void SetCallbacks(TransportCallbacks callbacks) noexcept;

        /* Запуск потока чтения управляющего канала. */
        void StartReceiver() noexcept;

        /*
         * Отправка блока данных целиком.
         * Возвращает false при разрыве соединения. Частичная отправка
         * обрабатывается внутри циклом - TCP имеет право принять меньше,
         * чем ему дали.
         */
        bool SendAll(const uint8_t* data, size_t size) noexcept;

        /* Сколько байт отправлено с момента подключения. */
        uint64_t BytesSent() const noexcept { return m_bytesSent.load(std::memory_order_relaxed); }

        /* Получен ли уже пакет HELLO. */
        bool HelloReceived() const noexcept { return m_helloReceived.load(std::memory_order_acquire); }

    private:
        void ReceiverLoop() noexcept;

        /* Чтение ровно size байт. false - соединение закрыто или ошибка. */
        bool ReceiveExact(uint8_t* buffer, size_t size) noexcept;

        /* Разбор одного управляющего кадра. */
        bool ProcessControlMessage(const PmCtrlHeader& header,
                                   const std::vector<uint8_t>& payload) noexcept;

        /* Применение параметров сокета, влияющих на задержку. */
        void ApplySocketOptions() noexcept;

    private:
        SOCKET m_socket = INVALID_SOCKET;
        std::atomic<bool> m_connected{ false };
        std::atomic<bool> m_stopping{ false };
        std::atomic<bool> m_helloReceived{ false };
        std::atomic<uint64_t> m_bytesSent{ 0 };

        std::thread m_receiverThread;
        TransportCallbacks m_callbacks;

        static bool s_winsockReady;
    };
}
