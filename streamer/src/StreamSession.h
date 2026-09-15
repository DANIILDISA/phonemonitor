#pragma once

/*
 * =============================================================================
 *  Оркестратор сессии: связывает драйвер, кодировщик и сеть.
 * =============================================================================
 *
 *  Последовательность работы (метод Run):
 *
 *    1.  Подключение к драйверу IDD через разделяемую память.
 *    2.  Поиск adb, ожидание устройства, создание форварда tcp:6100.
 *    3.  Подключение к 127.0.0.1:6100 и запуск чтения управляющего канала.
 *    4.  Ожидание пакета HELLO с геометрией экрана телефона.
 *    5.  Согласование разрешения: выравнивание, ограничение возможностями
 *        декодера, запрос режима у драйвера, ожидание подтверждения.
 *    6.  Создание аппаратного кодировщика под согласованный режим.
 *    7.  Главный цикл: кадр -> кодирование -> отправка.
 *    8.  Реакция на события: тепловой троттлинг, запрос ключевого кадра,
 *        разрыв соединения, смена режима на стороне Windows.
 *
 *  Потоки:
 *    * главный поток - шаги 1-7;
 *    * поток TcpTransport - чтение управляющих сообщений;
 *    * потоки внутри кодировщика и драйвера - нам не принадлежат.
 *
 *  Обмен между потоком чтения и главным циклом идёт через атомарные флаги:
 *  очередь сообщений здесь избыточна, событий единицы в секунду.
 */

#include <atomic>
#include <memory>
#include <cstdint>

#include "Config.h"
#include "ipc/IpcClient.h"
#include "net/TcpTransport.h"
#include "net/AdbBridge.h"
#include "encoder/IEncoder.h"

namespace pm
{
    class StreamSession
    {
    public:
        explicit StreamSession(const AppConfig& config) noexcept;
        ~StreamSession() noexcept;

        StreamSession(const StreamSession&) = delete;
        StreamSession& operator=(const StreamSession&) = delete;

        /* Полный цикл работы. Возвращает код возврата процесса. */
        int Run() noexcept;

        /* Запрос остановки из обработчика Ctrl+C. Потокобезопасна. */
        void RequestStop() noexcept;

        /* Аварийная уборка для обработчика консольных сигналов:
           снимает adb-форвард, который иначе остался бы висеть. */
        void EmergencyCleanup() noexcept;

    private:
        /* Шаги инициализации. */
        bool SetupAdb() noexcept;
        bool ConnectToDriver() noexcept;
        bool ConnectToClient() noexcept;
        bool WaitForHello(uint32_t timeoutMs) noexcept;

        /* Согласование разрешения между телефоном, драйвером и кодировщиком. */
        bool NegotiateResolution() noexcept;

        /* Создание/пересоздание кодировщика под текущий режим. */
        bool CreateEncoder(uint32_t width, uint32_t height, uint32_t bitrateBps) noexcept;

        /* Главный цикл передачи. */
        int StreamLoop() noexcept;

        /* Обработка отложенных событий управляющего канала. */
        void ProcessPendingEvents() noexcept;

        /* Применение теплового режима. */
        void ApplyThermalState() noexcept;

        /* Периодическая сводка в журнал. */
        void LogStatistics(uint64_t frameCount) noexcept;

        /* Полный сброс сессии перед переподключением. */
        void TeardownSession() noexcept;

    private:
        AppConfig m_config;

        IpcClient m_ipc;
        TcpTransport m_transport;
        AdbBridge m_adb;
        std::unique_ptr<IVideoEncoder> m_encoder;

        /* --- Геометрия ------------------------------------------------------ */
        std::atomic<uint32_t> m_clientWidth{ 0 };
        std::atomic<uint32_t> m_clientHeight{ 0 };
        std::atomic<uint32_t> m_clientRefreshHz{ 60 };
        std::atomic<uint32_t> m_maxDecoderWidth{ 0 };
        std::atomic<uint32_t> m_maxDecoderHeight{ 0 };

        uint32_t m_encodeWidth = 0;
        uint32_t m_encodeHeight = 0;
        uint32_t m_encodeBitrate = 0;

        /* --- Флаги событий, выставляемые потоком чтения ---------------------- */
        std::atomic<bool> m_helloArrived{ false };
        std::atomic<bool> m_keyFrameRequested{ false };
        std::atomic<bool> m_clientDisconnected{ false };
        std::atomic<int32_t> m_thermalStatus{ PM_THERMAL_NONE };
        std::atomic<bool> m_thermalChanged{ false };
        std::atomic<bool> m_thermalActive{ false };

        /* --- Управление жизненным циклом -------------------------------------- */
        std::atomic<bool> m_stopRequested{ false };

        /* --- Статистика --------------------------------------------------------- */
        uint64_t m_framesEncoded = 0;
        uint64_t m_framesSent = 0;
        uint64_t m_framesSkipped = 0;
        uint64_t m_bytesEncoded = 0;
        uint64_t m_totalLatencyUs = 0;
        uint64_t m_maxLatencyUs = 0;
        uint64_t m_totalEncodeUs = 0;
        uint64_t m_statsWindowStartQpc = 0;
    };
}
