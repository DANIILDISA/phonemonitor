#include "Direct3DDevice.h"
#include "Trace.h"

using Microsoft::WRL::ComPtr;

namespace pm
{
    Direct3DDevice::Direct3DDevice(LUID adapterLuid) noexcept
        : m_adapterLuid(adapterLuid)
    {
    }

    Direct3DDevice::Direct3DDevice() noexcept
    {
        // Нулевой LUID трактуется в Init() как «любой адаптер по умолчанию».
        m_adapterLuid.LowPart = 0;
        m_adapterLuid.HighPart = 0;
    }

    HRESULT Direct3DDevice::Init() noexcept
    {
        // ---------------------------------------------------------------------
        // Шаг 1. Фабрика DXGI.
        // Берём именно CreateDXGIFactory2 (а не CreateDXGIFactory), потому что
        // только она умеет отдать IDXGIFactory5, где есть EnumAdapterByLuid.
        // ---------------------------------------------------------------------
        UINT factoryFlags = 0;
#if defined(_DEBUG) || defined(DBG)
        // Слой отладки DXGI ловит утечки интерфейсов; в релизе он запрещён,
        // так как заметно увеличивает стоимость каждого вызова.
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

        HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
        if (FAILED(hr))
        {
            // Повторная попытка без отладочного флага: на системах без
            // установленного «Graphics Tools» слой отсутствует и вызов падает.
            PM_LOG_WARN(L"CreateDXGIFactory2 с флагом отладки не удался (0x%08X), пробуем без него", hr);
            hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
        }
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Не удалось создать фабрику DXGI, HRESULT=0x%08X", hr);
            return hr;
        }

        // ---------------------------------------------------------------------
        // Шаг 2. Поиск адаптера.
        // ---------------------------------------------------------------------
        if (m_adapterLuid.LowPart != 0 || m_adapterLuid.HighPart != 0)
        {
            ComPtr<IDXGIAdapter1> adapter;
            hr = m_factory->EnumAdapterByLuid(m_adapterLuid, IID_PPV_ARGS(&adapter));
            if (FAILED(hr))
            {
                PM_LOG_ERROR(L"Адаптер с LUID %08X:%08X не найден, HRESULT=0x%08X",
                    m_adapterLuid.HighPart, m_adapterLuid.LowPart, hr);
                return hr;
            }
            m_adapter = adapter;
        }
        else
        {
            // Фолбэк: первый перечисленный адаптер.
            hr = m_factory->EnumAdapters1(0, &m_adapter);
            if (FAILED(hr))
            {
                PM_LOG_ERROR(L"В системе не перечислен ни один адаптер DXGI, HRESULT=0x%08X", hr);
                return hr;
            }
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (SUCCEEDED(m_adapter->GetDesc1(&adapterDesc)))
        {
            // Запоминаем реальный LUID: если пришли по пути «адаптер по умолчанию»,
            // поле m_adapterLuid было нулевым и его нужно актуализировать.
            m_adapterLuid = adapterDesc.AdapterLuid;
            PM_LOG_INFO(L"Выбран графический адаптер: %s (LUID %08X:%08X, VRAM %llu МиБ)",
                adapterDesc.Description,
                adapterDesc.AdapterLuid.HighPart,
                adapterDesc.AdapterLuid.LowPart,
                static_cast<unsigned long long>(adapterDesc.DedicatedVideoMemory / (1024ull * 1024ull)));
        }

        // ---------------------------------------------------------------------
        // Шаг 3. Создание устройства D3D11.
        //
        // BGRA_SUPPORT обязателен: рабочий стол композитится в формате
        // DXGI_FORMAT_B8G8R8A8_UNORM, и без этого флага мы не сможем создать
        // совместимые ресурсы.
        //
        // Список уровней функциональности идёт по убыванию: берём максимум из
        // доступного, но не опускаемся ниже 11_0 - ID3D11Fence требует 11_0+
        // и драйвера с поддержкой D3D11.3.
        // ---------------------------------------------------------------------
        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG) || defined(DBG)
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        static const D3D_FEATURE_LEVEL kFeatureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        D3D_FEATURE_LEVEL achievedLevel{};
        hr = D3D11CreateDevice(
            m_adapter.Get(),
            // Тип драйвера ОБЯЗАН быть UNKNOWN, когда передан конкретный адаптер:
            // указание HARDWARE вместе с непустым pAdapter возвращает E_INVALIDARG.
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            deviceFlags,
            kFeatureLevels,
            ARRAYSIZE(kFeatureLevels),
            D3D11_SDK_VERSION,
            &m_device,
            &achievedLevel,
            &m_context);

        if (FAILED(hr) && (deviceFlags & D3D11_CREATE_DEVICE_DEBUG))
        {
            PM_LOG_WARN(L"Создание устройства с отладочным слоем не удалось (0x%08X), повтор без него", hr);
            deviceFlags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
            hr = D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                deviceFlags, kFeatureLevels, ARRAYSIZE(kFeatureLevels),
                D3D11_SDK_VERSION, &m_device, &achievedLevel, &m_context);
        }

        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"D3D11CreateDevice завершился ошибкой 0x%08X", hr);
            return hr;
        }

        PM_LOG_INFO(L"Устройство D3D11 создано, уровень функциональности 0x%04X", achievedLevel);

        // ---------------------------------------------------------------------
        // Шаг 4. Запрос расширенных интерфейсов.
        // ID3D11Device5  - CreateFence;
        // ID3D11DeviceContext4 - Signal/Wait по фенсу.
        // Если их нет, драйвер физически не сможет синхронизироваться со
        // стримером, поэтому это фатальная ошибка, а не предупреждение.
        // ---------------------------------------------------------------------
        hr = m_device.As(&m_device5);
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Драйвер GPU не предоставляет ID3D11Device5 (нет поддержки фенсов), HRESULT=0x%08X", hr);
            return hr;
        }

        hr = m_context.As(&m_context4);
        if (FAILED(hr))
        {
            PM_LOG_ERROR(L"Драйвер GPU не предоставляет ID3D11DeviceContext4, HRESULT=0x%08X", hr);
            return hr;
        }

        // ---------------------------------------------------------------------
        // Шаг 5. Многопоточная защита устройства.
        //
        // Контекст трогают два потока: поток обработки swapchain (копирование
        // кадра) и поток IPC (создание/пересоздание общих текстур при смене
        // режима). Без SetMultithreadProtected это классическая гонка,
        // которая проявляется редкими зависаниями GPU.
        // ---------------------------------------------------------------------
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(m_context.As(&multithread)))
        {
            multithread->SetMultithreadProtected(TRUE);
            PM_LOG_TRACE(L"Включена потокобезопасность контекста D3D11");
        }
        else
        {
            PM_LOG_WARN(L"ID3D11Multithread недоступен: доступ к контексту придётся сериализовать вручную");
        }

        return S_OK;
    }
}
