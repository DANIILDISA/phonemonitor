#include "Driver.h"
#include "Trace.h"

#include <objbase.h>
#include <algorithm>
#include <new>

using namespace pm;

// =============================================================================
//  DriverEntry - первая точка входа драйвера.
// =============================================================================
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    PM_LOG_INFO(L"DriverEntry: инициализация драйвера PhoneMonitor IDD");

    WDF_DRIVER_CONFIG config{};
    WDF_DRIVER_CONFIG_INIT(&config, PmEvtDriverDeviceAdd);

    // Объект драйвера не несёт собственного контекста - всё состояние живёт
    // на уровне устройства, поэтому атрибуты по умолчанию.
    WDF_OBJECT_ATTRIBUTES attributes{};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    const NTSTATUS status = WdfDriverCreate(
        driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status))
    {
        PM_LOG_ERROR(L"WdfDriverCreate завершился со статусом 0x%08X", status);
        return status;
    }

    PM_LOG_INFO(L"Драйвер зарегистрирован в WDF успешно");
    return STATUS_SUCCESS;
}

// =============================================================================
//  EvtDriverDeviceAdd - на каждое обнаруженное PnP-устройство.
// =============================================================================
NTSTATUS PmEvtDriverDeviceAdd(WDFDRIVER /*driver*/, PWDFDEVICE_INIT deviceInit)
{
    PM_LOG_INFO(L"EvtDriverDeviceAdd: создание экземпляра виртуального дисплея");

    // -------------------------------------------------------------------------
    // Шаг 1. Конфигурация расширения класса IddCx.
    //
    // IddCxDeviceInitConfig ОБЯЗАН быть вызван до WdfDeviceCreate: он
    // подменяет ряд обработчиков в WDFDEVICE_INIT, и после создания устройства
    // сделать это уже невозможно.
    // -------------------------------------------------------------------------
    IDD_CX_CLIENT_CONFIG iddConfig{};
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);

    iddConfig.EvtIddCxAdapterInitFinished = PmEvtIddCxAdapterInitFinished;
    iddConfig.EvtIddCxAdapterCommitModes = PmEvtIddCxAdapterCommitModes;
    iddConfig.EvtIddCxParseMonitorDescription = PmEvtIddCxParseMonitorDescription;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = PmEvtIddCxMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes = PmEvtIddCxMonitorQueryTargetModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain = PmEvtIddCxMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain = PmEvtIddCxMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfig);
    if (!NT_SUCCESS(status))
    {
        PM_LOG_ERROR(L"IddCxDeviceInitConfig завершился со статусом 0x%08X", status);
        return status;
    }

    // -------------------------------------------------------------------------
    // Шаг 2. Обработчики питания.
    // Инициализацию адаптера делаем в D0Entry, а не здесь: на момент DeviceAdd
    // стек устройства ещё не поднят, и IddCxAdapterInitAsync вернёт ошибку.
    // -------------------------------------------------------------------------
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower{};
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDeviceD0Entry = PmEvtDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpPower);

    // -------------------------------------------------------------------------
    // Шаг 3. Создание WDFDEVICE с нашим контекстом.
    // -------------------------------------------------------------------------
    WDF_OBJECT_ATTRIBUTES attributes{};
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, IndirectDeviceContextWrapper);
    attributes.EvtCleanupCallback = PmEvtDeviceContextCleanup;

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        PM_LOG_ERROR(L"WdfDeviceCreate завершился со статусом 0x%08X", status);
        return status;
    }

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status))
    {
        PM_LOG_ERROR(L"IddCxDeviceInitialize завершился со статусом 0x%08X", status);
        return status;
    }

    // -------------------------------------------------------------------------
    // Шаг 4. Размещение C++-контекста.
    //
    // WDF выделил под обёртку сырую память без вызова конструкторов, поэтому
    // указатель инициализируем вручную, а сам объект создаём через new.
    // Освобождение - в PmEvtDeviceContextCleanup.
    // -------------------------------------------------------------------------
    auto* wrapper = PmGetDeviceContext(device);
    wrapper->pContext = new (std::nothrow) IndirectDeviceContext(device);
    if (wrapper->pContext == nullptr)
    {
        PM_LOG_ERROR(L"Не хватило памяти под контекст устройства");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PM_LOG_INFO(L"Экземпляр устройства создан");
    return STATUS_SUCCESS;
}

void PmEvtDeviceContextCleanup(WDFOBJECT object)
{
    PM_LOG_INFO(L"Очистка контекста устройства");
    auto* wrapper = PmGetDeviceContext(object);
    if (wrapper != nullptr)
    {
        wrapper->Cleanup();
    }
}

NTSTATUS PmEvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    PM_LOG_INFO(L"Переход в D0 (предыдущее состояние %d)", static_cast<int>(previousState));

    auto* wrapper = PmGetDeviceContext(device);
    if (wrapper == nullptr || wrapper->pContext == nullptr)
    {
        PM_LOG_ERROR(L"D0Entry вызван при отсутствующем контексте устройства");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // Повторный вход в D0 (например, после сна) не должен приводить к повторной
    // инициализации адаптера - InitAdapter защищён внутри от этого.
    return wrapper->pContext->InitAdapter();
}

// =============================================================================
//  Реализация IndirectDeviceContext
// =============================================================================
namespace pm
{
    IndirectDeviceContext::IndirectDeviceContext(WDFDEVICE device) noexcept
        : m_wdfDevice(device)
    {
        InitializeCriticalSection(&m_stateLock);

        // Стабильный ContainerId: генерируется один раз на экземпляр устройства.
        // Windows связывает с ним положение монитора в рабочем пространстве,
        // поэтому при смене разрешения мы переиспользуем тот же GUID.
        if (FAILED(CoCreateGuid(&m_containerId)))
        {
            // Фолбэк на фиксированный GUID: хуже, чем случайный, только тем,
            // что два экземпляра драйвера в одной системе конфликтуют.
            m_containerId = { 0x7b5d7a9c, 0x4f2e, 0x4a31,
                              { 0x9c, 0x10, 0x55, 0xa2, 0x3f, 0x18, 0x6d, 0x04 } };
            PM_LOG_WARN(L"CoCreateGuid не сработал, используется резервный ContainerId");
        }

        // Стартовая геометрия до подключения телефона. Реальные размеры придут
        // от клиента через IPC и заменят эти значения.
        m_modes = BuildModeTable(m_activeWidth, m_activeHeight, m_activeRefresh);
        m_edid = BuildEdid(m_activeWidth, m_activeHeight, m_activeRefresh);
    }

    IndirectDeviceContext::~IndirectDeviceContext() noexcept
    {
        PM_LOG_INFO(L"Уничтожение контекста устройства");

        UnassignSwapChain();
        m_ipc.Stop();
        DeleteCriticalSection(&m_stateLock);
    }

    NTSTATUS IndirectDeviceContext::InitAdapter() noexcept
    {
        if (m_adapter != nullptr)
        {
            // Уже инициализирован - повторный D0Entry после выхода из сна.
            PM_LOG_INFO(L"Адаптер уже инициализирован, повторная инициализация не требуется");
            return STATUS_SUCCESS;
        }

        // ---------------------------------------------------------------------
        // Запуск IPC-сервера ДО объявления адаптера.
        //
        // Порядок важен: стример может быть уже запущен и ждать секцию. Если
        // создать адаптер раньше секции, появится окно, в котором монитор уже
        // существует, а канал передачи кадров - ещё нет.
        // ---------------------------------------------------------------------
        const HRESULT ipcResult = m_ipc.Start(
            [this](uint32_t w, uint32_t h, uint32_t hz) -> HRESULT
            {
                // Колбэк исполняется в служебном потоке IpcServer.
                return this->ApplyDisplayMode(w, h, hz);
            });

        if (FAILED(ipcResult))
        {
            PM_LOG_ERROR(L"Не удалось поднять IPC-сервер, HRESULT=0x%08X", ipcResult);
            return STATUS_UNSUCCESSFUL;
        }

        // ---------------------------------------------------------------------
        // Описание возможностей адаптера.
        // ---------------------------------------------------------------------
        IDDCX_ADAPTER_CAPS caps{};
        caps.Size = sizeof(caps);

        // Один монитор на адаптер: телефон всегда один.
        caps.MaxMonitorsSupported = 1;

        caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
        caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
        // Тип передачи «прочее проводное»: физически это USB, но IddCx не знает
        // такой категории, а WIRED_OTHER корректно отражает отсутствие
        // ограничений по пропускной способности со стороны видеотракта.
        caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

        caps.EndPointDiagnostics.pEndPointFriendlyName = L"PhoneMonitor";
        caps.EndPointDiagnostics.pEndPointManufacturerName = L"PhoneMonitor Project";
        caps.EndPointDiagnostics.pEndPointModelName = L"USB Secondary Display";

        IDDCX_ENDPOINT_VERSION version{};
        version.Size = sizeof(version);
        version.MajorVer = 1;
        version.MinorVer = 0;
        caps.EndPointDiagnostics.pFirmwareVersion = &version;
        caps.EndPointDiagnostics.pHardwareVersion = &version;

        WDF_OBJECT_ATTRIBUTES adapterAttributes{};
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapterAttributes, IndirectDeviceContextWrapper);
        // Адаптер НЕ владеет контекстом: владелец - WDFDEVICE. Поэтому здесь
        // не назначается EvtCleanupCallback, иначе объект удалился бы дважды.

        IDARG_IN_ADAPTER_INIT adapterInit{};
        adapterInit.WdfDevice = m_wdfDevice;
        adapterInit.pCaps = &caps;
        adapterInit.ObjectAttributes = &adapterAttributes;

        IDARG_OUT_ADAPTER_INIT adapterInitOut{};
        const NTSTATUS status = IddCxAdapterInitAsync(&adapterInit, &adapterInitOut);
        if (!NT_SUCCESS(status))
        {
            PM_LOG_ERROR(L"IddCxAdapterInitAsync завершился со статусом 0x%08X", status);
            m_ipc.Stop();
            return status;
        }

        // Объект адаптера уже доступен, но фактическая готовность наступит
        // только в EvtIddCxAdapterInitFinished. Контекст прописываем сразу,
        // чтобы колбэк смог нас найти.
        m_adapter = adapterInitOut.AdapterObject;
        auto* wrapper = PmGetDeviceContext(m_adapter);
        wrapper->pContext = this;

        PM_LOG_INFO(L"Асинхронная инициализация адаптера запущена");
        return STATUS_SUCCESS;
    }

    void IndirectDeviceContext::OnAdapterInitFinished(IDDCX_ADAPTER adapter) noexcept
    {
        m_adapter = adapter;
        m_ipc.SetPipelineState(PM_STATE_IDLE);

        std::vector<MonitorMode> modes;
        EnterCriticalSection(&m_stateLock);
        modes = m_modes;
        LeaveCriticalSection(&m_stateLock);

        const HRESULT hr = CreateMonitor(modes);
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Не удалось объявить монитор системе, HRESULT=0x%08X", hr);
        }
    }

    // -------------------------------------------------------------------------
    //  Создание и снятие монитора
    // -------------------------------------------------------------------------
    HRESULT IndirectDeviceContext::CreateMonitor(const std::vector<MonitorMode>& modes) noexcept
    {
        if (m_adapter == nullptr || modes.empty())
        {
            return E_NOT_VALID_STATE;
        }

        EnterCriticalSection(&m_stateLock);
        // EDID пересобирается под предпочтительный режим: именно он попадёт
        // в свойства монитора и определит рекомендуемое разрешение в настройках.
        m_edid = BuildEdid(modes[0].width, modes[0].height, modes[0].refreshHz);
        std::vector<uint8_t> edidCopy = m_edid;
        LeaveCriticalSection(&m_stateLock);

        IDDCX_MONITOR_INFO monitorInfo{};
        monitorInfo.Size = sizeof(monitorInfo);
        // DISPLAYPORT_EXTERNAL - самый «безобидный» для композитора тип:
        // он не включает логику HDMI-аудио и не требует HDCP-переговоров.
        monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
        monitorInfo.ConnectorIndex = 0;
        monitorInfo.MonitorContainerId = m_containerId;

        monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
        monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
        monitorInfo.MonitorDescription.DataSize = static_cast<UINT>(edidCopy.size());
        monitorInfo.MonitorDescription.pData = edidCopy.data();

        WDF_OBJECT_ATTRIBUTES monitorAttributes{};
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monitorAttributes, IndirectMonitorContextWrapper);

        IDARG_IN_MONITORCREATE monitorCreate{};
        monitorCreate.ObjectAttributes = &monitorAttributes;
        monitorCreate.pMonitorInfo = &monitorInfo;

        IDARG_OUT_MONITORCREATE monitorCreateOut{};
        NTSTATUS status = IddCxMonitorCreate(m_adapter, &monitorCreate, &monitorCreateOut);
        if (!NT_SUCCESS(status))
        {
            PM_LOG_ERROR(L"IddCxMonitorCreate завершился со статусом 0x%08X", status);
            return HRESULT_FROM_NT(status);
        }

        m_monitor = monitorCreateOut.MonitorObject;
        PmGetMonitorContext(m_monitor)->pContext = this;

        // Arrival - момент, когда монитор становится виден в «Параметрах экрана».
        IDARG_OUT_MONITORARRIVAL arrivalOut{};
        status = IddCxMonitorArrival(m_monitor, &arrivalOut);
        if (!NT_SUCCESS(status))
        {
            PM_LOG_ERROR(L"IddCxMonitorArrival завершился со статусом 0x%08X", status);
            return HRESULT_FROM_NT(status);
        }

        m_monitorArrived = true;
        m_ipc.SetPipelineState(PM_STATE_ARRIVED);
        m_ipc.SetActiveMode(modes[0].width, modes[0].height, modes[0].refreshHz);

        PM_LOG_INFO(L"Виртуальный монитор объявлен системе: %ux%u@%u Гц",
            modes[0].width, modes[0].height, modes[0].refreshHz);
        return S_OK;
    }

    void IndirectDeviceContext::DestroyMonitor() noexcept
    {
        if (m_monitor == nullptr || !m_monitorArrived)
        {
            return;
        }

        // Departure отключает монитор от рабочего стола. Composer сам снимет
        // swapchain, что приведёт к вызову Unassign и остановке нашего потока.
        const NTSTATUS status = IddCxMonitorDeparture(m_monitor);
        if (!NT_SUCCESS(status))
        {
            PM_LOG_ERROR(L"IddCxMonitorDeparture завершился со статусом 0x%08X", status);
        }

        m_monitorArrived = false;
        m_monitor = nullptr;
        PM_LOG_INFO(L"Виртуальный монитор отключён от системы");
    }

    // -------------------------------------------------------------------------
    //  Смена режима по запросу клиента
    // -------------------------------------------------------------------------
    HRESULT IndirectDeviceContext::ApplyDisplayMode(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept
    {
        // Выравнивание под макроблоки H.264 делаем ЗДЕСЬ, а не в стримере:
        // так виртуальный монитор с самого начала имеет размер, который
        // кодировщик примет без подгонки и без лишнего масштабирования.
        AlignForEncoder(width, height);

        if (refreshHz == 0 || refreshHz > 240)
        {
            PM_LOG_WARN(L"Запрошена некорректная частота %u Гц, подставляем 60 Гц", refreshHz);
            refreshHz = 60;
        }

        EnterCriticalSection(&m_stateLock);
        const bool unchanged = (m_activeWidth == width &&
                                m_activeHeight == height &&
                                m_activeRefresh == refreshHz);
        LeaveCriticalSection(&m_stateLock);

        if (unchanged)
        {
            PM_LOG_INFO(L"Режим %ux%u@%u Гц уже активен - изменение не требуется",
                width, height, refreshHz);
            return S_OK;
        }

        PM_LOG_INFO(L"Применение нового режима: %ux%u@%u Гц", width, height, refreshHz);

        std::vector<MonitorMode> modes = BuildModeTable(width, height, refreshHz);

        EnterCriticalSection(&m_stateLock);
        m_modes = modes;
        m_activeWidth = width;
        m_activeHeight = height;
        m_activeRefresh = refreshHz;
        LeaveCriticalSection(&m_stateLock);

        const HRESULT hr = DisplayModesSet(modes);
        if (SUCCEEDED(hr))
        {
            m_ipc.SetActiveMode(width, height, refreshHz);
        }
        return hr;
    }

    HRESULT IndirectDeviceContext::DisplayModesSet(const std::vector<MonitorMode>& modes) noexcept
    {
        if (m_adapter == nullptr || modes.empty())
        {
            return E_NOT_VALID_STATE;
        }

#if defined(PM_HAVE_IDDCX_UPDATEMODES)
        // ---------------------------------------------------------------------
        // Быстрый путь (IddCx 1.9 и новее).
        //
        // IddCxMonitorUpdateModes заменяет список режимов монитора без его
        // отключения: рабочий стол не мигает, окна не перераскладываются.
        // Доступность проверяется на этапе сборки - макрос выставляет CMake,
        // когда в заголовках WDK есть соответствующая функция.
        // ---------------------------------------------------------------------
        if (m_monitor != nullptr && m_monitorArrived)
        {
            std::vector<IDDCX_MONITOR_MODE> iddModes;
            iddModes.reserve(modes.size());
            for (const auto& m : modes)
            {
                iddModes.push_back(CreateMonitorMode(m.width, m.height, m.refreshHz,
                    IDDCX_MONITOR_MODE_ORIGIN_DRIVER));
            }

            IDARG_IN_UPDATEMODES updateModes{};
            updateModes.MonitorModeCount = static_cast<UINT>(iddModes.size());
            updateModes.pMonitorModes = iddModes.data();
            // Причина обновления: изменились возможности приёмника (телефона).
            updateModes.Reason = IDDCX_UPDATE_REASON_OTHER;

            const NTSTATUS status = IddCxMonitorUpdateModes(m_monitor, &updateModes);
            if (NT_SUCCESS(status))
            {
                PM_LOG_INFO(L"Список режимов обновлён без переподключения монитора");
                return S_OK;
            }

            PM_LOG_WARN(L"IddCxMonitorUpdateModes вернул 0x%08X, откатываемся на переподключение", status);
        }
#endif

        // ---------------------------------------------------------------------
        // Универсальный путь: departure -> arrival.
        //
        // Работает на любой версии IddCx. Для системы это выглядит как
        // физическое переподключение кабеля: монитор исчезает и появляется
        // снова, уже с новым EDID и новым списком режимов. Занимает ~300-700 мс,
        // поэтому вызывается только при реальной смене геометрии.
        //
        // Порядок критичен: сначала снимаем swapchain (иначе поток захвата
        // продолжит работать с уже недействительным объектом), затем departure,
        // и только потом создаём монитор заново.
        // ---------------------------------------------------------------------
        UnassignSwapChain();
        DestroyMonitor();

        // Небольшая пауза даёт менеджеру дисплеев завершить обработку departure.
        // Без неё arrival иногда возвращает STATUS_GRAPHICS_ADAPTER_WAS_RESET.
        Sleep(120);

        return CreateMonitor(modes);
    }

    std::vector<MonitorMode> IndirectDeviceContext::CurrentModes() const noexcept
    {
        EnterCriticalSection(&m_stateLock);
        std::vector<MonitorMode> copy = m_modes;
        LeaveCriticalSection(&m_stateLock);
        return copy;
    }

    // -------------------------------------------------------------------------
    //  Swapchain
    // -------------------------------------------------------------------------
    NTSTATUS IndirectDeviceContext::AssignSwapChain(IDDCX_MONITOR monitor,
                                                    IDDCX_SWAPCHAIN swapChain,
                                                    LUID renderAdapter,
                                                    HANDLE newFrameEvent) noexcept
    {
        PM_LOG_INFO(L"Назначение swapchain монитору (LUID адаптера отрисовки %08X:%08X)",
            renderAdapter.HighPart, renderAdapter.LowPart);

        // Старый обработчик обязан быть остановлен до создания нового.
        UnassignSwapChain();

        m_monitor = monitor;

        // Устройство D3D создаём заново на КАЖДОЕ назначение swapchain:
        // композитор может переехать на другой GPU (гибридная графика,
        // переключение на дискретную карту), и старое устройство окажется
        // на чужом адаптере.
        m_d3d = std::make_shared<Direct3DDevice>(renderAdapter);
        HRESULT hr = m_d3d->Init();
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Не удалось создать устройство D3D11 для swapchain, HRESULT=0x%08X", hr);
            m_d3d.reset();
            // Возврат ошибки заставит IddCx повторить назначение позже.
            return STATUS_UNSUCCESSFUL;
        }

        hr = m_ipc.AttachDevice(m_d3d.get());
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"IPC не принял устройство D3D11, HRESULT=0x%08X", hr);
            m_d3d.reset();
            return STATUS_UNSUCCESSFUL;
        }

        // Заранее готовим кольцо текстур под текущий режим, чтобы первый же
        // кадр не тратил время на выделение памяти GPU.
        EnterCriticalSection(&m_stateLock);
        const uint32_t width = m_activeWidth;
        const uint32_t height = m_activeHeight;
        LeaveCriticalSection(&m_stateLock);
        (void)m_ipc.EnsureSharedTextures(width, height);

        m_swapChainProcessor = std::make_unique<SwapChainProcessor>(
            swapChain, m_d3d, newFrameEvent, this);

        hr = m_swapChainProcessor->Start();
        if (FAILED(hr))
        {
            m_swapChainProcessor.reset();
            m_ipc.DetachDevice();
            m_d3d.reset();
            return STATUS_UNSUCCESSFUL;
        }

        return STATUS_SUCCESS;
    }

    void IndirectDeviceContext::UnassignSwapChain() noexcept
    {
        if (m_swapChainProcessor)
        {
            PM_LOG_INFO(L"Снятие swapchain: останавливаем поток захвата");
            m_swapChainProcessor->Terminate();
            m_swapChainProcessor.reset();
        }

        m_ipc.DetachDevice();
        m_d3d.reset();
    }

    // -------------------------------------------------------------------------
    //  IFrameSink - горячий путь передачи кадра
    // -------------------------------------------------------------------------
    bool IndirectDeviceContext::IsSinkReady() const noexcept
    {
        // Копировать кадр имеет смысл только когда стример реально подключён:
        // иначе это чистая трата пропускной способности памяти GPU.
        return m_ipc.IsStreamerAttached();
    }

    HRESULT IndirectDeviceContext::DeviceTransmit(ID3D11Texture2D* frame) noexcept
    {
        // Единственная точка, через которую кадр покидает процесс драйвера.
        // Никакой обработки здесь нет и быть не должно - только публикация
        // в разделяемую память и сигнал фенса.
        return m_ipc.PublishFrame(frame);
    }

    void IndirectDeviceContext::OnPipelineStateChanged(uint32_t state) noexcept
    {
        m_ipc.SetPipelineState(state);
    }
}

// =============================================================================
//  Колбэки IddCx
// =============================================================================
namespace
{
    /* Достаёт контекст из любого объекта IddCx, где он был прописан. */
    IndirectDeviceContext* ContextFromAdapter(IDDCX_ADAPTER adapter) noexcept
    {
        auto* wrapper = PmGetDeviceContext(adapter);
        return (wrapper != nullptr) ? wrapper->pContext : nullptr;
    }

    IndirectDeviceContext* ContextFromMonitor(IDDCX_MONITOR monitor) noexcept
    {
        auto* wrapper = PmGetMonitorContext(monitor);
        return (wrapper != nullptr) ? wrapper->pContext : nullptr;
    }
}

NTSTATUS PmEvtIddCxAdapterInitFinished(IDDCX_ADAPTER adapter,
                                       const IDARG_IN_ADAPTER_INIT_FINISHED* args)
{
    PM_LOG_INFO(L"EvtIddCxAdapterInitFinished: статус 0x%08X", args->AdapterInitStatus);

    if (!NT_SUCCESS(args->AdapterInitStatus))
    {
        PM_LOG_ERROR(L"Инициализация адаптера не удалась, монитор создан не будет");
        return args->AdapterInitStatus;
    }

    auto* context = ContextFromAdapter(adapter);
    if (context == nullptr)
    {
        PM_LOG_ERROR(L"Контекст адаптера отсутствует");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    context->OnAdapterInitFinished(adapter);
    return STATUS_SUCCESS;
}

NTSTATUS PmEvtIddCxParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
                                           IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
    /*
     * Система просит разобрать описание монитора (наш EDID) и перечислить
     * режимы, которые из него следуют.
     *
     * Контракт вызова двухфазный:
     *   1) первый вызов приходит с pMonitorModes == nullptr - нужно вернуть
     *      только количество режимов в MonitorModeBufferOutputCount;
     *   2) второй вызов даёт буфер нужного размера - его и заполняем.
     * Нарушение этого контракта - самая частая причина, по которой монитор
     * появляется в системе, но не имеет ни одного доступного разрешения.
     */

    // Разбираем EDID ровно тем же кодом, который его сгенерировал: берём
    // предпочтительный режим из DTD #1. Это гарантирует согласованность.
    const uint8_t* edid = static_cast<const uint8_t*>(inArgs->MonitorDescription.pData);
    const UINT edidSize = inArgs->MonitorDescription.DataSize;

    if (inArgs->MonitorDescription.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID ||
        edid == nullptr || edidSize < 128)
    {
        PM_LOG_ERROR(L"ParseMonitorDescription получил некорректное описание (тип %d, размер %u)",
            static_cast<int>(inArgs->MonitorDescription.Type), edidSize);
        return STATUS_INVALID_PARAMETER;
    }

    // Извлечение активных размеров из подробного дескриптора синхронизации.
    // Раскладка описана в BuildEdid: байты 54..71, старшие биты упакованы
    // в байты 58 и 61.
    const uint32_t hActive = static_cast<uint32_t>(edid[56]) |
                             (static_cast<uint32_t>(edid[58] >> 4) << 8);
    const uint32_t vActive = static_cast<uint32_t>(edid[59]) |
                             (static_cast<uint32_t>(edid[61] >> 4) << 8);

    // Частоту берём из дескриптора пределов (байт 78 - максимум вертикальной
    // развёртки), уменьшая на заложенный при генерации запас в 15 Гц.
    uint32_t refreshHz = (edid[78] > 15) ? static_cast<uint32_t>(edid[78] - 15) : 60u;
    if (refreshHz < 24 || refreshHz > 240) refreshHz = 60;

    const std::vector<MonitorMode> modes = BuildModeTable(hActive, vActive, refreshHz);

    outArgs->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (inArgs->pMonitorModes == nullptr)
    {
        // Фаза 1: система только узнаёт размер.
        PM_LOG_TRACE(L"ParseMonitorDescription: фаза подсчёта, режимов %zu", modes.size());
        return STATUS_SUCCESS;
    }

    if (inArgs->MonitorModeBufferInputCount < modes.size())
    {
        // Буфер меньше нужного - это штатный способ системы уточнить размер.
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (size_t i = 0; i < modes.size(); ++i)
    {
        inArgs->pMonitorModes[i] = CreateMonitorMode(
            modes[i].width, modes[i].height, modes[i].refreshHz,
            IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }

    // Индекс предпочтительного режима. Ноль - тот самый, что пришёл от телефона.
    outArgs->PreferredMonitorModeIdx = 0;

    PM_LOG_INFO(L"ParseMonitorDescription: разобран EDID %ux%u@%u Гц, отдано %zu режимов",
        hActive, vActive, refreshHz, modes.size());
    return STATUS_SUCCESS;
}

NTSTATUS PmEvtIddCxMonitorGetDefaultModes(IDDCX_MONITOR monitor,
                                          const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs,
                                          IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs)
{
    /*
     * Вызывается, когда у монитора НЕТ описания (EDID) либо система хочет
     * дополнить список. У нас EDID всегда есть, но реализовать колбэк нужно:
     * иначе при некоторых сценариях горячего переподключения монитор остаётся
     * без режимов вовсе.
     */
    auto* context = ContextFromMonitor(monitor);
    const std::vector<MonitorMode> modes = (context != nullptr)
        ? context->CurrentModes()
        : BuildModeTable(1920, 1080, 60);

    outArgs->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (inArgs->pDefaultMonitorModes == nullptr)
    {
        return STATUS_SUCCESS;
    }

    if (inArgs->DefaultMonitorModeBufferInputCount < modes.size())
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (size_t i = 0; i < modes.size(); ++i)
    {
        inArgs->pDefaultMonitorModes[i] = CreateMonitorMode(
            modes[i].width, modes[i].height, modes[i].refreshHz,
            IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }

    outArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

NTSTATUS PmEvtIddCxMonitorQueryTargetModes(IDDCX_MONITOR monitor,
                                           const IDARG_IN_QUERYTARGETMODES* inArgs,
                                           IDARG_OUT_QUERYTARGETMODES* outArgs)
{
    /*
     * Система спрашивает, какие режимы поддерживает ЦЕЛЬ (видеовыход), в
     * отличие от режимов монитора. Для косвенного дисплея цель и монитор
     * совпадают, поэтому отдаём тот же список, но со структурами
     * IDDCX_TARGET_MODE, где vSyncFreqDivider обязан быть равен 1.
     */
    auto* context = ContextFromMonitor(monitor);
    const std::vector<MonitorMode> modes = (context != nullptr)
        ? context->CurrentModes()
        : BuildModeTable(1920, 1080, 60);

    outArgs->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (inArgs->pTargetModes == nullptr)
    {
        return STATUS_SUCCESS;
    }

    if (inArgs->TargetModeBufferInputCount < modes.size())
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (size_t i = 0; i < modes.size(); ++i)
    {
        inArgs->pTargetModes[i] = CreateTargetMode(
            modes[i].width, modes[i].height, modes[i].refreshHz);
    }

    PM_LOG_TRACE(L"QueryTargetModes: отдано %zu режимов цели", modes.size());
    return STATUS_SUCCESS;
}

NTSTATUS PmEvtIddCxAdapterCommitModes(IDDCX_ADAPTER adapter,
                                      const IDARG_IN_COMMITMODES* inArgs)
{
    /*
     * Финальное подтверждение выбранной конфигурации. Здесь драйвер обязан
     * либо принять набор путей, либо отказать. Мы принимаем всё: виртуальный
     * монитор не имеет аппаратных ограничений, а фактическое разрешение всё
     * равно задаётся телефоном.
     *
     * Диагностическая ценность колбэка высока: по журналу видно, какой именно
     * режим выбрал менеджер дисплеев, и совпал ли он с запрошенным.
     */
    auto* context = ContextFromAdapter(adapter);

    for (UINT i = 0; i < inArgs->PathCount; ++i)
    {
        const IDARG_IN_COMMITMODES_PATH& path = inArgs->pPaths[i];

        const LONG width = path.TargetVideoSignalInfo.targetVideoSignalInfo.activeSize.cx;
        const LONG height = path.TargetVideoSignalInfo.targetVideoSignalInfo.activeSize.cy;
        const UINT num = path.TargetVideoSignalInfo.targetVideoSignalInfo.vSyncFreq.Numerator;
        const UINT den = path.TargetVideoSignalInfo.targetVideoSignalInfo.vSyncFreq.Denominator;
        const UINT hz = (den != 0) ? (num / den) : 0;

        PM_LOG_INFO(L"CommitModes[%u]: активен %ldx%ld@%u Гц (активность пути: %s)",
            i, width, height, hz, path.Active ? L"да" : L"нет");

        if (path.Active && context != nullptr && width > 0 && height > 0)
        {
            // Синхронизируем разделяемую память с тем, что реально выбрала
            // система: стример должен кодировать ровно этот размер.
            context->OnPipelineStateChanged(PM_STATE_STREAMING);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS PmEvtIddCxMonitorAssignSwapChain(IDDCX_MONITOR monitor,
                                          const IDARG_IN_SETSWAPCHAIN* inArgs)
{
    auto* context = ContextFromMonitor(monitor);
    if (context == nullptr)
    {
        PM_LOG_ERROR(L"AssignSwapChain: контекст монитора отсутствует");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return context->AssignSwapChain(
        monitor,
        inArgs->hSwapChain,
        inArgs->RenderAdapterLuid,
        inArgs->hNextSurfaceAvailable);
}

NTSTATUS PmEvtIddCxMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
    auto* context = ContextFromMonitor(monitor);
    if (context == nullptr)
    {
        // Не ошибка: колбэк может прийти уже после разрушения контекста.
        PM_LOG_WARN(L"UnassignSwapChain: контекст монитора уже отсутствует");
        return STATUS_SUCCESS;
    }

    context->UnassignSwapChain();
    return STATUS_SUCCESS;
}
