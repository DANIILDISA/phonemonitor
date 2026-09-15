#pragma once

/*
 * =============================================================================
 *  Клиентская половина IPC: подключение стримера к IDD-драйверу.
 * =============================================================================
 *
 *  Задачи класса:
 *    1. Открыть секцию разделяемой памяти, созданную драйвером, и проверить
 *       совместимость бинарного контракта.
 *    2. Получить у драйвера HANDLE общего фенса (через схему запрос-ответ).
 *    3. Найти тот графический адаптер, на котором драйвер создал общие
 *       текстуры, и поднять на нём собственное устройство D3D11.
 *    4. По каждому кадру: дождаться сигнала, синхронизироваться по фенсу,
 *       скопировать содержимое слота в приватную текстуру и отдать её кодеру.
 *    5. Передать драйверу запрошенное клиентом разрешение и дождаться, пока
 *       виртуальный монитор переключится.
 *
 *  Почему кадр КОПИРУЕТСЯ в приватную текстуру, а не кодируется прямо из слота:
 *  драйвер циклически переиспользует слоты. Пока кодировщик держит слот,
 *  драйвер через PM_RING_SLOTS кадров начнёт писать в него снова. Один блит
 *  (доли миллисекунды на GPU) полностью снимает вопрос разрывов картинки и
 *  делает время жизни кадра в кодировщике неограниченным.
 */

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <cstdint>

#include "pm_ipc.h"

namespace pm
{
    /* Описание успешно полученного кадра. */
    struct AcquiredFrame
    {
        ID3D11Texture2D* texture = nullptr; /* приватная текстура стримера     */
        uint64_t frameIndex = 0;            /* сквозной номер кадра            */
        uint64_t presentQpc = 0;            /* QPC публикации драйвером        */
        uint32_t width = 0;
        uint32_t height = 0;
    };

    class IpcClient
    {
    public:
        IpcClient() noexcept;
        ~IpcClient() noexcept;

        IpcClient(const IpcClient&) = delete;
        IpcClient& operator=(const IpcClient&) = delete;

        /*
         * Подключение к драйверу. Ждёт появления секции до timeoutMs миллисекунд:
         * стример часто стартует раньше, чем система поднимет виртуальный монитор.
         */
        HRESULT Connect(uint32_t timeoutMs) noexcept;

        /* Отключение: снимает регистрацию стримера и освобождает ресурсы. */
        void Disconnect() noexcept;

        bool IsConnected() const noexcept { return m_header != nullptr; }

        /*
         * Запрос смены разрешения у драйвера.
         * Блокирует вызывающий поток до применения режима либо до таймаута.
         * Возвращает S_OK, если драйвер подтвердил режим.
         */
        HRESULT RequestMode(uint32_t width, uint32_t height, uint32_t refreshHz,
                            uint32_t timeoutMs) noexcept;

        /*
         * Ожидание и получение очередного кадра.
         * Возвращает:
         *   S_OK    - кадр получен, поля out заполнены;
         *   S_FALSE - истёк таймаут, новых кадров не было (рабочий стол статичен);
         *   ошибку  - соединение потеряно либо ресурсы стали недействительны.
         */
        HRESULT AcquireFrame(uint32_t timeoutMs, AcquiredFrame& out) noexcept;

        /* Подтверждение обработки кадра: обновляет счётчик потребления. */
        void ReleaseFrame(uint64_t frameIndex) noexcept;

        /* Передача драйверу флагов качества (например, режима энергосбережения). */
        void SetQualityFlags(uint32_t flags) noexcept;

        /* Текущее активное разрешение виртуального монитора. */
        void GetActiveMode(uint32_t& width, uint32_t& height, uint32_t& refreshHz) const noexcept;

        /* Состояние конвейера на стороне драйвера (PM_STATE_*). */
        uint32_t PipelineState() const noexcept;

        /* Число кадров, потерянных из-за того, что стример не успевал. */
        uint32_t DroppedFrames() const noexcept;

        /* Устройство, на котором живут приватные текстуры кадров.
           Кодировщик обязан использовать именно его. */
        ID3D11Device* Device() const noexcept { return m_device.Get(); }
        ID3D11DeviceContext* Context() const noexcept { return m_context.Get(); }
        LUID AdapterLuid() const noexcept { return m_adapterLuid; }

    private:
        /* Открытие объектов ядра, созданных драйвером. */
        HRESULT OpenKernelObjects(uint32_t timeoutMs) noexcept;

        /* Создание устройства D3D11 на адаптере драйвера.
           Нужный адаптер определяется перебором: берётся тот, на котором
           удаётся открыть общую текстуру нулевого слота. */
        HRESULT CreateDeviceOnDriverAdapter() noexcept;

        /* Получение общего фенса от драйвера. */
        HRESULT AcquireSharedFence(uint32_t timeoutMs) noexcept;

        /* Переоткрытие общих текстур при смене серии. */
        HRESULT RefreshSharedTextures() noexcept;

        /* Создание приватной текстуры-приёмника нужного размера. */
        HRESULT EnsureStagingTexture(uint32_t width, uint32_t height) noexcept;

        void ReleaseSharedResources() noexcept;

    private:
        HANDLE m_section = nullptr;
        HANDLE m_mutex = nullptr;
        HANDLE m_evtFrame = nullptr;
        HANDLE m_evtModeRequest = nullptr;
        HANDLE m_evtHandleRequest = nullptr;
        HANDLE m_evtHandleReady = nullptr;

        PmSharedHeader* m_header = nullptr;

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11Device1> m_device1;
        Microsoft::WRL::ComPtr<ID3D11Device5> m_device5;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
        LUID m_adapterLuid{};

        /* Открытые общие текстуры драйвера, по слотам. */
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTextures[PM_RING_SLOTS];
        uint32_t m_openedSerial = 0;

        /* Общий фенс. */
        Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
        HANDLE m_fenceHandle = nullptr;
        uint32_t m_fenceSerial = 0;

        /* Приватная текстура, в которую копируется кадр перед кодированием. */
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
        uint32_t m_stagingWidth = 0;
        uint32_t m_stagingHeight = 0;

        /* Индекс последнего обработанного кадра. */
        uint64_t m_lastFrameIndex = 0;
    };
}
