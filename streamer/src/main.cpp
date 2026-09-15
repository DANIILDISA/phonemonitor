/*
 * =============================================================================
 *  PhoneMonitor Streamer - точка входа.
 * =============================================================================
 *
 *  Приложение работает как консольная служба: запускается, поднимает конвейер
 *  и работает до Ctrl+C либо до закрытия окна.
 *
 *  Ключевой момент корректности - уборка при завершении. adb-форвард живёт в
 *  сервере adb, а не в нашем процессе, поэтому аварийное завершение оставило бы
 *  его висеть, и следующий запуск столкнулся бы с занятым портом. Обработчик
 *  консольных сигналов снимает форвард во всех сценариях завершения, включая
 *  закрытие окна и выход пользователя из системы.
 */

#include <windows.h>
#include <objbase.h>
#include <cstdio>

#include "Config.h"
#include "Log.h"
#include "StreamSession.h"
#include "net/TcpTransport.h"

namespace
{
    /* Указатель на активную сессию для обработчика сигналов.
       Volatile, а не atomic: обработчик выполняется в отдельном потоке,
       но присваивание указателя на x86/x64 атомарно по определению. */
    pm::StreamSession* volatile g_session = nullptr;

    /*
     * Обработчик консольных событий.
     *
     * Windows даёт обработчику ограниченное время (по умолчанию 5 секунд для
     * CTRL_CLOSE_EVENT), после чего процесс убивается принудительно. Поэтому
     * здесь только два действия: запрос остановки и снятие форварда.
     */
    BOOL WINAPI ConsoleHandler(DWORD signal) noexcept
    {
        switch (signal)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            PM_INFO("Получен сигнал прерывания - завершаем работу");
            if (g_session != nullptr)
            {
                g_session->RequestStop();
            }
            // TRUE означает «сигнал обработан»: процесс не будет завершён
            // системой немедленно, и главный цикл успеет выйти корректно.
            return TRUE;

        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            PM_INFO("Окно закрывается - аварийная уборка");
            if (g_session != nullptr)
            {
                g_session->RequestStop();
                // Времени на корректный выход нет: снимаем форвард прямо здесь.
                g_session->EmergencyCleanup();
            }
            return TRUE;

        default:
            return FALSE;
        }
    }

    /*
     * Повышение класса приоритета процесса.
     *
     * ABOVE_NORMAL, а не HIGH: HIGH_PRIORITY_CLASS у процесса, активно
     * работающего с GPU, способен вызвать голодание системных потоков
     * композитора и дать обратный эффект. Тонкая настройка приоритета
     * выполняется на уровне отдельных потоков через MMCSS.
     */
    void RaiseProcessPriority() noexcept
    {
        if (SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
        {
            PM_TRACE("Класс приоритета процесса повышен до ABOVE_NORMAL");
        }
        else
        {
            PM_WARN("Не удалось повысить приоритет процесса, код ошибки %lu", GetLastError());
        }
    }

    void PrintBanner(const pm::AppConfig& config) noexcept
    {
        PM_INFO("=====================================================");
        PM_INFO("  PhoneMonitor Streamer");
        PM_INFO("  Вторичный монитор Windows на экране Android по USB");
        PM_INFO("=====================================================");
        PM_INFO("Параметры: порт %u, битрейт %u кбит/с, %u к/с, кодировщик \"%s\"",
            config.port, config.bitrateBps / 1000, config.fps, config.encoderBackend.c_str());
        PM_INFO("Intra-Refresh: %s%s | Тепловой троттлинг: %s",
            config.intraRefresh ? "включён" : "выключен",
            config.intraRefresh ? "" : "",
            config.thermalMitigation ? "включён" : "выключен");
    }
}

int wmain(int argc, wchar_t** argv)
{
    pm::AppConfig config;

    // Разбор аргументов делаем ДО инициализации журнала: ключи --verbose и
    // --no-colors влияют на то, как журнал настраивается.
    if (!pm::ParseCommandLine(argc, argv, config))
    {
        return 1;
    }

    pm::g_logLevel = config.verbose ? pm::LogLevel::Trace : pm::LogLevel::Info;
    pm::LogInit(!config.noColors);

    if (!config.logFilePath.empty())
    {
        if (!pm::LogOpenFile(config.logFilePath.c_str()))
        {
            fwprintf(stderr, L"Не удалось открыть файл журнала: %s\n", config.logFilePath.c_str());
        }
    }

    PrintBanner(config);

    // ---------------------------------------------------------------------
    // COM нужен Media Foundation и DXGI.
    //
    // Многопоточная модель (MTA) обязательна: объекты MF вызываются из
    // нескольких потоков, а STA потребовала бы цикла обработки сообщений.
    // ---------------------------------------------------------------------
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult))
    {
        PM_ERROR("CoInitializeEx не удался: %s", pm::HResultToText(comResult));
        pm::LogShutdown();
        return 1;
    }

    if (!pm::TcpTransport::GlobalInit())
    {
        CoUninitialize();
        pm::LogShutdown();
        return 1;
    }

    RaiseProcessPriority();

    int exitCode = 0;
    {
        pm::StreamSession session(config);
        g_session = &session;

        if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE))
        {
            PM_WARN("Не удалось установить обработчик консольных сигналов, код %lu",
                GetLastError());
        }

        exitCode = session.Run();

        // Снимаем обработчик до разрушения сессии: иначе сигнал, пришедший в
        // момент уборки, обратился бы к уже уничтоженному объекту.
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        g_session = nullptr;
    }

    pm::TcpTransport::GlobalShutdown();
    CoUninitialize();

    PM_INFO("Работа завершена, код возврата %d", exitCode);
    pm::LogShutdown();

    return exitCode;
}
