#include "SwapChainProcessor.h"
#include "Direct3DDevice.h"
#include "IpcServer.h"
#include "Trace.h"

#include <avrt.h>

using Microsoft::WRL::ComPtr;

namespace pm
{
    namespace
    {
        /* Индексы массива ожидания в цикле захвата. */
        enum FrameWaitSlot : DWORD
        {
            WaitTerminate = 0,
            WaitNewFrame = 1,
            WaitCount = 2
        };

        /* Период вывода сводки в журнал, в кадрах.
           600 кадров - это примерно 10 секунд при 60 Гц; чаще засорять
           отладочный вывод нельзя, он сам по себе стоит тактов. */
        constexpr uint64_t kLogSummaryEvery = 600;
    }

    SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain,
                                           std::shared_ptr<Direct3DDevice> device,
                                           HANDLE newFrameEvent,
                                           IFrameSink* sink) noexcept
        : m_swapChain(swapChain)
        , m_device(std::move(device))
        , m_newFrameEvent(newFrameEvent)
        , m_sink(sink)
    {
    }

    SwapChainProcessor::~SwapChainProcessor() noexcept
    {
        Terminate();

        if (m_terminateEvent != nullptr)
        {
            CloseHandle(m_terminateEvent);
            m_terminateEvent = nullptr;
        }
    }

    HRESULT SwapChainProcessor::Start() noexcept
    {
        // Событие останова - с ручным сбросом: его должен увидеть поток
        // независимо от того, в какой точке цикла он находится.
        m_terminateEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_terminateEvent == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"Не удалось создать событие останова swapchain, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        m_thread = CreateThread(nullptr, 0, &SwapChainProcessor::ThreadProc, this, 0, nullptr);
        if (m_thread == nullptr)
        {
            const DWORD err = GetLastError();
            PM_LOG_ERROR(L"Не удалось создать поток обработки swapchain, код ошибки %lu", err);
            return HRESULT_FROM_WIN32(err);
        }

        PM_LOG_INFO(L"Поток обработки swapchain запущен");
        return S_OK;
    }

    void SwapChainProcessor::Terminate() noexcept
    {
        if (m_terminateEvent != nullptr)
        {
            SetEvent(m_terminateEvent);
        }

        if (m_thread != nullptr)
        {
            // IddCx требует, чтобы после возврата из Unassign мы гарантированно
            // не обращались к swapchain, поэтому ждём поток безусловно.
            // Таймаут 2 с страхует от зависшего драйвера GPU.
            const DWORD waitResult = WaitForSingleObject(m_thread, 2000);
            if (waitResult == WAIT_TIMEOUT)
            {
                PM_LOG_ERROR(L"Поток swapchain не завершился за 2 с - возможна утечка ресурсов");
            }
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
    }

    DWORD WINAPI SwapChainProcessor::ThreadProc(LPVOID param) noexcept
    {
        auto* self = static_cast<SwapChainProcessor*>(param);
        self->Run();
        return 0;
    }

    void SwapChainProcessor::Run() noexcept
    {
        // ---------------------------------------------------------------------
        // Приоритет потока.
        //
        // "Distribution" - штатный класс задач MMCSS для передачи видеопотока.
        // Он даёт потоку гарантированный квант процессорного времени и
        // защищает от вытеснения обычными фоновыми задачами. Если MMCSS
        // недоступен (служба Audiosrv выключена), просто работаем дальше:
        // латентность вырастет, но функциональность сохранится.
        // ---------------------------------------------------------------------
        DWORD taskIndex = 0;
        HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);
        if (avrtHandle == nullptr)
        {
            PM_LOG_WARN(L"AvSetMmThreadCharacteristics недоступен (код %lu), работаем с обычным приоритетом",
                GetLastError());
        }
        else
        {
            // Внутри класса задач поднимаем приоритет до критического:
            // захват кадра обязан вытеснять всё остальное в этом процессе.
            AvSetMmThreadPriority(avrtHandle, AVRT_PRIORITY_CRITICAL);
            PM_LOG_INFO(L"Поток swapchain переведён в класс MMCSS Distribution, приоритет CRITICAL");
        }

        // Регистрируем наше устройство D3D11 в swapchain. Без этого вызова
        // IddCxSwapChainReleaseAndAcquireBuffer вернёт STATUS_INVALID_DEVICE_STATE.
        IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
        setDevice.pDevice = m_device->Device();

        const NTSTATUS setStatus = IddCxSwapChainSetDevice(m_swapChain, &setDevice);
        if (!NT_SUCCESS(setStatus))
        {
            PM_LOG_ERROR(L"IddCxSwapChainSetDevice завершился со статусом 0x%08X", setStatus);
        }
        else
        {
            if (m_sink != nullptr)
            {
                m_sink->OnPipelineStateChanged(PM_STATE_STREAMING);
            }
            ProcessFrames();
        }

        if (m_sink != nullptr)
        {
            m_sink->OnPipelineStateChanged(PM_STATE_ARRIVED);
        }

        if (avrtHandle != nullptr)
        {
            AvRevertMmThreadCharacteristics(avrtHandle);
        }

        PM_LOG_INFO(L"Поток swapchain завершён. Обработано кадров: %llu, сбоев: %llu",
            static_cast<unsigned long long>(m_framesProcessed),
            static_cast<unsigned long long>(m_framesFailed));
    }

    void SwapChainProcessor::ProcessFrames() noexcept
    {
        HANDLE waits[WaitCount];
        waits[WaitTerminate] = m_terminateEvent;
        waits[WaitNewFrame] = m_newFrameEvent;

        for (;;)
        {
            // -----------------------------------------------------------------
            // Шаг 1. Забираем очередной буфер.
            //
            // Ключевой момент: сначала ПЫТАЕМСЯ забрать кадр без ожидания.
            // IddCx может держать уже готовый кадр в очереди, и уход в
            // WaitForMultipleObjects до попытки чтения добавил бы лишний
            // переключатель контекста (~50-200 мкс) на каждом кадре.
            // -----------------------------------------------------------------
            IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
            const NTSTATUS acquireStatus =
                IddCxSwapChainReleaseAndAcquireBuffer(m_swapChain, &buffer);

            if (acquireStatus == STATUS_PENDING)
            {
                // Новых кадров нет - рабочий стол статичен. Засыпаем на событии,
                // которое IddCx взведёт при появлении кадра.
                const DWORD waitResult = WaitForMultipleObjects(WaitCount, waits, FALSE, 16);

                if (waitResult == WAIT_OBJECT_0 + WaitTerminate)
                {
                    PM_LOG_INFO(L"Цикл захвата получил команду останова");
                    break;
                }
                // WAIT_TIMEOUT здесь - норма: 16 мс соответствует одному кадру
                // при 60 Гц. Периодическое пробуждение нужно, чтобы не пропустить
                // команду останова, если событие кадра больше не взводится.
                continue;
            }

            if (acquireStatus == E_PENDING || acquireStatus == STATUS_GRAPHICS_INDIRECT_DISPLAY_DEVICE_STOPPED)
            {
                PM_LOG_INFO(L"Косвенный дисплей остановлен системой - выходим из цикла захвата");
                break;
            }

            if (!NT_SUCCESS(acquireStatus))
            {
                ++m_framesFailed;
                PM_LOG_ERROR(L"ReleaseAndAcquireBuffer вернул статус 0x%08X", acquireStatus);
                // Ошибка захвата почти всегда означает потерю устройства
                // (смена режима, сброс драйвера GPU). Выходим: IddCx пересоздаст
                // swapchain и заново вызовет AssignSwapChain.
                break;
            }

            // -----------------------------------------------------------------
            // Шаг 2. Получаем ID3D11Texture2D из вернувшегося ресурса.
            //
            // MetaData.pSurface - это IDXGIResource; сама текстура достаётся
            // через QueryInterface. Ресурс принадлежит IddCx и действителен
            // ровно до следующего вызова ReleaseAndAcquireBuffer.
            // -----------------------------------------------------------------
            ComPtr<ID3D11Texture2D> acquiredTexture;
            HRESULT hr = E_FAIL;

            if (buffer.MetaData.pSurface != nullptr)
            {
                hr = buffer.MetaData.pSurface->QueryInterface(IID_PPV_ARGS(&acquiredTexture));
            }

            if (SUCCEEDED(hr) && acquiredTexture)
            {
                // -------------------------------------------------------------
                // Шаг 3. Публикация кадра стримеру.
                //
                // Дешёвая оптимизация: если стример не подключён, не тратим
                // такты на копирование в общую текстуру. Кадр всё равно нужно
                // забрать и отпустить, иначе композитор заблокируется.
                // -------------------------------------------------------------
                if (m_sink != nullptr && m_sink->IsSinkReady())
                {
                    const HRESULT publishResult = m_sink->DeviceTransmit(acquiredTexture.Get());
                    if (FAILED(publishResult))
                    {
                        ++m_framesFailed;
                        // Не прерываем цикл: единичный сбой публикации лечится
                        // сам собой на следующем кадре (например, во время
                        // пересоздания текстур при смене режима).
                        PM_LOG_WARN(L"Публикация кадра не удалась, HRESULT=0x%08X", publishResult);
                    }
                }
                ++m_framesProcessed;
            }
            else
            {
                ++m_framesFailed;
                PM_LOG_WARN(L"Не удалось получить ID3D11Texture2D из буфера swapchain, HRESULT=0x%08X", hr);
            }

            // -----------------------------------------------------------------
            // Шаг 4. Сообщаем IddCx, что кадр обработан.
            //
            // Это обязательный парный вызов к ReleaseAndAcquireBuffer. Без него
            // композитор считает кадр «в работе» и не выдаст следующий.
            // -----------------------------------------------------------------
            const NTSTATUS finishStatus = IddCxSwapChainFinishedProcessingFrame(m_swapChain);
            if (!NT_SUCCESS(finishStatus))
            {
                PM_LOG_ERROR(L"FinishedProcessingFrame вернул статус 0x%08X", finishStatus);
                break;
            }

            // Периодическая сводка: помогает по журналу понять, идёт ли поток
            // кадров вообще, не включая подробную трассировку.
            if ((m_framesProcessed % kLogSummaryEvery) == 0 && m_framesProcessed != 0)
            {
                PM_LOG_INFO(L"Захват работает: обработано %llu кадров, сбоев %llu",
                    static_cast<unsigned long long>(m_framesProcessed),
                    static_cast<unsigned long long>(m_framesFailed));
            }

            // Проверка останова без блокировки: WaitForSingleObject с нулевым
            // таймаутом стоит десятки наносекунд и не влияет на латентность.
            if (WaitForSingleObject(m_terminateEvent, 0) == WAIT_OBJECT_0)
            {
                PM_LOG_INFO(L"Цикл захвата останавливается по запросу");
                break;
            }
        }
    }
}
