#pragma once

/*
 * =============================================================================
 *  Поток обработки swapchain виртуального монитора.
 * =============================================================================
 *
 *  Как это работает со стороны Windows:
 *  Композитор (DWM) рисует содержимое виртуального рабочего стола в swapchain,
 *  который IddCx назначает нашему монитору. Драйвер обязан непрерывно вызывать
 *  IddCxSwapChainReleaseAndAcquireBuffer: каждый вызов возвращает очередной
 *  готовый кадр и одновременно отпускает предыдущий. Если драйвер перестанет
 *  забирать кадры, композитор заблокируется, и вся система начнёт подтормаживать.
 *
 *  Наш путь кадра максимально короткий:
 *      AcquireBuffer -> CopyResource в общую текстуру -> Signal фенса ->
 *      публикация метаданных в разделяемую память -> FinishedProcessingFrame.
 *
 *  Внутри WUDFHost.exe НЕ выполняется ни кодирование, ни сетевой ввод-вывод:
 *  этот процесс охраняется PnP-менеджером, и долгая работа в нём приводит к
 *  таймаутам и принудительной перезагрузке устройства.
 *
 *  Приоритет потока поднимается через AvSetMmThreadCharacteristics("Distribution"),
 *  что переводит его в класс задач мультимедиа: планировщик Windows перестаёт
 *  вытеснять его фоновыми задачами, и дрожание (jitter) захвата падает в разы.
 */

#include "DriverCommon.h"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <memory>

namespace pm
{
    class Direct3DDevice;

    /*
     * Приёмник кадров. Реализуется контекстом устройства (IndirectDeviceContext).
     *
     * Введён специально, чтобы поток захвата ничего не знал ни про IddCx-объекты,
     * ни про разделяемую память: он умеет только «забрать кадр и отдать его
     * приёмнику». Это позволяет тестировать конвейер захвата в изоляции и
     * держать точку DeviceTransmit единственной на весь драйвер.
     */
    struct IFrameSink
    {
        /* Есть ли смысл вообще копировать кадр (подключён ли потребитель). */
        virtual bool IsSinkReady() const noexcept = 0;

        /*
         * Передача одного захваченного кадра дальше по конвейеру.
         * Реализация обязана быть быстрой и неблокирующей: она выполняется
         * в горячем пути захвата, и любая задержка здесь напрямую добавляется
         * к сквозной латентности системы.
         */
        virtual HRESULT DeviceTransmit(ID3D11Texture2D* frame) noexcept = 0;

        /* Уведомление о смене состояния конвейера (PM_STATE_*). */
        virtual void OnPipelineStateChanged(uint32_t state) noexcept = 0;

    protected:
        ~IFrameSink() = default;
    };

    class SwapChainProcessor
    {
    public:
        SwapChainProcessor(IDDCX_SWAPCHAIN swapChain,
                           std::shared_ptr<Direct3DDevice> device,
                           HANDLE newFrameEvent,
                           IFrameSink* sink) noexcept;

        ~SwapChainProcessor() noexcept;

        SwapChainProcessor(const SwapChainProcessor&) = delete;
        SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

        /* Запускает рабочий поток. */
        HRESULT Start() noexcept;

        /* Корректно останавливает поток и дожидается его завершения.
           Вызывается из EvtIddCxMonitorUnassignSwapChain, где IddCx гарантирует,
           что после возврата мы больше не тронем swapchain. */
        void Terminate() noexcept;

    private:
        static DWORD WINAPI ThreadProc(LPVOID param) noexcept;
        void Run() noexcept;

        /* Основной цикл забора кадров. Выделен отдельно, чтобы Run() отвечал
           только за настройку приоритета и уборку. */
        void ProcessFrames() noexcept;

    private:
        IDDCX_SWAPCHAIN m_swapChain = nullptr;
        std::shared_ptr<Direct3DDevice> m_device;
        HANDLE m_newFrameEvent = nullptr;   /* создано IddCx, мы не владеем    */
        IFrameSink* m_sink = nullptr;       /* не владеем                      */

        HANDLE m_thread = nullptr;
        HANDLE m_terminateEvent = nullptr;  /* ручной сброс                    */

        /* Счётчики для периодической сводки в журнал. */
        uint64_t m_framesProcessed = 0;
        uint64_t m_framesFailed = 0;
    };
}
