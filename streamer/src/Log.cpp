#include "Log.h"

#include <mutex>
#include <ctime>
#include <iterator>   // std::size

namespace pm
{
    LogLevel g_logLevel = LogLevel::Info;

    namespace
    {
        std::mutex g_logMutex;
        FILE* g_logFile = nullptr;
        bool g_colors = false;

        /* ANSI-последовательности. Пустые строки, если цвет выключен. */
        const char* LevelColor(LogLevel level) noexcept
        {
            if (!g_colors) return "";
            switch (level)
            {
            case LogLevel::Error: return "\x1b[91m";  // ярко-красный
            case LogLevel::Warn:  return "\x1b[93m";  // жёлтый
            case LogLevel::Info:  return "\x1b[97m";  // белый
            case LogLevel::Trace: return "\x1b[90m";  // серый
            }
            return "";
        }

        const char* LevelName(LogLevel level) noexcept
        {
            switch (level)
            {
            case LogLevel::Error: return "ОШИБКА  ";
            case LogLevel::Warn:  return "ВНИМАНИЕ";
            case LogLevel::Info:  return "ИНФО    ";
            case LogLevel::Trace: return "ТРАССА  ";
            }
            return "?       ";
        }
    }

    void LogInit(bool enableColors) noexcept
    {
        // Кодовая страница 65001 (UTF-8) обязательна: без неё русский текст
        // в консоли Windows превращается в вопросительные знаки.
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        g_colors = false;
        if (enableColors)
        {
            // Виртуальный терминал включается флагом консольного режима.
            // На Windows 10 1511+ поддерживается всегда; если вызов не прошёл,
            // просто отключаем цвет, а не падаем.
            const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
            {
                if (SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
                {
                    g_colors = true;
                }
            }
        }

        // Отключение построчной буферизации: при аварийном завершении не
        // должно теряться последнее сообщение, а именно оно обычно и важно.
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

    bool LogOpenFile(const wchar_t* path) noexcept
    {
        std::lock_guard<std::mutex> lock(g_logMutex);

        if (g_logFile != nullptr)
        {
            fclose(g_logFile);
            g_logFile = nullptr;
        }

        // Режим "a" - дозапись: журнал переживает перезапуски стримера,
        // что критично при отладке разрывов соединения.
        if (_wfopen_s(&g_logFile, path, L"a, ccs=UTF-8") != 0 || g_logFile == nullptr)
        {
            return false;
        }
        return true;
    }

    void LogShutdown() noexcept
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile != nullptr)
        {
            fflush(g_logFile);
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }

    void LogWrite(LogLevel level, const char* format, ...) noexcept
    {
        // Метка времени с миллисекундами: локальное время, потому что журнал
        // читает человек и сопоставляет его с событиями на экране.
        SYSTEMTIME st{};
        GetLocalTime(&st);

        char body[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(body, sizeof(body), format, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_logMutex);

        fprintf(stdout, "%s[%02u:%02u:%02u.%03u][%s][поток %5lu] %s%s\n",
            LevelColor(level),
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LevelName(level),
            GetCurrentThreadId(),
            body,
            g_colors ? "\x1b[0m" : "");

        if (g_logFile != nullptr)
        {
            fprintf(g_logFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u][%s][поток %lu] %s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                LevelName(level), GetCurrentThreadId(), body);
            fflush(g_logFile);
        }
    }

    const char* HResultToText(HRESULT hr) noexcept
    {
        // Кэш на поток: FormatMessage выделяет память, а нам нужно вернуть
        // указатель, живущий до следующего вызова из того же потока.
        static thread_local char buffer[512];

        // ---------------------------------------------------------------------
        // Только широкая версия FormatMessage.
        //
        // FormatMessageA возвращает текст в системной кодировке ANSI (на
        // русской Windows это CP1251), а весь наш вывод идёт в UTF-8. В
        // результате расшифровка системной ошибки печаталась кракозябрами
        // ровно в тот момент, когда она нужнее всего - при диагностике сбоя.
        //
        // Правильный путь: получить UTF-16 и явно перекодировать в UTF-8.
        // ---------------------------------------------------------------------
        wchar_t wide[512];

        DWORD length = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            static_cast<DWORD>(hr),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            wide, static_cast<DWORD>(std::size(wide)) - 1, nullptr);

        if (length == 0)
        {
            snprintf(buffer, sizeof(buffer), "неизвестный код 0x%08X", static_cast<unsigned>(hr));
            return buffer;
        }

        // Системные сообщения заканчиваются переводом строки - убираем его,
        // иначе журнал разъезжается.
        while (length > 0 && (wide[length - 1] == L'\n' || wide[length - 1] == L'\r'))
        {
            wide[--length] = L'\0';
        }

        const int converted = WideCharToMultiByte(
            CP_UTF8, 0, wide, static_cast<int>(length),
            buffer, static_cast<int>(sizeof(buffer)) - 1, nullptr, nullptr);

        if (converted <= 0)
        {
            snprintf(buffer, sizeof(buffer), "код 0x%08X (текст не перекодировался)",
                static_cast<unsigned>(hr));
            return buffer;
        }

        buffer[converted] = '\0';
        return buffer;
    }
}
