#include "Config.h"
#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace pm
{
    namespace
    {
        bool Matches(const wchar_t* argument, const wchar_t* shortForm, const wchar_t* longForm) noexcept
        {
            return (shortForm != nullptr && _wcsicmp(argument, shortForm) == 0) ||
                   (longForm != nullptr && _wcsicmp(argument, longForm) == 0);
        }

        /* Чтение числового значения следующего аргумента с проверкой границ. */
        bool ReadUInt(int argc, wchar_t** argv, int& index, const wchar_t* name,
                      uint32_t minValue, uint32_t maxValue, uint32_t& target) noexcept
        {
            if (index + 1 >= argc)
            {
                fwprintf(stderr, L"Ключ %s требует значения\n", name);
                return false;
            }

            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(argv[index + 1], &end, 10);

            if (end == argv[index + 1] || (end != nullptr && *end != L'\0'))
            {
                fwprintf(stderr, L"Значение ключа %s не является числом: %s\n", name, argv[index + 1]);
                return false;
            }
            if (parsed < minValue || parsed > maxValue)
            {
                fwprintf(stderr, L"Значение ключа %s вне диапазона %u..%u\n", name, minValue, maxValue);
                return false;
            }

            target = static_cast<uint32_t>(parsed);
            ++index;
            return true;
        }

        std::string ToUtf8(const wchar_t* text) noexcept
        {
            const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (required <= 1) return {};
            std::string result(static_cast<size_t>(required - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
            return result;
        }
    }

    void PrintUsage() noexcept
    {
        wprintf(
            L"\n"
            L"PhoneMonitor Streamer - передача виртуального монитора на Android по USB\n"
            L"\n"
            L"Использование: phonemonitor-streamer.exe [ключи]\n"
            L"\n"
            L"Сеть:\n"
            L"  --port <N>              Порт по обе стороны adb-форварда (по умолчанию %u)\n"
            L"  --connect-timeout <мс>  Сколько ждать запуска клиента (по умолчанию 120000)\n"
            L"\n"
            L"ADB:\n"
            L"  --adb <путь>            Явный путь к adb.exe\n"
            L"  --no-adb                Не управлять форвардом (создан вручную)\n"
            L"  --device-wait <мс>      Ожидание подключения телефона (по умолчанию 60000)\n"
            L"\n"
            L"Кодирование:\n"
            L"  --bitrate <кбит/с>      Целевой битрейт (по умолчанию 15000)\n"
            L"  --fps <N>               Частота кадров (по умолчанию 60)\n"
            L"  --encoder <имя>         auto | nvenc | mf (по умолчанию auto)\n"
            L"  --intra-refresh <%%>     Ширина полосы Intra-Refresh, %% (по умолчанию 5)\n"
            L"  --no-intra-refresh      Отключить Intra-Refresh\n"
            L"\n"
            L"Тепловой режим:\n"
            L"  --no-thermal            Не снижать разрешение при нагреве телефона\n"
            L"  --thermal-height <N>    Высота кадра в режиме экономии (по умолчанию 720)\n"
            L"  --thermal-bitrate <кбит/с>  Битрейт в режиме экономии (по умолчанию 6000)\n"
            L"\n"
            L"Диагностика:\n"
            L"  --verbose               Подробный журнал (уровень ТРАССА)\n"
            L"  --no-colors             Отключить цветной вывод\n"
            L"  --log-file <путь>       Дублировать журнал в файл\n"
            L"  --stats-interval <N>    Сводка раз в N кадров (по умолчанию 300)\n"
            L"\n"
            L"Прочее:\n"
            L"  --driver-wait <мс>      Ожидание драйвера IDD (по умолчанию 30000)\n"
            L"  --no-reconnect          Завершаться при разрыве соединения\n"
            L"  --help                  Показать эту справку\n"
            L"\n",
            static_cast<unsigned>(PM_TCP_PORT));
    }

    bool ParseCommandLine(int argc, wchar_t** argv, AppConfig& config) noexcept
    {
        for (int i = 1; i < argc; ++i)
        {
            const wchar_t* argument = argv[i];

            if (Matches(argument, L"-h", L"--help") || _wcsicmp(argument, L"/?") == 0)
            {
                PrintUsage();
                config.helpRequested = true;
                return false;
            }
            else if (Matches(argument, nullptr, L"--port"))
            {
                uint32_t value = 0;
                if (!ReadUInt(argc, argv, i, L"--port", 1, 65535, value)) return false;
                config.port = static_cast<uint16_t>(value);
            }
            else if (Matches(argument, nullptr, L"--connect-timeout"))
            {
                if (!ReadUInt(argc, argv, i, L"--connect-timeout", 1000, 3600000,
                              config.connectTimeoutMs)) return false;
            }
            else if (Matches(argument, nullptr, L"--adb"))
            {
                if (i + 1 >= argc)
                {
                    fwprintf(stderr, L"Ключ --adb требует пути\n");
                    return false;
                }
                config.adbPath = argv[++i];
            }
            else if (Matches(argument, nullptr, L"--no-adb"))
            {
                config.manageAdbForward = false;
            }
            else if (Matches(argument, nullptr, L"--device-wait"))
            {
                if (!ReadUInt(argc, argv, i, L"--device-wait", 1000, 3600000,
                              config.deviceWaitMs)) return false;
            }
            else if (Matches(argument, nullptr, L"--bitrate"))
            {
                uint32_t kbps = 0;
                // Верхняя граница 100 Мбит/с: больше не вытянет ни USB 2.0,
                // ни аппаратный декодер телефона среднего класса.
                if (!ReadUInt(argc, argv, i, L"--bitrate", 500, 100000, kbps)) return false;
                config.bitrateBps = kbps * 1000;
            }
            else if (Matches(argument, nullptr, L"--fps"))
            {
                if (!ReadUInt(argc, argv, i, L"--fps", 15, 240, config.fps)) return false;
            }
            else if (Matches(argument, nullptr, L"--encoder"))
            {
                if (i + 1 >= argc)
                {
                    fwprintf(stderr, L"Ключ --encoder требует значения\n");
                    return false;
                }
                config.encoderBackend = ToUtf8(argv[++i]);
            }
            else if (Matches(argument, nullptr, L"--intra-refresh"))
            {
                if (!ReadUInt(argc, argv, i, L"--intra-refresh", 1, 50,
                              config.intraRefreshPercent)) return false;
                config.intraRefresh = true;
            }
            else if (Matches(argument, nullptr, L"--no-intra-refresh"))
            {
                config.intraRefresh = false;
            }
            else if (Matches(argument, nullptr, L"--no-thermal"))
            {
                config.thermalMitigation = false;
            }
            else if (Matches(argument, nullptr, L"--thermal-height"))
            {
                if (!ReadUInt(argc, argv, i, L"--thermal-height", 240, 2160,
                              config.thermalHeight)) return false;
            }
            else if (Matches(argument, nullptr, L"--thermal-bitrate"))
            {
                uint32_t kbps = 0;
                if (!ReadUInt(argc, argv, i, L"--thermal-bitrate", 500, 50000, kbps)) return false;
                config.thermalBitrateBps = kbps * 1000;
            }
            else if (Matches(argument, nullptr, L"--driver-wait"))
            {
                if (!ReadUInt(argc, argv, i, L"--driver-wait", 1000, 600000,
                              config.driverWaitMs)) return false;
            }
            else if (Matches(argument, L"-v", L"--verbose"))
            {
                config.verbose = true;
            }
            else if (Matches(argument, nullptr, L"--no-colors"))
            {
                config.noColors = true;
            }
            else if (Matches(argument, nullptr, L"--log-file"))
            {
                if (i + 1 >= argc)
                {
                    fwprintf(stderr, L"Ключ --log-file требует пути\n");
                    return false;
                }
                config.logFilePath = argv[++i];
            }
            else if (Matches(argument, nullptr, L"--stats-interval"))
            {
                if (!ReadUInt(argc, argv, i, L"--stats-interval", 10, 100000,
                              config.statsIntervalFrames)) return false;
            }
            else if (Matches(argument, nullptr, L"--no-reconnect"))
            {
                config.autoReconnect = false;
            }
            else
            {
                fwprintf(stderr, L"Неизвестный ключ: %s\n", argument);
                PrintUsage();
                return false;
            }
        }

        // Тепловой битрейт не должен превышать основной - иначе «экономия»
        // увеличила бы нагрузку.
        if (config.thermalBitrateBps > config.bitrateBps)
        {
            config.thermalBitrateBps = config.bitrateBps / 2;
        }

        return true;
    }
}
