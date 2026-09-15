#include "IpcServer.h"
#include "Direct3DDevice.h"
#include "Trace.h"

#include <sddl.h>
#include <strsafe.h>

using Microsoft::WRL::ComPtr;

namespace pm
{
    namespace
    {
        /* Индексы в массиве ожидания служебного потока. Держим их в enum,
           чтобы switch по результату WaitForMultipleObjects читался однозначно. */
        enum ServiceWaitSlot : DWORD
        {
            WaitShutdown = 0,
            WaitHandleRequest = 1,
            WaitModeRequest = 2,
            WaitCount = 3
        };

        /* Текущее значение счётчика производительности.
           Частота читается один раз - она неизменна с момента загрузки системы. */
        inline uint64_t QueryQpc() noexcept
        {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            return static_cast<uint64_t>(counter.QuadPart);
        }
    }

    IpcServer::IpcServer() noexcept
    {
        ZeroMemory(m_slotHandles, sizeof(m_slotHandles));
    }

    IpcServer::~IpcServer() noexcept
    {
        Stop();
    }

    // -------------------------------------------------------------------------
    //  Создание объектов ядра
    // -------------------------------------------------------------------------
    HRESULT IpcServer::CreateKernelObjects() noexcept
    {
        // Строим дескриптор безопасности из SDDL. Без него объекты, созданные
        // в сессии 0 из-под SYSTEM, будут недоступны пользовательскому процессу:
        // DACL по умолчанию не содержит записи для интерактивного пользователя.
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                PM_SDDL_SHARED, SDDL_REVISION_1, &m_securityDescriptor, nullptr))
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"Не удалось разобрать SDDL для общих объектов, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        m_securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        m_securityAttributes.lpSecurityDescriptor = m_securityDescriptor;
        m_securityAttributes.bInheritHandle = FALSE;

        // --- Секция разделяемой памяти ---------------------------------------
        m_section = CreateFileMappingW(
            INVALID_HANDLE_VALUE,       // подложка - системный файл подкачки
            &m_securityAttributes,
            PAGE_READWRITE,
            0,                          // старшие 32 бита размера
            PM_SECTION_SIZE,            // младшие 32 бита размера
            PM_NAME_SECTION);

        if (m_section == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"CreateFileMappingW не смог создать секцию, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        // Важно отличить «создали» от «открыли существующую»: вторая ситуация
        // означает, что в системе уже живёт другой экземпляр драйвера.
        const bool alreadyExisted = (GetLastError() == ERROR_ALREADY_EXISTS);
        if (alreadyExisted)
        {
            PM_LOG_WARN(L"Секция разделяемой памяти уже существовала - вероятно, остался прошлый экземпляр драйвера");
        }

        m_header = static_cast<PmSharedHeader*>(
            MapViewOfFile(m_section, FILE_MAP_ALL_ACCESS, 0, 0, PM_SECTION_SIZE));
        if (m_header == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"MapViewOfFile не смог отобразить секцию, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        // Полная очистка: секция из файла подкачки приходит обнулённой только
        // при первом создании, поэтому чистим явно.
        ZeroMemory(m_header, sizeof(PmSharedHeader));

        m_header->magic = PM_IPC_MAGIC;
        m_header->version = PM_IPC_VERSION;
        m_header->headerSize = static_cast<uint32_t>(sizeof(PmSharedHeader));
        m_header->ringSlots = PM_RING_SLOTS;
        m_header->driverPid = GetCurrentProcessId();
        m_header->pipelineState = PM_STATE_IDLE;

        // Барьер: magic должен стать видимым читателю ПОСЛЕ всех остальных
        // полей шапки, иначе стример увидит корректную сигнатуру при мусорном
        // headerSize и попытается работать с несовместимой раскладкой.
        MemoryBarrier();
        m_header->driverAlive = 1;

        // --- Мьютекс и события ------------------------------------------------
        m_mutex = CreateMutexW(&m_securityAttributes, FALSE, PM_NAME_MUTEX);
        if (m_mutex == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"Не удалось создать именованный мьютекс, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        struct { HANDLE* target; const wchar_t* name; BOOL manualReset; } events[] = {
            { &m_evtFrame,         PM_NAME_EVT_FRAME,   FALSE },
            { &m_evtModeRequest,   PM_NAME_EVT_MODEREQ, FALSE },
            { &m_evtHandleRequest, PM_NAME_EVT_HNDREQ,  FALSE },
            { &m_evtHandleReady,   PM_NAME_EVT_HNDRDY,  FALSE },
        };

        for (const auto& e : events)
        {
            *e.target = CreateEventW(&m_securityAttributes, e.manualReset, FALSE, e.name);
            if (*e.target == nullptr)
            {
                const DWORD err = GetLastError();
                PM_LOG_ERROR(L"Не удалось создать событие %s, код ошибки %lu", e.name, err);
                return HRESULT_FROM_WIN32(err);
            }
        }

        // Событие останова - безымянное и с ручным сбросом: его ждёт только
        // наш собственный служебный поток, снаружи оно никому не нужно.
        m_evtShutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_evtShutdown == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"Не удалось создать событие останова, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        PM_LOG_INFO(L"Объекты IPC созданы: секция %u байт, версия контракта %u",
            PM_SECTION_SIZE, PM_IPC_VERSION);
        return S_OK;
    }

    // -------------------------------------------------------------------------
    //  Запуск / останов
    // -------------------------------------------------------------------------
    HRESULT IpcServer::Start(ModeRequestCallback onModeRequest) noexcept
    {
        if (InterlockedCompareExchange(&m_running, 1, 0) != 0)
        {
            PM_LOG_WARN(L"IpcServer::Start вызван повторно - игнорируем");
            return S_FALSE;
        }

        InitializeCriticalSection(&m_resourceLock);
        m_lockInitialized = true;

        m_onModeRequest = std::move(onModeRequest);

        const HRESULT hr = CreateKernelObjects();
        if (FAILED(hr))
        {
            Stop();
            return hr;
        }

        m_serviceThread = CreateThread(nullptr, 0, &IpcServer::ServiceThreadProc, this, 0, nullptr);
        if (m_serviceThread == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"Не удалось запустить служебный поток IPC, код ошибки %lu", err);
            Stop();
            return HRESULT_FROM_WIN32(err);
        }

        PM_LOG_INFO(L"Служебный поток IPC запущен");
        return S_OK;
    }

    void IpcServer::Stop() noexcept
    {
        if (InterlockedCompareExchange(&m_running, 0, 1) == 1)
        {
            // Сообщаем стримеру, что ресурсы вот-вот исчезнут: он должен
            // немедленно прекратить обращения к общим текстурам.
            if (m_header != nullptr)
            {
                m_header->pipelineState = PM_STATE_SHUTDOWN;
                m_header->driverAlive = 0;
                MemoryBarrier();
            }
            if (m_evtFrame != nullptr)
            {
                // Будим стримера, чтобы он не висел на ожидании кадра.
                SetEvent(m_evtFrame);
            }
        }

        if (m_evtShutdown != nullptr)
        {
            SetEvent(m_evtShutdown);
        }

        if (m_serviceThread != nullptr)
        {
            // 3 секунды - с запасом: служебный поток не выполняет блокирующих
            // операций длиннее одного WaitForMultipleObjects.
            if (WaitForSingleObject(m_serviceThread, 3000) == WAIT_TIMEOUT)
            {
                PM_LOG_WARN(L"Служебный поток IPC не завершился за 3 с, оставляем его на произвол загрузчика");
            }
            CloseHandle(m_serviceThread);
            m_serviceThread = nullptr;
        }

        DetachDevice();

        HANDLE* handles[] = {
            &m_evtShutdown, &m_evtHandleReady, &m_evtHandleRequest,
            &m_evtModeRequest, &m_evtFrame, &m_mutex
        };
        for (HANDLE* h : handles)
        {
            if (*h != nullptr)
            {
                CloseHandle(*h);
                *h = nullptr;
            }
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
        if (m_securityDescriptor != nullptr)
        {
            LocalFree(m_securityDescriptor);
            m_securityDescriptor = nullptr;
        }
        if (m_lockInitialized)
        {
            DeleteCriticalSection(&m_resourceLock);
            m_lockInitialized = false;
        }

        PM_LOG_INFO(L"IPC-сервер остановлен, все объекты освобождены");
    }

    // -------------------------------------------------------------------------
    //  Привязка устройства и общие ресурсы
    // -------------------------------------------------------------------------
    HRESULT IpcServer::AttachDevice(Direct3DDevice* device) noexcept
    {
        if (device == nullptr || !device->IsValid())
        {
            PM_LOG_ERROR(L"AttachDevice получил непригодное устройство D3D11");
            return E_INVALIDARG;
        }

        EnterCriticalSection(&m_resourceLock);
        m_device = device;
        const HRESULT hr = CreateSharedFence();
        LeaveCriticalSection(&m_resourceLock);

        if (SUCCEEDED(hr))
        {
            PM_LOG_INFO(L"Устройство D3D11 привязано к IPC, общий фенс создан");
        }
        return hr;
    }

    void IpcServer::DetachDevice() noexcept
    {
        if (!m_lockInitialized)
        {
            return;
        }

        EnterCriticalSection(&m_resourceLock);

        ReleaseSharedTexturesLocked();

        m_fence.Reset();
        if (m_fenceLocalHandle != nullptr)
        {
            CloseHandle(m_fenceLocalHandle);
            m_fenceLocalHandle = nullptr;
        }
        m_fenceCounter = 0;
        m_device = nullptr;

        if (m_header != nullptr)
        {
            // Обнуляем серию: стример увидит 0 и поймёт, что текстур больше нет.
            m_header->textureSerial = 0;
            m_header->fenceHandleSerial = 0;
            m_header->fenceHandleValue = 0;
            m_header->fenceHandleOwnerPid = 0;
            MemoryBarrier();
        }

        LeaveCriticalSection(&m_resourceLock);
    }

    HRESULT IpcServer::CreateSharedFence() noexcept
    {
        if (m_device == nullptr)
        {
            return E_NOT_VALID_STATE;
        }

        // D3D11_FENCE_FLAG_SHARED обязателен, иначе CreateSharedHandle откажет.
        HRESULT hr = m_device->Device5()->CreateFence(
            0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"CreateFence завершился ошибкой 0x%08X", hr);
            return hr;
        }

        // Хендл создаётся БЕЗ имени: именованные фенсы нельзя открыть по имени
        // (в D3D11 нет OpenSharedFenceByName), поэтому единственный рабочий
        // путь - дублирование хендла в процесс стримера через DuplicateHandle.
        hr = m_fence->CreateSharedHandle(
            &m_securityAttributes, GENERIC_ALL, nullptr, &m_fenceLocalHandle);
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Не удалось создать общий хендл фенса, HRESULT=0x%08X", hr);
            m_fence.Reset();
            return hr;
        }

        m_fenceCounter = 0;
        PM_LOG_INFO(L"Общий фенс синхронизации готов");
        return S_OK;
    }

    void IpcServer::ReleaseSharedTexturesLocked() noexcept
    {
        for (uint32_t i = 0; i < PM_RING_SLOTS; ++i)
        {
            m_slotTextures[i].Reset();
            if (m_slotHandles[i] != nullptr)
            {
                // Закрытие хендла уничтожает имя объекта: стример, который
                // попробует открыть его после этого, получит ERROR_FILE_NOT_FOUND.
                CloseHandle(m_slotHandles[i]);
                m_slotHandles[i] = nullptr;
            }
            if (m_header != nullptr)
            {
                m_header->slots[i].valid = 0;
            }
        }
        m_texWidth = 0;
        m_texHeight = 0;
        m_nextSlot = 0;
    }

    HRESULT IpcServer::EnsureSharedTextures(uint32_t width, uint32_t height) noexcept
    {
        if (width == 0 || height == 0)
        {
            return E_INVALIDARG;
        }

        EnterCriticalSection(&m_resourceLock);

        // Уже готово под нужный размер - выходим, не трогая GPU.
        if (m_texWidth == width && m_texHeight == height && m_slotTextures[0])
        {
            LeaveCriticalSection(&m_resourceLock);
            return S_FALSE;
        }

        if (m_device == nullptr)
        {
            LeaveCriticalSection(&m_resourceLock);
            PM_LOG_ERROR(L"EnsureSharedTextures вызван до привязки устройства D3D11");
            return E_NOT_VALID_STATE;
        }

        PM_LOG_INFO(L"Пересоздание кольца общих текстур: %ux%u -> %ux%u",
            m_texWidth, m_texHeight, width, height);

        ReleaseSharedTexturesLocked();

        // Новая серия имён. Инкремент ДО создания: стример, увидев чужую серию,
        // просто переоткроет ресурсы, а старые имена уже закрыты.
        ++m_textureSerial;
        if (m_textureSerial == 0)
        {
            m_textureSerial = 1; // ноль зарезервирован под «ресурсов нет»
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // BGRA - нативный формат рабочего стола; любая другая раскладка
        // добавила бы конверсию на каждом кадре.
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // SHADER_RESOURCE нужен стримеру: аппаратные кодировщики принимают
        // текстуру как SRV, а без этого флага он не сможет создать вид.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        // Пара флагов SHARED | SHARED_NTHANDLE - единственная комбинация,
        // при которой работает CreateSharedHandle с именем и последующий
        // OpenSharedResourceByName на стороне стримера.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        HRESULT hr = S_OK;
        for (uint32_t i = 0; i < PM_RING_SLOTS; ++i)
        {
            hr = m_device->Device()->CreateTexture2D(&desc, nullptr, &m_slotTextures[i]);
            if (FAILED(hr))
            {
                PM_LOG_ERROR(L"CreateTexture2D для слота %u завершился ошибкой 0x%08X", i, hr);
                break;
            }

            ComPtr<IDXGIResource1> dxgiResource;
            hr = m_slotTextures[i].As(&dxgiResource);
            if (FAILED(hr))
            {
                PM_LOG_ERROR(L"Текстура слота %u не отдаёт IDXGIResource1, HRESULT=0x%08X", i, hr);
                break;
            }

            wchar_t sharedName[PM_MAX_TEX_NAME];
            (void)StringCchPrintfW(sharedName, PM_MAX_TEX_NAME, PM_FMT_TEXTURE, m_textureSerial, i);

            hr = dxgiResource->CreateSharedHandle(
                &m_securityAttributes,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                sharedName,
                &m_slotHandles[i]);
            if (FAILED(hr))
            {
                PM_LOG_ERROR(L"CreateSharedHandle для слота %u (%s) завершился ошибкой 0x%08X",
                    i, sharedName, hr);
                break;
            }

            // Заполняем описание слота ДО того, как объявим его валидным.
            m_header->slots[i].width = width;
            m_header->slots[i].height = height;
            m_header->slots[i].dxgiFormat = static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
            (void)StringCchCopyW(const_cast<wchar_t*>(m_header->slots[i].name), PM_MAX_TEX_NAME, sharedName);
            MemoryBarrier();
            m_header->slots[i].valid = 1;

            PM_LOG_TRACE(L"Слот %u готов: %s", i, sharedName);
        }

        if (FAILED(hr))
        {
            ReleaseSharedTexturesLocked();
            LeaveCriticalSection(&m_resourceLock);
            return hr;
        }

        m_texWidth = width;
        m_texHeight = height;
        m_nextSlot = 0;

        // Публикуем серию последней: до этого момента стример не имеет права
        // открывать ни одно из имён.
        MemoryBarrier();
        m_header->textureSerial = m_textureSerial;

        LeaveCriticalSection(&m_resourceLock);

        PM_LOG_INFO(L"Кольцо из %u общих текстур %ux%u создано, серия %u",
            PM_RING_SLOTS, width, height, m_textureSerial);
        return S_OK;
    }

    // -------------------------------------------------------------------------
    //  Публикация кадра
    // -------------------------------------------------------------------------
    HRESULT IpcServer::PublishFrame(ID3D11Texture2D* source) noexcept
    {
        if (source == nullptr)
        {
            return E_INVALIDARG;
        }
        if (m_header == nullptr || m_running == 0)
        {
            return E_NOT_VALID_STATE;
        }

        D3D11_TEXTURE2D_DESC srcDesc{};
        source->GetDesc(&srcDesc);

        // Ленивое создание/пересоздание кольца: размер кадра мог измениться
        // из-за смены режима, а отдельного уведомления об этом нет.
        HRESULT hr = EnsureSharedTextures(srcDesc.Width, srcDesc.Height);
        if (FAILED(hr))
        {
            return hr;
        }

        EnterCriticalSection(&m_resourceLock);

        if (m_device == nullptr || !m_fence || !m_slotTextures[m_nextSlot])
        {
            LeaveCriticalSection(&m_resourceLock);
            return E_NOT_VALID_STATE;
        }

        const uint32_t slot = m_nextSlot;

        // Форматы обязаны совпадать: CopyResource не умеет конвертировать.
        D3D11_TEXTURE2D_DESC dstDesc{};
        m_slotTextures[slot]->GetDesc(&dstDesc);
        if (dstDesc.Format != srcDesc.Format ||
            dstDesc.Width != srcDesc.Width ||
            dstDesc.Height != srcDesc.Height)
        {
            LeaveCriticalSection(&m_resourceLock);
            PM_LOG_WARN(L"Несовпадение геометрии кадра: источник %ux%u формат %u, слот %ux%u формат %u",
                srcDesc.Width, srcDesc.Height, srcDesc.Format,
                dstDesc.Width, dstDesc.Height, dstDesc.Format);
            return E_UNEXPECTED;
        }

        // Копия целиком: swapchain отдаёт поверхность в состоянии, пригодном
        // для чтения GPU, а CopyResource на одном устройстве - это чистая
        // блитовая операция без синхронизации с CPU.
        m_device->Context()->CopyResource(m_slotTextures[slot].Get(), source);

        // Значение фенса увеличиваем ПЕРЕД Signal: стример будет ждать именно
        // этого числа, и оно должно быть строго больше предыдущего.
        const uint64_t fenceValue = ++m_fenceCounter;
        hr = m_device->Context4()->Signal(m_fence.Get(), fenceValue);
        if (FAILED(hr))
        {
            LeaveCriticalSection(&m_resourceLock);
            PM_LOG_ERROR(L"Signal по фенсу завершился ошибкой 0x%08X", hr);
            return hr;
        }

        // Немедленный сброс командного буфера. Без Flush копия может пролежать
        // в очереди драйвера десятки миллисекунд - ровно то, чего мы избегаем.
        m_device->Context()->Flush();

        m_nextSlot = (m_nextSlot + 1) % PM_RING_SLOTS;

        LeaveCriticalSection(&m_resourceLock);

        // --- Публикация метаданных кадра ------------------------------------
        // Порядок критичен: сначала всё описание, затем барьер, затем frameIndex.
        const uint64_t previousIndex = m_header->frameIndex;
        const uint64_t consumed = m_header->streamerFramesConsumed;

        m_header->slot = slot;
        m_header->fenceValue = fenceValue;
        m_header->presentQpc = QueryQpc();
        MemoryBarrier();
        m_header->frameIndex = previousIndex + 1;
        m_header->driverFramesProduced = previousIndex + 1;

        // Оценка потерь: сколько кадров стример пропустил, не успев забрать.
        if (previousIndex > consumed)
        {
            m_header->droppedFrames = static_cast<uint32_t>(previousIndex - consumed);
        }

        // Будим стримера. Событие с авто-сбросом: если он уже проснулся и
        // крутится в цикле, лишний SetEvent просто останется взведённым.
        SetEvent(m_evtFrame);

        PM_LOG_TRACE(L"Кадр %llu опубликован в слот %u, фенс %llu",
            static_cast<unsigned long long>(previousIndex + 1), slot,
            static_cast<unsigned long long>(fenceValue));

        return S_OK;
    }

    // -------------------------------------------------------------------------
    //  Служебный поток
    // -------------------------------------------------------------------------
    DWORD WINAPI IpcServer::ServiceThreadProc(LPVOID param) noexcept
    {
        auto* self = static_cast<IpcServer*>(param);
        self->ServiceLoop();
        return 0;
    }

    void IpcServer::ServiceLoop() noexcept
    {
        HANDLE waits[WaitCount];
        waits[WaitShutdown] = m_evtShutdown;
        waits[WaitHandleRequest] = m_evtHandleRequest;
        waits[WaitModeRequest] = m_evtModeRequest;

        PM_LOG_INFO(L"Служебный цикл IPC вошёл в ожидание событий");

        for (;;)
        {
            const DWORD result = WaitForMultipleObjects(WaitCount, waits, FALSE, INFINITE);

            if (result == WAIT_OBJECT_0 + WaitShutdown)
            {
                PM_LOG_INFO(L"Служебный цикл IPC получил команду останова");
                break;
            }
            else if (result == WAIT_OBJECT_0 + WaitHandleRequest)
            {
                HandleFenceHandleRequest();
            }
            else if (result == WAIT_OBJECT_0 + WaitModeRequest)
            {
                HandleModeRequest();
            }
            else if (result == WAIT_FAILED)
            {
                PM_LOG_ERROR(L"WaitForMultipleObjects в служебном цикле упал, код ошибки %lu", GetLastError());
                break;
            }
        }
    }

    void IpcServer::HandleFenceHandleRequest() noexcept
    {
        const DWORD streamerPid = m_header->streamerPid;
        if (streamerPid == 0)
        {
            PM_LOG_WARN(L"Запрос хендла фенса без указанного PID стримера - игнорируем");
            return;
        }

        EnterCriticalSection(&m_resourceLock);
        HANDLE localFence = m_fenceLocalHandle;
        LeaveCriticalSection(&m_resourceLock);

        if (localFence == nullptr)
        {
            PM_LOG_WARN(L"Стример (PID %lu) запросил фенс, но устройство ещё не привязано", streamerPid);
            // Всё равно взводим событие: пусть стример получит нулевой хендл,
            // поймёт, что не готово, и повторит запрос позже.
            m_header->fenceHandleValue = 0;
            m_header->fenceHandleOwnerPid = 0;
            MemoryBarrier();
            ++m_header->fenceHandleSerial;
            SetEvent(m_evtHandleReady);
            return;
        }

        // PROCESS_DUP_HANDLE - минимально необходимое право. Драйвер работает
        // с более высокими привилегиями, чем стример, поэтому дублирование
        // делаем именно отсюда: обратное направление потребовало бы от стримера
        // права на открытие WUDFHost.exe, что в общем случае недоступно.
        HANDLE targetProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, streamerPid);
        if (targetProcess == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"OpenProcess(PID %lu) для дублирования фенса не удался, код ошибки %lu",
                streamerPid, err);
            return;
        }

        HANDLE duplicated = nullptr;
        const BOOL ok = DuplicateHandle(
            GetCurrentProcess(), localFence,
            targetProcess, &duplicated,
            0, FALSE, DUPLICATE_SAME_ACCESS);

        CloseHandle(targetProcess);

        if (!ok)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"DuplicateHandle фенса в процесс %lu не удался, код ошибки %lu", streamerPid, err);
            return;
        }

        // Значение хендла валидно только в адресном пространстве стримера.
        // Пишем владельца и значение, и только потом инкрементируем серию -
        // она служит флагом готовности.
        m_header->fenceHandleValue = reinterpret_cast<uint64_t>(duplicated);
        m_header->fenceHandleOwnerPid = streamerPid;
        MemoryBarrier();
        ++m_header->fenceHandleSerial;

        SetEvent(m_evtHandleReady);

        PM_LOG_INFO(L"Хендл фенса продублирован в процесс %lu (значение 0x%llX, серия %u)",
            streamerPid, static_cast<unsigned long long>(m_header->fenceHandleValue),
            m_header->fenceHandleSerial);
    }

    void IpcServer::HandleModeRequest() noexcept
    {
        const uint32_t serial = m_header->modeRequestSerial;
        const uint32_t width = m_header->requestedWidth;
        const uint32_t height = m_header->requestedHeight;
        uint32_t refresh = m_header->requestedRefreshHz;

        if (serial == m_header->modeAppliedSerial)
        {
            // Событие пришло, но серия не изменилась - дубликат сигнала.
            return;
        }

        if (refresh == 0)
        {
            refresh = 60; // разумное значение по умолчанию
        }

        PM_LOG_INFO(L"Стример запросил режим %ux%u@%u Гц (серия %u)", width, height, refresh, serial);

        HRESULT hr = E_NOT_VALID_STATE;
        if (m_onModeRequest)
        {
            hr = m_onModeRequest(width, height, refresh);
        }
        else
        {
            PM_LOG_ERROR(L"Колбэк смены режима не установлен");
        }

        m_header->modeApplyStatus = static_cast<uint32_t>(hr);
        MemoryBarrier();
        m_header->modeAppliedSerial = serial;

        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Смена режима на %ux%u не удалась, HRESULT=0x%08X", width, height, hr);
        }
    }

    // -------------------------------------------------------------------------
    //  Мелкие аксессоры
    // -------------------------------------------------------------------------
    void IpcServer::SetActiveMode(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept
    {
        if (m_header == nullptr)
        {
            return;
        }
        m_header->activeWidth = width;
        m_header->activeHeight = height;
        m_header->activeRefreshHz = refreshHz;
        MemoryBarrier();
        PM_LOG_INFO(L"Активный режим виртуального монитора: %ux%u@%u Гц", width, height, refreshHz);
    }

    void IpcServer::SetPipelineState(uint32_t state) noexcept
    {
        if (m_header != nullptr)
        {
            m_header->pipelineState = state;
            MemoryBarrier();
        }
    }

    uint32_t IpcServer::QualityFlags() const noexcept
    {
        return (m_header != nullptr) ? m_header->qualityFlags : 0u;
    }

    bool IpcServer::IsStreamerAttached() const noexcept
    {
        return (m_header != nullptr) && (m_header->streamerPid != 0);
    }
}
