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

#include <windows.h>
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

namespace pm
{
    /*
     * Форматирует и выводит одну строку журнала.
     * Буфер намеренно лежит на стеке: выделение кучи в пути захвата кадра -
     * источник непредсказуемых задержек, а нам нужен предсказуемый latency.
     */
    inline void TraceWrite(const wchar_t* level, const wchar_t* format, ...) noexcept
    {
        wchar_t body[512];
        wchar_t line[640];

        va_list args;
        va_start(args, format);
        // StringCchVPrintfW безопасно обрезает строку и всегда ставит нуль-терминатор.
        (void)StringCchVPrintfW(body, ARRAYSIZE(body), format, args);
        va_end(args);

        (void)StringCchPrintfW(line, ARRAYSIZE(line),
            L"[PhoneMonitor][%s][tid=%lu] %s\n",
            level, GetCurrentThreadId(), body);

        OutputDebugStringW(line);
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
