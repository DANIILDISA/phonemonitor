#ifndef PM_IPC_H
#define PM_IPC_H

/*
 * =============================================================================
 *  PhoneMonitor - контракт разделяемой памяти между IDD-драйвером и стримером.
 * =============================================================================
 *
 *  Этот заголовок подключается ОБЕИМИ сторонами:
 *    * драйвером (UMDF 2.0, процесс WUDFHost.exe, сессия 0);
 *    * приложением-стримером (обычный Win32-процесс).
 *
 *  Правила, которые нельзя нарушать:
 *    1. Здесь запрещены C++-типы, STL и выравнивание, зависящее от компилятора.
 *       Структура читается двумя разными бинарями, поэтому используем только
 *       фиксированные целочисленные типы и #pragma pack(8).
 *    2. Все поля, к которым обращаются обе стороны без захвата мьютекса,
 *       помечены volatile и читаются/пишутся атомарно (не более 8 байт).
 *    3. Порядок публикации кадра строго определён (см. комментарий к frameIndex):
 *       сначала пишутся ВСЕ описывающие поля, затем барьер, затем frameIndex.
 *       Читатель делает наоборот: сначала frameIndex, затем барьер, затем поля.
 *
 *  Модель владения объектами ядра:
 *    * Драйвер СОЗДАЁТ секцию, мьютекс и все события (он стартует первым).
 *    * Стример только ОТКРЫВАЕТ их; если открыть не удалось - драйвер ещё не
 *      загружен, стример входит в цикл ожидания.
 * =============================================================================
 */

#include <stdint.h>

#if defined(_MSC_VER)
#pragma pack(push, 8)
#endif

/* Сигнатура PMON в little-endian. Сторож против чужой секции с тем же именем. */
#define PM_IPC_MAGIC        0x4E4F4D50u

/* Версия бинарного контракта. Инкрементируется при ЛЮБОМ изменении структуры.
   Стример обязан отказаться работать при несовпадении версий. */
#define PM_IPC_VERSION      3u

/* Количество слотов кольцевого буфера общих текстур.
   Три слота - компромисс: драйвер пишет в слот N, стример читает N-1,
   слот N-2 остаётся остывать под возможной задержкой GPU-копии. */
#define PM_RING_SLOTS       3u

/* Максимальная длина имени именованной общей текстуры (в wchar_t, с нулём). */
#define PM_MAX_TEX_NAME     96u

/* Имена объектов ядра. Префикс Global обязателен: драйвер живёт в сессии 0,
   стример - в интерактивной сессии пользователя, локальное пространство имён
   у них разное. */
#define PM_NAME_SECTION     L"Global\\PhoneMonitor.SharedState.v3"
#define PM_NAME_MUTEX       L"Global\\PhoneMonitor.Mutex.v3"
#define PM_NAME_EVT_FRAME   L"Global\\PhoneMonitor.FrameReady.v3"
#define PM_NAME_EVT_MODEREQ L"Global\\PhoneMonitor.ModeRequest.v3"
#define PM_NAME_EVT_HNDREQ  L"Global\\PhoneMonitor.HandleRequest.v3"
#define PM_NAME_EVT_HNDRDY  L"Global\\PhoneMonitor.HandleReady.v3"

/* Шаблон имени общей текстуры: <серия пересоздания>.<номер слота>.
   Серия меняется при каждой смене режима - старые имена становятся мусором,
   и стример по несовпадению серии понимает, что нужно переоткрыть ресурсы. */
#define PM_FMT_TEXTURE      L"Global\\PhoneMonitor.Tex.%u.%u"

/* SDDL: полный доступ SYSTEM и администраторам, чтение/запись - прошедшим
   проверку пользователям. Драйвер применяет её ко всем создаваемым объектам,
   иначе стример из пользовательской сессии не сможет их открыть. */
#define PM_SDDL_SHARED      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGWGX;;;AU)"

/* --- Состояния конвейера (поле pipelineState) ------------------------------ */
#define PM_STATE_IDLE       0u  /* драйвер загружен, монитор не подключён      */
#define PM_STATE_ARRIVED    1u  /* монитор объявлен системе, swapchain нет     */
#define PM_STATE_STREAMING  2u  /* swapchain назначен, кадры публикуются       */
#define PM_STATE_SHUTDOWN   3u  /* драйвер выгружается, ресурсы недействительны*/

/* --- Флаги качества, выставляемые стримером (поле qualityFlags) ------------ */
#define PM_QF_THERMAL_SAVE  0x00000001u /* троттлинг: просят понизить нагрузку */

/*
 * Описание одного слота кольца общих текстур.
 * Драйвер заполняет структуру ДО того, как первый раз опубликует кадр в слот.
 */
typedef struct PmSlotInfo
{
    volatile uint32_t width;          /* ширина текстуры в пикселях              */
    volatile uint32_t height;         /* высота текстуры в пикселях              */
    volatile uint32_t dxgiFormat;     /* DXGI_FORMAT, обычно B8G8R8A8_UNORM      */
    volatile uint32_t valid;          /* 1 - текстура создана и расшарена        */
    wchar_t           name[PM_MAX_TEX_NAME]; /* имя для OpenSharedResourceByName */
} PmSlotInfo;

/*
 * Главный заголовок разделяемой секции.
 *
 * Схема взаимодействия по кадрам (lock-free, один писатель - один читатель):
 *
 *   Драйвер:                              Стример:
 *   ---------                             ---------
 *   1. CopyResource -> slots[i]           1. idx = frameIndex   (acquire)
 *   2. ctx4->Signal(fence, v)             2. читает slot/fenceValue/presentQpc
 *   3. slot = i; fenceValue = v;          3. ctx4->Wait(fence, fenceValue)
 *      presentQpc = QPC()                 4. копирует текстуру slots[slot]
 *   4. MemoryBarrier()                    5. если frameIndex изменился за время
 *   5. frameIndex++      (release)           чтения - кадр устарел, повтор.
 *   6. SetEvent(FrameReady)
 */
typedef struct PmSharedHeader
{
    /* --- Неизменяемая шапка (пишется один раз при создании) --------------- */
    uint32_t magic;                     /* PM_IPC_MAGIC                       */
    uint32_t version;                   /* PM_IPC_VERSION                     */
    uint32_t headerSize;                /* sizeof(PmSharedHeader)             */
    uint32_t ringSlots;                 /* PM_RING_SLOTS                      */

    /* --- Идентификация процессов ------------------------------------------ */
    volatile uint32_t driverPid;        /* PID WUDFHost.exe, где живёт драйвер*/
    volatile uint32_t streamerPid;      /* PID стримера; 0 - стример отключён */
    volatile uint32_t driverAlive;      /* 1 пока драйвер держит секцию       */
    volatile uint32_t pipelineState;    /* PM_STATE_*                         */

    /* --- Запрос режима: стример -> драйвер --------------------------------- */
    /* Стример пишет желаемое разрешение (пришедшее от Android), инкрементирует
       modeRequestSerial и дёргает PM_NAME_EVT_MODEREQ. Драйвер применяет режим
       и копирует серию в modeAppliedSerial. */
    volatile uint32_t requestedWidth;
    volatile uint32_t requestedHeight;
    volatile uint32_t requestedRefreshHz;
    volatile uint32_t modeRequestSerial;
    volatile uint32_t modeAppliedSerial;
    volatile uint32_t modeApplyStatus;   /* 0 - ок, иначе HRESULT                */

    /* --- Текущий активный режим: драйвер -> стример ------------------------ */
    volatile uint32_t activeWidth;
    volatile uint32_t activeHeight;
    volatile uint32_t activeRefreshHz;
    volatile uint32_t textureSerial;     /* серия имён текстур; 0 - нет ресурсов */

    /* --- Публикация кадров -------------------------------------------------- */
    volatile uint64_t frameIndex;        /* монотонный счётчик; пишется ПОСЛЕДНИМ */
    volatile uint64_t fenceValue;        /* значение фенса для этого кадра        */
    volatile uint64_t presentQpc;        /* QPC на момент публикации (латентность)*/
    volatile uint32_t slot;              /* индекс слота с последним кадром       */
    volatile uint32_t droppedFrames;     /* кадры, которые стример не успел взять */

    /* --- Передача HANDLE фенса через дублирование --------------------------- */
    /* Фенс нельзя открыть по имени (OpenSharedFence принимает только HANDLE),
       поэтому используется схема запрос-ответ:
         стример: streamerPid = GetCurrentProcessId(); SetEvent(HandleRequest)
         драйвер: DuplicateHandle(... в процесс стримера ...);
                  fenceHandleValue = дубликат; fenceHandleSerial++;
                  SetEvent(HandleReady)
       Значение fenceHandleValue валидно ТОЛЬКО в адресном пространстве
       процесса с pid == fenceHandleOwnerPid и только для серии fenceHandleSerial. */
    volatile uint64_t fenceHandleValue;
    volatile uint32_t fenceHandleSerial;
    volatile uint32_t fenceHandleOwnerPid;

    /* --- Управление качеством: стример -> драйвер --------------------------- */
    volatile uint32_t qualityFlags;      /* PM_QF_*                            */
    volatile uint32_t reserved0;

    /* --- Телеметрия --------------------------------------------------------- */
    volatile uint64_t driverFramesProduced;
    volatile uint64_t streamerFramesConsumed;

    /* --- Кольцо слотов ------------------------------------------------------ */
    PmSlotInfo slots[PM_RING_SLOTS];
} PmSharedHeader;

/* Размер отображаемой секции. Округляем вверх до 64 КиБ - гранулярность
   резервирования адресного пространства в Windows, экономить тут нечего. */
#define PM_SECTION_SIZE     (64u * 1024u)

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#endif /* PM_IPC_H */
