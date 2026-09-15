#pragma once

/*
 * =============================================================================
 *  Управление adb-форвардом.
 * =============================================================================
 *
 *  Схема соединения:
 *
 *      [Windows: стример] --connect--> 127.0.0.1:6100
 *                                          |
 *                                     adb.exe (хост)
 *                                          |  USB
 *                                     adbd (телефон)
 *                                          |
 *                          127.0.0.1:6100 <--listen-- [Android: ServerSocket]
 *
 *  Команда `adb forward tcp:6100 tcp:6100` создаёт этот канал: всё, что
 *  записано в локальный порт 6100 на ПК, выходит из порта 6100 внутри
 *  устройства. Поэтому стример - TCP-КЛИЕНТ, а телефон - сервер.
 *
 *  Обязанности класса:
 *    1. Найти adb.exe: переменная окружения, PATH, стандартные места установки
 *       Android SDK и Android Studio, каталог рядом с исполняемым файлом.
 *    2. Проверить, что устройство подключено и авторизовано.
 *    3. Создать форвард и ГАРАНТИРОВАННО снять его при завершении - включая
 *       аварийное (Ctrl+C, закрытие окна, вылет).
 *
 *  Про гарантию снятия: деструктор не выполняется при ExitProcess, поэтому
 *  снятие форварда дополнительно вызывается из обработчика консольных
 *  сигналов в main.cpp.
 */

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace pm
{
    /* Сведения о подключённом устройстве. */
    struct AdbDeviceInfo
    {
        std::wstring serial;      /* серийный номер                            */
        std::wstring state;       /* device / unauthorized / offline           */
        std::wstring model;       /* модель, если сообщил adb                  */
        bool usable = false;      /* state == device                           */
    };

    class AdbBridge
    {
    public:
        AdbBridge() noexcept = default;
        ~AdbBridge() noexcept;

        AdbBridge(const AdbBridge&) = delete;
        AdbBridge& operator=(const AdbBridge&) = delete;

        /*
         * Поиск adb.exe. explicitPath, если задан, проверяется первым.
         * Возвращает false, если исполняемый файл так и не найден.
         */
        bool Locate(const std::wstring& explicitPath) noexcept;

        const std::wstring& AdbPath() const noexcept { return m_adbPath; }

        /* Список устройств по данным `adb devices -l`. */
        std::vector<AdbDeviceInfo> ListDevices() noexcept;

        /*
         * Ожидание появления пригодного устройства.
         * Возвращает серийный номер первого устройства в состоянии device
         * либо пустую строку по таймауту.
         */
        std::wstring WaitForDevice(uint32_t timeoutMs) noexcept;

        /*
         * Создание форварда hostPort -> devicePort.
         * Перед созданием снимает возможный «висячий» форвард с прошлого
         * запуска: adb не считает повторное создание ошибкой, но старая
         * запись может указывать на другое устройство.
         */
        bool CreateForward(uint16_t hostPort, uint16_t devicePort,
                           const std::wstring& serial) noexcept;

        /* Снятие ранее созданного форварда. Идемпотентна. */
        void RemoveForward() noexcept;

        /* Запуск сервера adb (adb start-server) - ускоряет первое соединение. */
        bool StartServer() noexcept;

    private:
        /*
         * Запуск adb с аргументами и захватом stdout/stderr.
         * Возвращает код возврата процесса; вывод кладётся в output.
         * Реализовано через анонимный канал, а не через system(): нам нужен
         * вывод, и нельзя показывать пользователю мигающее окно консоли.
         */
        int RunAdb(const std::wstring& arguments, std::wstring& output,
                   uint32_t timeoutMs = 10000) noexcept;

        /* Проверка существования файла. */
        static bool FileExists(const std::wstring& path) noexcept;

        /* Сбор списка мест, где может лежать adb.exe. */
        static std::vector<std::wstring> BuildSearchPaths() noexcept;

    private:
        std::wstring m_adbPath;
        std::wstring m_serial;
        uint16_t m_hostPort = 0;
        bool m_forwardActive = false;
    };
}
