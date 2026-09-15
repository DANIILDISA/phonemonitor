#include "IpcClient.h"
#include "../Log.h"

#include <vector>

using Microsoft::WRL::ComPtr;

namespace pm
{
    namespace
    {
        /* Шаг опроса при ожидании появления секции драйвера. */
        constexpr uint32_t kConnectPollMs = 250;
    }

    IpcClient::IpcClient() noexcept = default;

    IpcClient::~IpcClient() noexcept
    {
        Disconnect();
    }

    // -------------------------------------------------------------------------
    HRESULT IpcClient::Connect(uint32_t timeoutMs) noexcept
    {
        HRESULT hr = OpenKernelObjects(timeoutMs);
        if (FAILED(hr))
        {
            return hr;
        }

        // Регистрируемся: драйвер по ненулевому streamerPid понимает, что
        // копировать кадры теперь имеет смысл.
        m_header->streamerPid = GetCurrentProcessId();
        MemoryBarrier();

        PM_INFO("Подключение к драйверу установлено. PID драйвера: %u, состояние конвейера: %u",
            m_header->driverPid, m_header->pipelineState);

        hr = CreateDeviceOnDriverAdapter();
        if (FAILED(hr))
        {
            PM_WARN("Устройство D3D11 пока не создано - вероятно, драйвер ещё не получил swapchain");
            // Не фатально: адаптер будет найден при первом кадре.
        }

        hr = AcquireSharedFence(3000);
        if (FAILED(hr))
        {
            PM_WARN("Общий фенс пока недоступен, повторим при получении кадра");
        }

        return S_OK;
    }

    HRESULT IpcClient::OpenKernelObjects(uint32_t timeoutMs) noexcept
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;

        for (;;)
        {
            m_section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PM_NAME_SECTION);
            if (m_section != nullptr)
            {
                break;
            }

            if (GetTickCount64() >= deadline)
            {
                PM_ERROR("Секция разделяемой памяти не появилась за %u мс. "
                         "Проверьте, установлен и запущен ли драйвер PhoneMonitor IDD", timeoutMs);
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }

            PM_TRACE("Ожидание драйвера...");
            Sleep(kConnectPollMs);
        }

        m_header = static_cast<PmSharedHeader*>(
            MapViewOfFile(m_section, FILE_MAP_ALL_ACCESS, 0, 0, PM_SECTION_SIZE));
        if (m_header == nullptr)
        {
            const DWORD err = GetLastError();
            PM_ERROR("MapViewOfFile не смог отобразить секцию, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        // Проверка контракта. Несовпадение версии - жёсткая ошибка: молчаливая
        // работа с чужой раскладкой структуры приводит к порче памяти.
        if (m_header->magic != PM_IPC_MAGIC)
        {
            PM_ERROR("Неверная сигнатура разделяемой памяти: 0x%08X вместо 0x%08X",
                m_header->magic, PM_IPC_MAGIC);
            return E_UNEXPECTED;
        }
        if (m_header->version != PM_IPC_VERSION)
        {
            PM_ERROR("Несовместимая версия IPC: драйвер %u, стример %u. "
                     "Пересоберите оба компонента из одного дерева исходников",
                m_header->version, PM_IPC_VERSION);
            return E_UNEXPECTED;
        }
        if (m_header->headerSize != sizeof(PmSharedHeader))
        {
            PM_ERROR("Размер заголовка не совпал: драйвер %u байт, стример %zu байт",
                m_header->headerSize, sizeof(PmSharedHeader));
            return E_UNEXPECTED;
        }
        if (m_header->driverAlive == 0)
        {
            PM_ERROR("Драйвер пометил себя как неактивный");
            return E_NOT_VALID_STATE;
        }

        struct { HANDLE* target; const wchar_t* name; } objects[] = {
            { &m_evtFrame,         PM_NAME_EVT_FRAME   },
            { &m_evtModeRequest,   PM_NAME_EVT_MODEREQ },
            { &m_evtHandleRequest, PM_NAME_EVT_HNDREQ  },
            { &m_evtHandleReady,   PM_NAME_EVT_HNDRDY  },
        };

        for (const auto& obj : objects)
        {
            *obj.target = OpenEventW(EVENT_ALL_ACCESS, FALSE, obj.name);
            if (*obj.target == nullptr)
            {
                const DWORD err = GetLastError();
                PM_ERROR("Не удалось открыть событие %S, код ошибки %lu", obj.name, err);
                return HRESULT_FROM_WIN32(err);
            }
        }

        m_mutex = OpenMutexW(SYNCHRONIZE, FALSE, PM_NAME_MUTEX);
        if (m_mutex == nullptr)
        {
            PM_WARN("Именованный мьютекс недоступен (код %lu) - работаем без него", GetLastError());
        }

        return S_OK;
    }

    void IpcClient::Disconnect() noexcept
    {
        if (m_header != nullptr)
        {
            // Снимаем регистрацию: драйвер перестанет тратить такты на копии.
            m_header->streamerPid = 0;
            m_header->qualityFlags = 0;
            MemoryBarrier();
        }

        ReleaseSharedResources();

        m_stagingTexture.Reset();
        m_context4.Reset();
        m_context.Reset();
        m_device5.Reset();
        m_device1.Reset();
        m_device.Reset();

        HANDLE* handles[] = { &m_evtHandleReady, &m_evtHandleRequest,
                              &m_evtModeRequest, &m_evtFrame, &m_mutex };
        for (HANDLE* h : handles)
        {
            if (*h != nullptr) { CloseHandle(*h); *h = nullptr; }
        }

        if (m_header != nullptr)
        {
            UnmapViewOfFile(m_header);
            m_header = nullptr;
        }
        if (m_section != nullptr)
        {
            CloseHandle(m_section);
            m_section = nullptr;
        }
    }

    void IpcClient::ReleaseSharedResources() noexcept
    {
        for (auto& tex : m_sharedTextures)
        {
            tex.Reset();
        }
        m_openedSerial = 0;

        m_fence.Reset();
        if (m_fenceHandle != nullptr)
        {
            CloseHandle(m_fenceHandle);
            m_fenceHandle = nullptr;
        }
        m_fenceSerial = 0;
    }

    // -------------------------------------------------------------------------
    //  Поиск адаптера и создание устройства
    // -------------------------------------------------------------------------
    HRESULT IpcClient::CreateDeviceOnDriverAdapter() noexcept
    {
        if (m_device)
        {
            return S_OK;
        }

        const uint32_t serial = m_header->textureSerial;
        if (serial == 0 || m_header->slots[0].valid == 0)
        {
            // Драйвер ещё не создал текстуры - определять адаптер не по чему.
            return E_PENDING;
        }

        // Имя текстуры копируем на стек: драйвер может пересоздать кольцо
        // в любой момент, и указатель в секции станет невалидным.
        wchar_t probeName[PM_MAX_TEX_NAME];
        wcsncpy_s(probeName, const_cast<const wchar_t*>(m_header->slots[0].name), PM_MAX_TEX_NAME - 1);

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            PM_ERROR("CreateDXGIFactory1 не удался: %s", HResultToText(hr));
            return hr;
        }

        // -------------------------------------------------------------------
        // Перебор адаптеров.
        //
        // Общий ресурс D3D11 можно открыть ТОЛЬКО на том же физическом
        // адаптере, где он создан. LUID драйвера нам напрямую не известен,
        // поэтому определяем его эмпирически: создаём устройство на каждом
        // адаптере по очереди и пробуем открыть текстуру нулевого слота.
        // На типовой машине это 1-2 итерации.
        // -------------------------------------------------------------------
        for (UINT index = 0; ; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            // Программный адаптер WARP кодировать нечем - пропускаем.
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            static const D3D_FEATURE_LEVEL levels[] = {
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
            };

            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                &device, nullptr, &context);

            if (FAILED(hr))
            {
                PM_TRACE("Адаптер %u (%S) не поднялся: %s", index, desc.Description, HResultToText(hr));
                continue;
            }

            ComPtr<ID3D11Device1> device1;
            if (FAILED(device.As(&device1)))
            {
                continue;
            }

            // Пробное открытие. Успех означает, что адаптер найден.
            ComPtr<ID3D11Texture2D> probe;
            hr = device1->OpenSharedResourceByName(
                probeName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&probe));

            if (FAILED(hr))
            {
                PM_TRACE("На адаптере %u (%S) общая текстура не открылась: %s",
                    index, desc.Description, HResultToText(hr));
                continue;
            }

            m_device = device;
            m_device1 = device1;
            m_context = context;
            m_adapterLuid = desc.AdapterLuid;

            if (FAILED(m_device.As(&m_device5)) || FAILED(m_context.As(&m_context4)))
            {
                PM_ERROR("Адаптер %S не поддерживает интерфейсы D3D11.4 (нужны для фенсов)",
                    desc.Description);
                m_device.Reset();
                m_device1.Reset();
                m_context.Reset();
                continue;
            }

            // Устройство трогают поток захвата и поток кодирования.
            ComPtr<ID3D11Multithread> mt;
            if (SUCCEEDED(m_context.As(&mt)))
            {
                mt->SetMultithreadProtected(TRUE);
            }

            PM_INFO("Кадры будут приниматься на адаптере: %S (LUID %08X:%08X)",
                desc.Description, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
            return S_OK;
        }

        PM_ERROR("Не найден адаптер, на котором можно открыть общие текстуры драйвера");
        return E_FAIL;
    }

    // -------------------------------------------------------------------------
    //  Фенс
    // -------------------------------------------------------------------------
    HRESULT IpcClient::AcquireSharedFence(uint32_t timeoutMs) noexcept
    {
        if (!m_device5)
        {
            return E_PENDING;
        }

        const uint32_t serialBefore = m_header->fenceHandleSerial;

        // Публикуем свой PID (драйвер дублирует хендл именно в этот процесс)
        // и просим выдать фенс.
        m_header->streamerPid = GetCurrentProcessId();
        MemoryBarrier();
        SetEvent(m_evtHandleRequest);

        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            const DWORD wait = WaitForSingleObject(m_evtHandleReady, 200);

            if (m_header->fenceHandleSerial != serialBefore &&
                m_header->fenceHandleOwnerPid == GetCurrentProcessId() &&
                m_header->fenceHandleValue != 0)
            {
                break;
            }

            if (GetTickCount64() >= deadline)
            {
                PM_ERROR("Драйвер не выдал хендл фенса за %u мс", timeoutMs);
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }

            if (wait == WAIT_FAILED)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }

        // Старый фенс больше не нужен.
        if (m_fenceHandle != nullptr)
        {
            CloseHandle(m_fenceHandle);
            m_fenceHandle = nullptr;
        }
        m_fence.Reset();

        // Значение уже является валидным хендлом в НАШЕМ адресном пространстве:
        // драйвер продублировал его через DuplicateHandle. Владение переходит
        // к нам, закрывать обязаны мы.
        m_fenceHandle = reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(m_header->fenceHandleValue));

        const HRESULT hr = m_device5->OpenSharedFence(m_fenceHandle, IID_PPV_ARGS(&m_fence));
        if (FAILED(hr))
        {
            PM_ERROR("OpenSharedFence не удался: %s", HResultToText(hr));
            CloseHandle(m_fenceHandle);
            m_fenceHandle = nullptr;
            return hr;
        }

        m_fenceSerial = m_header->fenceHandleSerial;
        PM_INFO("Общий фенс синхронизации получен от драйвера (серия %u)", m_fenceSerial);
        return S_OK;
    }

    // -------------------------------------------------------------------------
    //  Общие текстуры
    // -------------------------------------------------------------------------
    HRESULT IpcClient::RefreshSharedTextures() noexcept
    {
        const uint32_t serial = m_header->textureSerial;
        if (serial == 0)
        {
            return E_PENDING;
        }
        if (serial == m_openedSerial && m_sharedTextures[0])
        {
            return S_OK;
        }

        PM_INFO("Серия общих текстур изменилась (%u -> %u), переоткрываем ресурсы",
            m_openedSerial, serial);

        for (auto& tex : m_sharedTextures)
        {
            tex.Reset();
        }

        for (uint32_t i = 0; i < PM_RING_SLOTS; ++i)
        {
            if (m_header->slots[i].valid == 0)
            {
                PM_ERROR("Слот %u помечен драйвером как невалидный", i);
                return E_PENDING;
            }

            wchar_t name[PM_MAX_TEX_NAME];
            wcsncpy_s(name, const_cast<const wchar_t*>(m_header->slots[i].name), PM_MAX_TEX_NAME - 1);

            const HRESULT hr = m_device1->OpenSharedResourceByName(
                name, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&m_sharedTextures[i]));
            if (FAILED(hr))
            {
                PM_ERROR("Не удалось открыть общую текстуру %S: %s", name, HResultToText(hr));
                for (auto& tex : m_sharedTextures) tex.Reset();
                return hr;
            }
        }

        m_openedSerial = serial;
        PM_INFO("Открыто %u общих текстур серии %u (%ux%u)",
            PM_RING_SLOTS, serial, m_header->slots[0].width, m_header->slots[0].height);
        return S_OK;
    }

    HRESULT IpcClient::EnsureStagingTexture(uint32_t width, uint32_t height) noexcept
    {
        if (m_stagingTexture && m_stagingWidth == width && m_stagingHeight == height)
        {
            return S_OK;
        }

        m_stagingTexture.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // RENDER_TARGET нужен для конвертации BGRA -> NV12 средствами
        // видеопроцессора, SHADER_RESOURCE - для чтения кодировщиком.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        const HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
        if (FAILED(hr))
        {
            PM_ERROR("Не удалось создать приватную текстуру %ux%u: %s", width, height, HResultToText(hr));
            return hr;
        }

        m_stagingWidth = width;
        m_stagingHeight = height;
        PM_INFO("Создана приватная текстура приёма кадров: %ux%u", width, height);
        return S_OK;
    }

    // -------------------------------------------------------------------------
    //  Получение кадра
    // -------------------------------------------------------------------------
    HRESULT IpcClient::AcquireFrame(uint32_t timeoutMs, AcquiredFrame& out) noexcept
    {
        if (m_header == nullptr)
        {
            return E_NOT_VALID_STATE;
        }
        if (m_header->driverAlive == 0 || m_header->pipelineState == PM_STATE_SHUTDOWN)
        {
            PM_WARN("Драйвер сообщил о завершении работы");
            return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        }

        // Ленивая инициализация: на момент Connect драйвер мог ещё не иметь
        // ни устройства, ни текстур.
        if (!m_device)
        {
            const HRESULT hr = CreateDeviceOnDriverAdapter();
            if (FAILED(hr)) return (hr == E_PENDING) ? S_FALSE : hr;
        }

        // Ждём сигнала о новом кадре, если с прошлого раза ничего не появилось.
        if (m_header->frameIndex == m_lastFrameIndex)
        {
            const DWORD wait = WaitForSingleObject(m_evtFrame, timeoutMs);
            if (wait == WAIT_TIMEOUT)
            {
                // Норма: рабочий стол не меняется, новых кадров нет.
                return S_FALSE;
            }
            if (wait == WAIT_FAILED)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            if (m_header->frameIndex == m_lastFrameIndex)
            {
                // Событие могло быть взведено драйвером при остановке.
                return S_FALSE;
            }
        }

        HRESULT hr = RefreshSharedTextures();
        if (FAILED(hr))
        {
            return (hr == E_PENDING) ? S_FALSE : hr;
        }

        // Фенс мог быть пересоздан вместе с устройством драйвера.
        if (!m_fence || m_fenceSerial != m_header->fenceHandleSerial)
        {
            hr = AcquireSharedFence(2000);
            if (FAILED(hr))
            {
                return (hr == E_PENDING) ? S_FALSE : hr;
            }
        }

        // ---------------------------------------------------------------------
        // Согласованное чтение метаданных кадра.
        //
        // Драйвер пишет frameIndex ПОСЛЕДНИМ. Поэтому читаем его первым,
        // затем описание, затем сверяем frameIndex ещё раз: если он изменился,
        // значит драйвер успел опубликовать новый кадр прямо во время чтения,
        // и прочитанное описание может относиться уже к нему. Повторяем.
        // Ограничение в 4 попытки страхует от бесконечного цикла на машине,
        // где драйвер публикует кадры быстрее, чем мы их читаем.
        // ---------------------------------------------------------------------
        uint64_t frameIndex = 0;
        uint64_t fenceValue = 0;
        uint64_t presentQpc = 0;
        uint32_t slot = 0;
        bool consistent = false;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            frameIndex = m_header->frameIndex;
            MemoryBarrier();
            slot = m_header->slot;
            fenceValue = m_header->fenceValue;
            presentQpc = m_header->presentQpc;
            MemoryBarrier();

            if (frameIndex == m_header->frameIndex)
            {
                consistent = true;
                break;
            }
        }

        if (!consistent)
        {
            PM_TRACE("Не удалось согласованно прочитать описание кадра, пропускаем итерацию");
            return S_FALSE;
        }

        if (slot >= PM_RING_SLOTS || !m_sharedTextures[slot])
        {
            PM_WARN("Драйвер указал недопустимый слот %u", slot);
            return S_FALSE;
        }

        D3D11_TEXTURE2D_DESC desc{};
        m_sharedTextures[slot]->GetDesc(&desc);

        hr = EnsureStagingTexture(desc.Width, desc.Height);
        if (FAILED(hr))
        {
            return hr;
        }

        // ---------------------------------------------------------------------
        // Синхронизация по фенсу.
        //
        // Wait ставит в очередь НАШЕГО устройства барьер: последующие команды
        // не начнут исполняться, пока фенс не достигнет fenceValue. Это
        // синхронизация на стороне GPU - процессор здесь не блокируется, что и
        // даёт выигрыш по сравнению с ожиданием через событие.
        // ---------------------------------------------------------------------
        hr = m_context4->Wait(m_fence.Get(), fenceValue);
        if (FAILED(hr))
        {
            PM_ERROR("Ожидание фенса (значение %llu) не удалось: %s",
                static_cast<unsigned long long>(fenceValue), HResultToText(hr));
            return hr;
        }

        // Копия в приватную текстуру: после неё слот можно отдавать драйверу
        // на переиспользование, а кодировщик работает со стабильными данными.
        m_context->CopyResource(m_stagingTexture.Get(), m_sharedTextures[slot].Get());

        m_lastFrameIndex = frameIndex;

        out.texture = m_stagingTexture.Get();
        out.frameIndex = frameIndex;
        out.presentQpc = presentQpc;
        out.width = desc.Width;
        out.height = desc.Height;

        PM_TRACE("Принят кадр %llu из слота %u (%ux%u), фенс %llu",
            static_cast<unsigned long long>(frameIndex), slot, desc.Width, desc.Height,
            static_cast<unsigned long long>(fenceValue));

        return S_OK;
    }

    void IpcClient::ReleaseFrame(uint64_t frameIndex) noexcept
    {
        if (m_header != nullptr)
        {
            m_header->streamerFramesConsumed = frameIndex;
        }
    }

    // -------------------------------------------------------------------------
    //  Смена режима
    // -------------------------------------------------------------------------
    HRESULT IpcClient::RequestMode(uint32_t width, uint32_t height, uint32_t refreshHz,
                                   uint32_t timeoutMs) noexcept
    {
        if (m_header == nullptr)
        {
            return E_NOT_VALID_STATE;
        }

        // Мьютекс защищает от гонки двух одновременных запросов режима
        // (например, смена разрешения и включение теплового режима).
        const bool locked = (m_mutex != nullptr) &&
                            (WaitForSingleObject(m_mutex, 1000) == WAIT_OBJECT_0);

        m_header->requestedWidth = width;
        m_header->requestedHeight = height;
        m_header->requestedRefreshHz = refreshHz;
        MemoryBarrier();

        const uint32_t serial = m_header->modeRequestSerial + 1;
        m_header->modeRequestSerial = serial;
        MemoryBarrier();

        SetEvent(m_evtModeRequest);

        PM_INFO("Драйверу отправлен запрос режима %ux%u@%u Гц (серия %u)",
            width, height, refreshHz, serial);

        HRESULT result = S_OK;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            if (m_header->modeAppliedSerial == serial)
            {
                result = static_cast<HRESULT>(m_header->modeApplyStatus);
                break;
            }
            if (GetTickCount64() >= deadline)
            {
                PM_ERROR("Драйвер не подтвердил смену режима за %u мс", timeoutMs);
                result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                break;
            }
            Sleep(20);
        }

        if (locked)
        {
            ReleaseMutex(m_mutex);
        }

        if (SUCCEEDED(result))
        {
            // Смена режима означает пересоздание всех GPU-ресурсов драйвера:
            // старые текстуры и фенс уже недействительны.
            ReleaseSharedResources();
            m_lastFrameIndex = 0;

            PM_INFO("Режим применён драйвером. Активное разрешение: %ux%u@%u Гц",
                m_header->activeWidth, m_header->activeHeight, m_header->activeRefreshHz);
        }

        return result;
    }

    void IpcClient::SetQualityFlags(uint32_t flags) noexcept
    {
        if (m_header != nullptr)
        {
            m_header->qualityFlags = flags;
            MemoryBarrier();
        }
    }

    void IpcClient::GetActiveMode(uint32_t& width, uint32_t& height, uint32_t& refreshHz) const noexcept
    {
        if (m_header != nullptr)
        {
            width = m_header->activeWidth;
            height = m_header->activeHeight;
            refreshHz = m_header->activeRefreshHz;
        }
        else
        {
            width = height = refreshHz = 0;
        }
    }

    uint32_t IpcClient::PipelineState() const noexcept
    {
        return (m_header != nullptr) ? m_header->pipelineState : PM_STATE_SHUTDOWN;
    }

    uint32_t IpcClient::DroppedFrames() const noexcept
    {
        return (m_header != nullptr) ? m_header->droppedFrames : 0u;
    }
}
