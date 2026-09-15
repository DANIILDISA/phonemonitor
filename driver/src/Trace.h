#pragma once

/*
 * =============================================================================
 *  Подсистема логирования драйвера PhoneMonitor.
 * =============================================================================
 *
 *  WUDFHost.exe не имеет консоли, поэтому единственный дешёвый канал вывода -
 *  OutputDebugStringW, который ловится DebugView / WinDbg / Visual Studio.
 *
 *  Почему не WPP: WPP требует препроцессора tracewpp из WDK на этапе сборки,
 *  что ломает переносимую сборку через CMake. Здесь важнее, чтобы проект
 *  собирался одной командой, а трассировка была читаемой глазами.
 *
 *  Все сообщения - на русском, как требует спецификация проекта.
 *  Формат строки: [PhoneMonitor][УРОВЕНЬ][tid=1234] текст
 *
 *  ВАЖНО про производительность: PM_LOG_TRACE вызывается в горячем цикле
 *  захвата кадров, поэтому он полностью вырезается препроцессором в релизе
 *  (PM_TRACE_LEVEL по умолчанию = INFO). Никаких вычислений аргументов при
 *  выключенном уровне не происходит - это обеспечивается коротким замыканием
 *  условия внутри do/while.
 */

#include "DriverCommon.h"
#include <strsafe.h>

 /* Уровни детализации. Чем меньше число - тем важнее сообщение. */
#define PM_LEVEL_ERROR   0
#define PM_LEVEL_WARN    1
#define PM_LEVEL_INFO    2
#define PM_LEVEL_TRACE   3

#ifndef PM_TRACE_LEVEL
#if defined(_DEBUG) || defined(DBG)
#define PM_TRACE_LEVEL PM_LEVEL_TRACE
#else
#define PM_TRACE_LEVEL PM_LEVEL_INFO
#endif
#endif

/*
 * Файл журнала драйвера.
 *
 * OutputDebugStringW из сессии 0 виден только глобальному отладчику
 * (DebugView с Capture Global Win32), а это требует отдельного запуска от
 * администратора и не годится для автоматической диагностики. Поэтому
 * дублируем вывод в файл, который может прочитать кто угодно.
 *
 * Каталог ProgramData выбран потому, что WUDFHost.exe работает как SYSTEM и
 * имеет туда доступ на запись при любой конфигурации системы, в отличие от
 * профиля пользователя, которого у службы просто нет.
 */
#define PM_LOG_DIR   L"C:\\ProgramData\\PhoneMonitor"
#define PM_LOG_PATH  L"C:\\ProgramData\\PhoneMonitor\\driver.log"

namespace pm
{
    namespace detail
    {
        /*
         * Ленивая инициализация файла журнала.
         *
         * Открываем один раз на процесс и держим хендл до выгрузки.
         * FILE_APPEND_DATA вместо GENERIC_WRITE: запись атомарно добавляется в
         * конец, поэтому несколько потоков драйвера не перетирают друг друга
         * даже без блокировки.
         *
         * FILE_SHARE_READ | FILE_SHARE_WRITE обязательны: без них файл нельзя
         * будет открыть на чтение, пока драйвер работает, а нам нужно смотреть
         * журнал именно вживую.
         */
        inline HANDLE GetLogFile() noexcept
        {
            static HANDLE handle = INVALID_HANDLE_VALUE;
            static LONG initialized = 0;

            // Однократная инициализация без блокировки: гонка максимум
            // приведёт к лишнему CreateDirectory, что безвредно.
            if (InterlockedCompareExchange(&initialized, 1, 0) == 0)
            {
                CreateDirectoryW(PM_LOG_DIR, nullptr);

                handle = CreateFileW(
                    PM_LOG_PATH,
                    FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
            }

            return handle;
        }

        /* Запись строки в файл журнала в UTF-8. */
        inline void WriteLogFile(const wchar_t* line) noexcept
        {
            const HANDLE file = GetLogFile();
            if (file == INVALID_HANDLE_VALUE || file == nullptr)
            {
                return;
            }

            char utf8[1024];
            const int bytes = WideCharToMultiByte(
                CP_UTF8, 0, line, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);

            if (bytes > 1)
            {
                DWORD written = 0;
                // bytes - 1: нуль-терминатор в файл не пишем.
                WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
                // Немедленный сброс: если драйвер сейчас упадёт, последняя
                // строка - самая важная, и потерять её нельзя.
                FlushFileBuffers(file);
            }
        }
    }

    /*
     * Форматирует и выводит одну строку журнала.
     * Буфер намеренно лежит на стеке: выделение кучи в пути захвата кадра -
     * источник непредсказуемых задержек, а нам нужен предсказуемый latency.
     */
    inline void TraceWrite(const wchar_t* level, const wchar_t* format, ...) noexcept
    {
        wchar_t body[512];
        wchar_t line[700];

        va_list args;
        va_start(args, format);
        // StringCchVPrintfW безопасно обрезает строку и всегда ставит нуль-терминатор.
        (void)StringCchVPrintfW(body, ARRAYSIZE(body), format, args);
        va_end(args);

        SYSTEMTIME st{};
        GetLocalTime(&st);

        (void)StringCchPrintfW(line, ARRAYSIZE(line),
            L"[%02u:%02u:%02u.%03u][PhoneMonitor][%s][pid=%lu tid=%lu] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level, GetCurrentProcessId(), GetCurrentThreadId(), body);

        OutputDebugStringW(line);
        detail::WriteLogFile(line);
    }
}

/*
 * Макросы-обёртки. Условие с PM_TRACE_LEVEL вычисляется на этапе компиляции,
 * поэтому отключённые уровни не оставляют в коде ни одной инструкции.
 */
#define PM_LOG_ERROR(fmt, ...) \
    do { if (PM_TRACE_LEVEL >= PM_LEVEL_ERROR) ::pm::TraceWrite(L"ОШИБКА",  fmt, __VA_ARGS__); } while (0)

#define PM_LOG_WARN(fmt, ...)  \
    do { if (PM_TRACE_LEVEL >= PM_LEVEL_WARN)  ::pm::TraceWrite(L"ВНИМАНИЕ", fmt, __VA_ARGS__); } while (0)

#define PM_LOG_INFO(fmt, ...)  \
    do { if (PM_TRACE_LEVEL >= PM_LEVEL_INFO)  ::pm::TraceWrite(L"ИНФО",    fmt, __VA_ARGS__); } while (0)

#define PM_LOG_TRACE(fmt, ...) \
    do { if (PM_TRACE_LEVEL >= PM_LEVEL_TRACE) ::pm::TraceWrite(L"ТРАССА",  fmt, __VA_ARGS__); } while (0)

/*
 * Проверка HRESULT с логированием и ранним возвратом.
 * Используется там, где ошибка фатальна для текущей операции, но не для
 * драйвера в целом.
 */
#define PM_RETURN_IF_FAILED(expr)                                              \
    do {                                                                       \
        const HRESULT pm_hr_ = (expr);                                         \
        if (FAILED(pm_hr_)) {                                                  \
            PM_LOG_ERROR(L"Сбой вызова \"%S\" в %S:%d, HRESULT=0x%08X",        \
                         #expr, __FUNCTION__, __LINE__, pm_hr_);               \
            return pm_hr_;                                                     \
        }                                                                      \
    } while (0)

/* То же самое, но для функций, возвращающих void/NTSTATUS-обёртку. */
#define PM_LOG_IF_FAILED(expr)                                                 \
    do {                                                                       \
        const HRESULT pm_hr_ = (expr);                                         \
        if (FAILED(pm_hr_)) {                                                  \
            PM_LOG_ERROR(L"Сбой вызова \"%S\" в %S:%d, HRESULT=0x%08X",        \
                         #expr, __FUNCTION__, __LINE__, pm_hr_);               \
        }                                                                      \
    } while (0)
