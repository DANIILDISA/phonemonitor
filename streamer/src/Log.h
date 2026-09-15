#pragma once

/*
 * =============================================================================
 *  Журналирование стримера.
 * =============================================================================
 *
 *  Требования, из которых выросла эта реализация:
 *    * вывод в консоль в UTF-8 (сообщения на русском);
 *    * метка времени с точностью до миллисекунд - без неё невозможно
 *      анализировать задержки конвейера;
 *    * потокобезопасность: пишут поток захвата, поток кодирования, поток
 *      управляющего канала и поток ADB;
 *    * нулевая стоимость отключённого уровня TRACE в горячем цикле.
 *
 *  Сознательное упрощение: используется одна критическая секция на весь вывод.
 *  Логирование в горячем пути включается только на уровне TRACE, который в
 *  боевом режиме выключен, поэтому конкуренция за блокировку не возникает.
 */

#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace pm
{
    enum class LogLevel : int
    {
        Error = 0,
        Warn = 1,
        Info = 2,
        Trace = 3
    };

    /* Глобальный порог. Меняется ключом командной строки --verbose. */
    extern LogLevel g_logLevel;

    /* Инициализация консоли: включение UTF-8 и виртуальных терминальных
       последовательностей для цветного вывода. Вызывается один раз из main. */
    void LogInit(bool enableColors) noexcept;

    /* Закрытие файла журнала, если он открывался. */
    void LogShutdown() noexcept;

    /* Дублирование вывода в файл (ключ --log-file). */
    bool LogOpenFile(const wchar_t* path) noexcept;

    /* Низкоуровневый вывод. Не вызывать напрямую - есть макросы ниже. */
    void LogWrite(LogLevel level, const char* format, ...) noexcept;

    /* Расшифровка HRESULT в читаемое сообщение (для журнала). */
    const char* HResultToText(HRESULT hr) noexcept;
}

#define PM_ERROR(fmt, ...) \
    do { if (::pm::g_logLevel >= ::pm::LogLevel::Error) ::pm::LogWrite(::pm::LogLevel::Error, fmt, ##__VA_ARGS__); } while (0)

#define PM_WARN(fmt, ...)  \
    do { if (::pm::g_logLevel >= ::pm::LogLevel::Warn)  ::pm::LogWrite(::pm::LogLevel::Warn,  fmt, ##__VA_ARGS__); } while (0)

#define PM_INFO(fmt, ...)  \
    do { if (::pm::g_logLevel >= ::pm::LogLevel::Info)  ::pm::LogWrite(::pm::LogLevel::Info,  fmt, ##__VA_ARGS__); } while (0)

#define PM_TRACE(fmt, ...) \
    do { if (::pm::g_logLevel >= ::pm::LogLevel::Trace) ::pm::LogWrite(::pm::LogLevel::Trace, fmt, ##__VA_ARGS__); } while (0)

/* Проверка HRESULT с логированием и выходом из функции. */
#define PM_CHECK_HR(expr, message)                                            \
    do {                                                                      \
        const HRESULT pm_hr_ = (expr);                                        \
        if (FAILED(pm_hr_)) {                                                 \
            PM_ERROR("%s (HRESULT=0x%08X, %s)", message,                      \
                     static_cast<unsigned>(pm_hr_), ::pm::HResultToText(pm_hr_)); \
            return pm_hr_;                                                    \
        }                                                                     \
    } while (0)
