#pragma once

/*
 * =============================================================================
 *  Серверная половина IPC: живёт внутри драйвера (WUDFHost.exe).
 * =============================================================================
 *
 *  Обязанности класса:
 *    1. Создать именованную секцию разделяемой памяти, мьютекс и события
 *       с такой DACL, чтобы обычный пользовательский процесс мог их открыть.
 *    2. Держать кольцо из PM_RING_SLOTS общих текстур (named NT handle),
 *       пересоздавая его при смене разрешения.
 *    3. Держать общий ID3D11Fence и уметь продублировать его HANDLE в процесс
 *       стримера по запросу.
 *    4. Публиковать кадры: копия в слот -> Signal фенса -> обновление заголовка
 *       -> SetEvent.
 *    5. Слушать запрос смены режима от стримера и пробрасывать его наверх
 *       через колбэк.
 *
 *  Потоковая модель:
 *    * Публикация кадров идёт из потока SwapChainProcessor.
 *    * Служебный поток m_serviceThread обслуживает события запроса HANDLE и
 *      запроса режима.
 *    * Пересоздание текстур защищено критической секцией m_resourceLock,
 *      потому что может конкурировать с публикацией кадра.
 */

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <functional>
#include <cstdint>

#include "../../common/pm_ipc.h"

namespace pm
{
    class Direct3DDevice;

    /* Прототип колбэка «стример просит режим WxH@Hz».
       Возвращает S_OK, если режим принят и применён. */
    using ModeRequestCallback = std::function<HRESULT(uint32_t width, uint32_t height, uint32_t refreshHz)>;

    class IpcServer
    {
    public:
        IpcServer() noexcept;
        ~IpcServer() noexcept;

        IpcServer(const IpcServer&) = delete;
        IpcServer& operator=(const IpcServer&) = delete;

        /* Создаёт все объекты ядра и запускает служебный поток.
           Вызывается один раз при инициализации адаптера. */
        HRESULT Start(ModeRequestCallback onModeRequest) noexcept;

        /* Останавливает служебный поток и освобождает ресурсы.
           Идемпотентна: повторный вызов безопасен. */
        void Stop() noexcept;

        /* Привязывает IPC к конкретному устройству D3D11.
           Вызывается при назначении swapchain: до этого момента устройства нет. */
        HRESULT AttachDevice(Direct3DDevice* device) noexcept;

        /* Отвязывает устройство и уничтожает все GPU-ресурсы (текстуры, фенс).
           Вызывается при снятии swapchain. */
        void DetachDevice() noexcept;

        /* Пересоздаёт кольцо общих текстур под новое разрешение.
           Безопасно вызывать повторно с теми же размерами - тогда это no-op. */
        HRESULT EnsureSharedTextures(uint32_t width, uint32_t height) noexcept;

        /* Публикация готового кадра.
           source - текстура, полученная из swapchain; копируется в текущий слот.
           Возвращает S_OK даже если стример не подключён (кадр просто теряется). */
        HRESULT PublishFrame(ID3D11Texture2D* source) noexcept;

        /* Обновляет поля активного режима в заголовке. */
        void SetActiveMode(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept;

        /* Обновляет состояние конвейера (PM_STATE_*). */
        void SetPipelineState(uint32_t state) noexcept;

        /* Читает флаги качества, выставленные стримером. */
        uint32_t QualityFlags() const noexcept;

        /* Подключён ли стример прямо сейчас. */
        bool IsStreamerAttached() const noexcept;

        const PmSharedHeader* Header() const noexcept { return m_header; }

    private:
        /* Тело служебного потока. */
        static DWORD WINAPI ServiceThreadProc(LPVOID param) noexcept;
        void ServiceLoop() noexcept;

        /* Обработчики конкретных событий служебного потока. */
        void HandleFenceHandleRequest() noexcept;
        void HandleModeRequest() noexcept;

        /* Создание объектов ядра с общедоступной DACL. */
        HRESULT CreateKernelObjects() noexcept;

        /* Освобождение GPU-ресурсов; вызывающий обязан держать m_resourceLock. */
        void ReleaseSharedTexturesLocked() noexcept;

        /* Создание фенса, разделяемого со стримером. */
        HRESULT CreateSharedFence() noexcept;

    private:
        /* --- Объекты ядра ------------------------------------------------- */
        HANDLE m_section = nullptr;         /* CreateFileMappingW              */
        HANDLE m_mutex = nullptr;           /* именованный мьютекс             */
        HANDLE m_evtFrame = nullptr;        /* авто-сброс: «есть новый кадр»   */
        HANDLE m_evtModeRequest = nullptr;  /* авто-сброс: «смени режим»       */
        HANDLE m_evtHandleRequest = nullptr;/* авто-сброс: «дай HANDLE фенса»  */
        HANDLE m_evtHandleReady = nullptr;  /* авто-сброс: «HANDLE готов»      */
        HANDLE m_evtShutdown = nullptr;     /* ручной сброс: остановка потока  */
        HANDLE m_serviceThread = nullptr;

        PmSharedHeader* m_header = nullptr; /* отображение секции              */

        /* --- Дескриптор безопасности --------------------------------------- */
        PSECURITY_DESCRIPTOR m_securityDescriptor = nullptr;
        SECURITY_ATTRIBUTES  m_securityAttributes{};

        /* --- GPU-ресурсы ---------------------------------------------------- */
        CRITICAL_SECTION m_resourceLock{};
        bool m_lockInitialized = false;

        Direct3DDevice* m_device = nullptr;     /* не владеем                    */

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_slotTextures[PM_RING_SLOTS];
        HANDLE m_slotHandles[PM_RING_SLOTS]{};  /* именованные NT-хендлы текстур */

        Microsoft::WRL::ComPtr<ID3D11Fence> m_fence;
        HANDLE m_fenceLocalHandle = nullptr;    /* хендл фенса в НАШЕМ процессе  */
        uint64_t m_fenceCounter = 0;            /* монотонное значение фенса     */

        uint32_t m_texWidth = 0;
        uint32_t m_texHeight = 0;
        uint32_t m_textureSerial = 0;
        uint32_t m_nextSlot = 0;

        ModeRequestCallback m_onModeRequest;
        volatile LONG m_running = 0;
    };
}
