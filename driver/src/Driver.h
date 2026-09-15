#pragma once

/*
 * =============================================================================
 *  PhoneMonitor IDD - каркас драйвера косвенного дисплея (UMDF 2.0 + IddCx).
 * =============================================================================
 *
 *  Жизненный цикл, который реализует этот файл:
 *
 *    DriverEntry
 *        -> WdfDriverCreate                (регистрация драйвера в WDF)
 *    EvtDriverDeviceAdd
 *        -> IddCxDeviceInitConfig          (подключение расширения класса IddCx)
 *        -> WdfDeviceCreate
 *        -> IddCxDeviceInitialize
 *    EvtDeviceD0Entry
 *        -> IddCxAdapterInitAsync          (асинхронная инициализация адаптера)
 *    EvtIddCxAdapterInitFinished
 *        -> IddCxMonitorCreate + IddCxMonitorArrival   (монитор появляется в системе)
 *    EvtIddCxMonitorAssignSwapChain
 *        -> запуск SwapChainProcessor      (начинается поток кадров)
 *    DeviceTransmit                        (горячий путь: кадр -> разделяемая память)
 *
 *  Смена разрешения по требованию Android-клиента идёт по отдельной ветке:
 *
 *    IpcServer (служебный поток) -> ModeRequestCallback ->
 *        IndirectDeviceContext::ApplyDisplayMode -> DisplayModesSet
 *
 *  ВАЖНОЕ ОГРАНИЧЕНИЕ ПРОЦЕССА:
 *  Весь этот код исполняется в WUDFHost.exe, который находится под надзором
 *  PnP-менеджера. Любая длительная операция здесь (кодирование видео, работа
 *  с сокетами, обращение к диску) приводит к таймауту старта устройства и
 *  принудительной перезагрузке драйвера. Поэтому драйвер делает ровно две вещи:
 *  отдаёт системе виртуальный монитор и копирует кадры в разделяемую память.
 */

#include "DriverCommon.h"

#include <memory>
#include <vector>
#include <cstdint>

#include "Direct3DDevice.h"
#include "IpcServer.h"
#include "MonitorModes.h"
#include "SwapChainProcessor.h"

namespace pm
{
    /*
     * Контекст одного экземпляра устройства.
     *
     * Объект живёт в куче C++, а WDF хранит только указатель на него внутри
     * своего контекста (см. IndirectDeviceContextWrapper). Так сделано потому,
     * что WDF-контексты - это POD-память без вызова конструкторов, а нам нужны
     * полноценные C++-объекты с деструкторами (ComPtr, std::vector, потоки).
     */
    class IndirectDeviceContext final : public IFrameSink
    {
    public:
        explicit IndirectDeviceContext(WDFDEVICE device) noexcept;
        ~IndirectDeviceContext() noexcept;

        IndirectDeviceContext(const IndirectDeviceContext&) = delete;
        IndirectDeviceContext& operator=(const IndirectDeviceContext&) = delete;

        /* Запускает асинхронную инициализацию адаптера IddCx. */
        NTSTATUS InitAdapter() noexcept;

        /* Вызывается из EvtIddCxAdapterInitFinished. */
        void OnAdapterInitFinished(IDDCX_ADAPTER adapter) noexcept;

        /* Назначение/снятие swapchain монитору. */
        NTSTATUS AssignSwapChain(IDDCX_MONITOR monitor,
                                 IDDCX_SWAPCHAIN swapChain,
                                 LUID renderAdapter,
                                 HANDLE newFrameEvent) noexcept;
        void UnassignSwapChain() noexcept;

        /* --- IFrameSink ---------------------------------------------------- */
        bool IsSinkReady() const noexcept override;
        HRESULT DeviceTransmit(ID3D11Texture2D* frame) noexcept override;
        void OnPipelineStateChanged(uint32_t state) noexcept override;

        /*
         * Применяет новый режим отображения.
         * Вызывается из служебного потока IPC, когда Android прислал геометрию
         * своего экрана. Идемпотентна: повтор с теми же размерами - no-op.
         *
         * powerConstrained различает две причины смены режима и влияет на то,
         * какое значение IDDCX_UPDATE_REASON получит система:
         *   true  - тепловой троттлинг телефона (POWER_CONSTRAINTS);
         *   false - согласование геометрии экрана (DISPLAY_LINK_BANDWIDTH).
         * Значение не декоративное: менеджер дисплеев использует его при
         * выборе стратегии перестроения рабочего стола.
         */
        HRESULT ApplyDisplayMode(uint32_t width, uint32_t height, uint32_t refreshHz,
                                 bool powerConstrained) noexcept;

        /* Текущий список режимов - нужен колбэкам QueryTargetModes и
           GetDefaultDescriptionModes, которые вызываются из потоков IddCx. */
        std::vector<MonitorMode> CurrentModes() const noexcept;

        IDDCX_ADAPTER Adapter() const noexcept { return m_adapter; }
        IDDCX_MONITOR Monitor() const noexcept { return m_monitor; }

    private:
        /*
         * Публикация нового набора режимов монитора.
         *
         * Имя намеренно повторяет терминологию задачи (IddCxMonitorDisplayModesSet).
         * Реализация выбирается на этапе компиляции:
         *   * при доступной IddCxMonitorUpdateModes режимы обновляются «на лету»
         *     без переподключения монитора;
         *   * иначе выполняется корректный цикл departure -> arrival с новым
         *     EDID. Для пользователя это выглядит как переподключение кабеля.
         *
         * ВАЖНО о контракте IddCxMonitorUpdateModes: она принимает список
         * ЦЕЛЕВЫХ режимов (IDDCX_TARGET_MODE), а не режимов монитора. Режимы
         * монитора система по-прежнему получает через EDID и колбэк
         * EvtIddCxParseMonitorDescription, поэтому при смене геометрии EDID
         * тоже обязан быть пересобран.
         */
        HRESULT DisplayModesSet(const std::vector<MonitorMode>& modes,
                                bool powerConstrained) noexcept;

        /* Создание и объявление монитора системе. */
        HRESULT CreateMonitor(const std::vector<MonitorMode>& modes) noexcept;

        /* Снятие монитора (departure). */
        void DestroyMonitor() noexcept;

    private:
        WDFDEVICE m_wdfDevice = nullptr;
        IDDCX_ADAPTER m_adapter = nullptr;
        IDDCX_MONITOR m_monitor = nullptr;

        std::shared_ptr<Direct3DDevice> m_d3d;
        std::unique_ptr<SwapChainProcessor> m_swapChainProcessor;

        IpcServer m_ipc;

        /* Текущая геометрия. Защищена m_stateLock, потому что читается из
           потоков IddCx, а пишется из служебного потока IPC. */
        mutable CRITICAL_SECTION m_stateLock{};
        std::vector<MonitorMode> m_modes;
        std::vector<uint8_t> m_edid;
        uint32_t m_activeWidth = 1920;
        uint32_t m_activeHeight = 1080;
        uint32_t m_activeRefresh = 60;

        /* Идентификатор контейнера монитора. Должен быть стабильным между
           переподключениями, иначе Windows каждый раз будет считать монитор
           новым устройством и сбрасывать его положение в рабочем пространстве. */
        GUID m_containerId{};

        bool m_monitorArrived = false;
    };

}

/*
 * Обёртки для хранения указателя в WDF-контексте.
 *
 * ВАЖНО, почему они объявлены в ГЛОБАЛЬНОМ пространстве имён, а не внутри pm:
 * макрос WDF_DECLARE_CONTEXT_TYPE_WITH_NAME склеивает имя типа с префиксами,
 * порождая идентификаторы вида _WDF_<тип>_TYPE_INFO. Если передать в него
 * квалифицированное имя pm::Wrapper, получится синтаксически недопустимое
 * _WDF_pm::Wrapper_TYPE_INFO, и файл перестаёт компилироваться. Тип контекста
 * WDF обязан быть именем без квалификации.
 *
 * Поле pContext уничтожается в EvtCleanupCallback владеющего объекта.
 */
struct IndirectDeviceContextWrapper
{
    pm::IndirectDeviceContext* pContext;

    void Cleanup() noexcept
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    pm::IndirectDeviceContext* pContext;   /* не владеем: владелец - WDFDEVICE */
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectDeviceContextWrapper, PmGetDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectMonitorContextWrapper, PmGetMonitorContext);

/* --- Точки входа, экспортируемые в WDF/IddCx ------------------------------- */

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD                       PmEvtDriverDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY                         PmEvtDeviceD0Entry;
EVT_WDF_OBJECT_CONTEXT_CLEANUP                  PmEvtDeviceContextCleanup;

EVT_IDD_CX_ADAPTER_INIT_FINISHED                PmEvtIddCxAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES                 PmEvtIddCxAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION            PmEvtIddCxParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES PmEvtIddCxMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES           PmEvtIddCxMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN             PmEvtIddCxMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN           PmEvtIddCxMonitorUnassignSwapChain;
