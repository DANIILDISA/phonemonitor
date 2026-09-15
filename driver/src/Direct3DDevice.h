#pragma once

/*
 * =============================================================================
 *  Обёртка над D3D11-устройством, привязанным к конкретному адаптеру.
 * =============================================================================
 *
 *  IddCx сообщает драйверу LUID адаптера, на котором композитор рисует наш
 *  виртуальный монитор. Мы ОБЯЗАНЫ создать устройство ровно на этом адаптере:
 *  иначе поверхности swapchain окажутся на чужом GPU, и любое обращение к ним
 *  приведёт либо к отказу, либо к скрытому копированию через системную память
 *  (это мгновенно съедает весь бюджет задержки).
 *
 *  Требуется уровень D3D11_1 и интерфейсы ...Device5 / ...DeviceContext4:
 *  без них нет ID3D11Fence, а фенс - основа синхронизации со стримером.
 */

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

namespace pm
{
    class Direct3DDevice
    {
    public:
        /* Создание устройства на адаптере с заданным LUID. */
        explicit Direct3DDevice(LUID adapterLuid) noexcept;

        /* Конструктор без LUID - берётся адаптер по умолчанию.
           Используется только в отладочных утилитах, в боевом пути не нужен. */
        Direct3DDevice() noexcept;

        ~Direct3DDevice() = default;

        Direct3DDevice(const Direct3DDevice&) = delete;
        Direct3DDevice& operator=(const Direct3DDevice&) = delete;

        /* Полная инициализация: фабрика -> адаптер -> устройство -> контекст.
           Возвращает S_OK либо HRESULT первой упавшей стадии. */
        HRESULT Init() noexcept;

        /* Признак того, что Init() отработал успешно. */
        bool IsValid() const noexcept { return m_device != nullptr; }

        LUID AdapterLuid() const noexcept { return m_adapterLuid; }

        ID3D11Device*        Device() const noexcept  { return m_device.Get(); }
        ID3D11Device5*       Device5() const noexcept { return m_device5.Get(); }
        ID3D11DeviceContext* Context() const noexcept { return m_context.Get(); }
        ID3D11DeviceContext4* Context4() const noexcept { return m_context4.Get(); }
        IDXGIFactory5*       Factory() const noexcept { return m_factory.Get(); }
        IDXGIAdapter1*       Adapter() const noexcept { return m_adapter.Get(); }

    private:
        LUID m_adapterLuid{};

        Microsoft::WRL::ComPtr<IDXGIFactory5>        m_factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1>        m_adapter;
        Microsoft::WRL::ComPtr<ID3D11Device>         m_device;
        Microsoft::WRL::ComPtr<ID3D11Device5>        m_device5;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext>  m_context;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    };
}
