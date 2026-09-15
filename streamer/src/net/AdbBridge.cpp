#include "AdbBridge.h"
#include "../Log.h"

#include <shlobj.h>
#include <sstream>
#include <algorithm>

namespace pm
{
    namespace
    {
        /* Разбиение строки вывода adb на непустые строки. */
        std::vector<std::wstring> SplitLines(const std::wstring& text) noexcept
        {
            std::vector<std::wstring> lines;
            std::wistringstream stream(text);
            std::wstring line;
            while (std::getline(stream, line))
            {
                // Убираем возврат каретки: adb пишет CRLF.
                while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
                {
                    line.pop_back();
                }
                if (!line.empty())
                {
                    lines.push_back(line);
                }
            }
            return lines;
        }

        /* Значение переменной окружения или пустая строка. */
        std::wstring GetEnv(const wchar_t* name) noexcept
        {
            wchar_t buffer[MAX_PATH * 2];
            const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
            if (length == 0 || length >= ARRAYSIZE(buffer))
            {
                return {};
            }
            return std::wstring(buffer, length);
        }

        /* Каталог, в котором лежит наш исполняемый файл. */
        std::wstring ModuleDirectory() noexcept
        {
            wchar_t path[MAX_PATH];
            const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
            if (length == 0)
            {
                return {};
            }
            std::wstring full(path, length);
            const size_t slash = full.find_last_of(L"\\/");
            return (slash == std::wstring::npos) ? std::wstring{} : full.substr(0, slash);
        }
    }

    AdbBridge::~AdbBridge() noexcept
    {
        RemoveForward();
    }

    bool AdbBridge::FileExists(const std::wstring& path) noexcept
    {
        if (path.empty()) return false;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return (attributes != INVALID_FILE_ATTRIBUTES) &&
               ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
    }

    std::vector<std::wstring> AdbBridge::BuildSearchPaths() noexcept
    {
        std::vector<std::wstring> candidates;

        // 1. Рядом с нашим исполняемым файлом - сценарий «портативная поставка»,
        //    когда adb.exe кладут в одну папку со стримером.
        const std::wstring moduleDir = ModuleDirectory();
        if (!moduleDir.empty())
        {
            candidates.push_back(moduleDir + L"\\adb.exe");
            candidates.push_back(moduleDir + L"\\platform-tools\\adb.exe");
        }

        // 2. Явные переменные окружения Android SDK.
        for (const wchar_t* var : { L"ANDROID_HOME", L"ANDROID_SDK_ROOT" })
        {
            const std::wstring sdk = GetEnv(var);
            if (!sdk.empty())
            {
                candidates.push_back(sdk + L"\\platform-tools\\adb.exe");
            }
        }

        // 3. Стандартные места установки Android Studio / SDK.
        const std::wstring localAppData = GetEnv(L"LOCALAPPDATA");
        if (!localAppData.empty())
        {
            candidates.push_back(localAppData + L"\\Android\\Sdk\\platform-tools\\adb.exe");
        }

        const std::wstring userProfile = GetEnv(L"USERPROFILE");
        if (!userProfile.empty())
        {
            candidates.push_back(userProfile + L"\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe");
            candidates.push_back(userProfile + L"\\Android\\Sdk\\platform-tools\\adb.exe");
        }

        for (const wchar_t* var : { L"ProgramFiles", L"ProgramFiles(x86)" })
        {
            const std::wstring programFiles = GetEnv(var);
            if (!programFiles.empty())
            {
                candidates.push_back(programFiles + L"\\Android\\platform-tools\\adb.exe");
                candidates.push_back(programFiles + L"\\Android\\android-sdk\\platform-tools\\adb.exe");
            }
        }

        // 4. Scoop / Chocolatey - частые способы установки platform-tools.
        if (!userProfile.empty())
        {
            candidates.push_back(userProfile + L"\\scoop\\apps\\adb\\current\\adb.exe");
        }
        candidates.push_back(L"C:\\ProgramData\\chocolatey\\bin\\adb.exe");

        return candidates;
    }

    bool AdbBridge::Locate(const std::wstring& explicitPath) noexcept
    {
        // Явно указанный путь имеет абсолютный приоритет: если пользователь
        // назвал конкретный adb, подменять его найденным в PATH нельзя.
        if (!explicitPath.empty())
        {
            if (FileExists(explicitPath))
            {
                m_adbPath = explicitPath;
                PM_INFO("Используется adb, указанный явно: %S", m_adbPath.c_str());
                return true;
            }
            PM_ERROR("Указанный путь к adb не существует: %S", explicitPath.c_str());
            return false;
        }

        // Поиск в PATH через SearchPathW - он учитывает PATHEXT и текущий каталог.
        {
            wchar_t resolved[MAX_PATH];
            const DWORD length = SearchPathW(nullptr, L"adb.exe", nullptr,
                ARRAYSIZE(resolved), resolved, nullptr);
            if (length > 0 && length < ARRAYSIZE(resolved))
            {
                m_adbPath.assign(resolved, length);
                PM_INFO("adb найден в PATH: %S", m_adbPath.c_str());
                return true;
            }
        }

        for (const std::wstring& candidate : BuildSearchPaths())
        {
            if (FileExists(candidate))
            {
                m_adbPath = candidate;
                PM_INFO("adb найден: %S", m_adbPath.c_str());
                return true;
            }
        }

        PM_ERROR("adb.exe не найден. Установите Android SDK Platform Tools либо "
                 "укажите путь ключом --adb <путь к adb.exe>");
        return false;
    }

    // -------------------------------------------------------------------------
    int AdbBridge::RunAdb(const std::wstring& arguments, std::wstring& output,
                          uint32_t timeoutMs) noexcept
    {
        output.clear();

        if (m_adbPath.empty())
        {
            return -1;
        }

        // ---------------------------------------------------------------------
        // Канал для захвата вывода.
        //
        // Наследуемым делаем ТОЛЬКО пишущий конец: если читающий конец тоже
        // унаследуется дочерним процессом, ReadFile никогда не вернёт EOF -
        // в системе останется открытая ссылка на запись.
        // ---------------------------------------------------------------------
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 64 * 1024))
        {
            PM_ERROR("Не удалось создать канал для вывода adb, код ошибки %lu", GetLastError());
            return -1;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        std::wstring commandLine = L"\"" + m_adbPath + L"\" " + arguments;
        // CreateProcessW имеет право модифицировать буфер командной строки,
        // поэтому передаём изменяемую копию.
        std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back(L'\0');

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startupInfo.hStdOutput = writePipe;
        startupInfo.hStdError = writePipe;
        startupInfo.hStdInput = nullptr;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo{};

        const BOOL created = CreateProcessW(
            nullptr, commandBuffer.data(), nullptr, nullptr,
            TRUE,                       // наследование хендлов обязательно
            CREATE_NO_WINDOW,           // без мигающего окна консоли
            nullptr, nullptr, &startupInfo, &processInfo);

        // Наш экземпляр пишущего конца закрываем немедленно: пока он открыт,
        // ReadFile не увидит конца потока.
        CloseHandle(writePipe);

        if (!created)
        {
            const DWORD err = GetLastError();
            PM_ERROR("Не удалось запустить adb (%S), код ошибки %lu", commandLine.c_str(), err);
            CloseHandle(readPipe);
            return -1;
        }

        PM_TRACE("Выполняется: %S", commandLine.c_str());

        // Чтение вывода до EOF. adb пишет немного, поэтому читаем синхронно.
        std::string rawOutput;
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            rawOutput.append(buffer, bytesRead);
        }
        CloseHandle(readPipe);

        const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, timeoutMs);
        DWORD exitCode = static_cast<DWORD>(-1);

        if (waitResult == WAIT_TIMEOUT)
        {
            PM_WARN("adb не завершился за %u мс - принудительно останавливаем", timeoutMs);
            TerminateProcess(processInfo.hProcess, 1);
        }
        else
        {
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        // adb выводит в кодировке консоли; на подавляющем большинстве систем
        // это ASCII-совместимый вывод, поэтому берём текущую кодовую страницу.
        if (!rawOutput.empty())
        {
            const int required = MultiByteToWideChar(CP_ACP, 0, rawOutput.c_str(),
                static_cast<int>(rawOutput.size()), nullptr, 0);
            output.resize(static_cast<size_t>(required));
            MultiByteToWideChar(CP_ACP, 0, rawOutput.c_str(),
                static_cast<int>(rawOutput.size()), output.data(), required);
        }

        return static_cast<int>(exitCode);
    }

    // -------------------------------------------------------------------------
    bool AdbBridge::StartServer() noexcept
    {
        std::wstring output;
        // Таймаут увеличен: первый запуск сервера adb может занять несколько
        // секунд, особенно при активном антивирусе.
        const int code = RunAdb(L"start-server", output, 20000);

        if (code != 0)
        {
            PM_WARN("adb start-server вернул код %d. Вывод: %S", code, output.c_str());
            return false;
        }

        PM_INFO("Сервер adb запущен");
        return true;
    }

    std::vector<AdbDeviceInfo> AdbBridge::ListDevices() noexcept
    {
        std::vector<AdbDeviceInfo> devices;

        std::wstring output;
        if (RunAdb(L"devices -l", output) != 0)
        {
            PM_WARN("adb devices завершился с ошибкой. Вывод: %S", output.c_str());
            return devices;
        }

        // Формат вывода:
        //   List of devices attached
        //   ABC123   device product:... model:Pixel_7 device:...
        for (const std::wstring& line : SplitLines(output))
        {
            if (line.find(L"List of devices") != std::wstring::npos)
            {
                continue;
            }

            std::wistringstream lineStream(line);
            AdbDeviceInfo info;
            lineStream >> info.serial >> info.state;

            if (info.serial.empty() || info.state.empty())
            {
                continue;
            }

            // Извлечение модели из хвоста строки (ключ model:).
            const size_t modelPos = line.find(L"model:");
            if (modelPos != std::wstring::npos)
            {
                const size_t start = modelPos + 6;
                size_t end = line.find(L' ', start);
                if (end == std::wstring::npos) end = line.size();
                info.model = line.substr(start, end - start);
            }

            info.usable = (info.state == L"device");
            devices.push_back(info);

            if (info.state == L"unauthorized")
            {
                PM_WARN("Устройство %S не авторизовано. Подтвердите отладку по USB на экране телефона",
                    info.serial.c_str());
            }
            else if (info.state == L"offline")
            {
                PM_WARN("Устройство %S в состоянии offline. Переподключите кабель", info.serial.c_str());
            }
        }

        return devices;
    }

    std::wstring AdbBridge::WaitForDevice(uint32_t timeoutMs) noexcept
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        bool reportedWaiting = false;

        for (;;)
        {
            for (const AdbDeviceInfo& device : ListDevices())
            {
                if (device.usable)
                {
                    // Формат %S в узкой строке означает широкую строку.
                    // Смешивать %s и %S в одном вызове нельзя, поэтому
                    // описание модели собирается заранее одной wstring.
                    const std::wstring description = device.model.empty()
                        ? device.serial
                        : device.serial + L" (модель " + device.model + L")";

                    PM_INFO("Устройство найдено: %S", description.c_str());
                    return device.serial;
                }
            }

            if (GetTickCount64() >= deadline)
            {
                PM_ERROR("Ни одно устройство Android не готово к работе за %u мс", timeoutMs);
                return {};
            }

            if (!reportedWaiting)
            {
                PM_INFO("Ожидание подключения устройства Android с включённой отладкой по USB...");
                reportedWaiting = true;
            }

            Sleep(700);
        }
    }

    // -------------------------------------------------------------------------
    bool AdbBridge::CreateForward(uint16_t hostPort, uint16_t devicePort,
                                  const std::wstring& serial) noexcept
    {
        // Снимаем возможный остаток с прошлого запуска: если предыдущий процесс
        // завершился аварийно, форвард остался и может указывать на другое
        // устройство.
        {
            std::wstring stale;
            std::wstring removeArgs = L"forward --remove tcp:" + std::to_wstring(hostPort);
            RunAdb(removeArgs, stale, 5000);
        }

        std::wstring arguments;
        if (!serial.empty())
        {
            // Ключ -s обязателен при нескольких подключённых устройствах,
            // иначе adb откажется выполнять команду.
            arguments = L"-s " + serial + L" ";
        }
        arguments += L"forward tcp:" + std::to_wstring(hostPort) +
                     L" tcp:" + std::to_wstring(devicePort);

        std::wstring output;
        const int code = RunAdb(arguments, output);

        if (code != 0)
        {
            PM_ERROR("Не удалось создать форвард tcp:%u -> tcp:%u (код %d). Вывод adb: %S",
                hostPort, devicePort, code, output.c_str());
            return false;
        }

        m_hostPort = hostPort;
        m_serial = serial;
        m_forwardActive = true;

        PM_INFO("Форвард создан: localhost:%u (ПК) -> localhost:%u (телефон)", hostPort, devicePort);
        return true;
    }

    void AdbBridge::RemoveForward() noexcept
    {
        if (!m_forwardActive)
        {
            return;
        }

        std::wstring arguments;
        if (!m_serial.empty())
        {
            arguments = L"-s " + m_serial + L" ";
        }
        arguments += L"forward --remove tcp:" + std::to_wstring(m_hostPort);

        std::wstring output;
        const int code = RunAdb(arguments, output, 5000);

        if (code == 0)
        {
            PM_INFO("Форвард tcp:%u снят", m_hostPort);
        }
        else
        {
            PM_WARN("Не удалось снять форвард tcp:%u (код %d). Вывод: %S",
                m_hostPort, code, output.c_str());
        }

        m_forwardActive = false;
    }
}
