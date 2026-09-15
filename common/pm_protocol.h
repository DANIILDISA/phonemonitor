#ifndef PM_PROTOCOL_H
#define PM_PROTOCOL_H

/*
 * =============================================================================
 *  PhoneMonitor - сетевой протокол между Windows-стримером и Android-клиентом.
 * =============================================================================
 *
 *  Транспорт: единственное TCP-соединение поверх adb-форварда.
 *      adb.exe forward tcp:6100 tcp:6100
 *  Android держит ServerSocket на 6100 (внутри устройства), Windows выступает
 *  КЛИЕНТОМ и подключается к 127.0.0.1:6100 на хосте: adb сам протаскивает
 *  соединение через USB.
 *
 *  Соединение асимметрично по формату:
 *
 *    Android -> Windows : поток управляющих кадров (структура PmCtrlHeader).
 *                         Первый кадр ОБЯЗАН быть PM_MSG_HELLO, иначе стример
 *                         разрывает соединение.
 *
 *    Windows -> Android : СЫРОЙ Annex B поток H.264 без какой-либо обёртки.
 *                         Это осознанное решение: декодер MediaCodec умеет
 *                         есть Annex B напрямую, а отсутствие заголовков
 *                         экономит и такты, и задержку. Синхронизация - по
 *                         стартовым кодам 00 00 00 01 / 00 00 01.
 *
 *  Порядок байт для управляющего канала: BIG-ENDIAN (сетевой).
 *  Причина: на стороне Android используется java.io.DataInputStream/ByteBuffer,
 *  у которых big-endian - поведение по умолчанию; так меньше шансов ошибиться.
 * =============================================================================
 */

#include <stdint.h>

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif

/* Сигнатура управляющего кадра: байты 'P','M','C','1'. */
#define PM_CTRL_MAGIC       0x504D4331u

/* Версия управляющего протокола. */
#define PM_PROTO_VERSION    1u

/* Порт по обе стороны adb-форварда. */
#define PM_TCP_PORT         6100

/* Максимальный размер полезной нагрузки управляющего кадра.
   Защита от мусора в потоке: всё, что больше, считается рассинхронизацией. */
#define PM_CTRL_MAX_PAYLOAD 4096u

/* --- Типы сообщений Android -> Windows -------------------------------------- */
#define PM_MSG_HELLO        1u  /* PmMsgHello   - геометрия экрана устройства   */
#define PM_MSG_THERMAL      2u  /* PmMsgThermal - тепловое/батарейное состояние */
#define PM_MSG_STATS        3u  /* PmMsgStats   - телеметрия декодера           */
#define PM_MSG_KEYFRAME_REQ 4u  /* без нагрузки - просьба выдать IDR немедленно */
#define PM_MSG_BYE          5u  /* без нагрузки - корректное закрытие сессии    */

/* --- Тепловые состояния (зеркало android.os.PowerManager.THERMAL_STATUS_*) --- */
#define PM_THERMAL_NONE     0
#define PM_THERMAL_LIGHT    1
#define PM_THERMAL_MODERATE 2
#define PM_THERMAL_SEVERE   3
#define PM_THERMAL_CRITICAL 4
#define PM_THERMAL_EMERGENCY 5
#define PM_THERMAL_SHUTDOWN 6

/*
 * Заголовок управляющего кадра. Фиксированные 12 байт, big-endian.
 * Читается ровно за один recv-до-заполнения, затем читается payloadLen байт.
 */
typedef struct PmCtrlHeader
{
    uint32_t magic;       /* PM_CTRL_MAGIC                                   */
    uint16_t version;     /* PM_PROTO_VERSION                                */
    uint16_t type;        /* PM_MSG_*                                        */
    uint32_t payloadLen;  /* длина тела в байтах, 0..PM_CTRL_MAX_PAYLOAD     */
} PmCtrlHeader;

/*
 * PM_MSG_HELLO - самый первый пакет в соединении.
 * Android сообщает физические размеры экрана, полученные из WindowManager.
 * Стример обязан:
 *   1) выровнять ширину/высоту по требованиям кодировщика (кратность 2/16);
 *   2) прописать их в разделяемую память и дёрнуть драйвер на смену режима;
 *   3) только после подтверждения режима начать кодирование.
 */
typedef struct PmMsgHello
{
    uint32_t width;        /* физическая ширина в пикселях                    */
    uint32_t height;       /* физическая высота в пикселях                    */
    uint32_t refreshMilliHz; /* частота обновления * 1000 (60000 = 60.000 Гц) */
    uint32_t densityDpi;   /* плотность экрана, для расчёта масштаба в Windows */
    uint32_t maxDecoderW;  /* максимум, который тянет аппаратный декодер       */
    uint32_t maxDecoderH;
    uint32_t flags;        /* зарезервировано, должно быть 0                  */
} PmMsgHello;

/*
 * PM_MSG_THERMAL - Android периодически (и при каждом изменении) рапортует
 * о нагреве. Стример по этим данным опускает кодирование в 720p.
 */
typedef struct PmMsgThermal
{
    int32_t  thermalStatus; /* PM_THERMAL_*                                   */
    int32_t  batteryPercent;/* 0..100, -1 если неизвестно                     */
    int32_t  isCharging;    /* 0/1                                            */
    int32_t  reserved;
} PmMsgThermal;

/*
 * PM_MSG_STATS - телеметрия для диагностики задержки.
 * decodeLatencyUs измеряется от момента постановки NAL в декодер до отдачи
 * кадра на Surface.
 */
typedef struct PmMsgStats
{
    uint64_t framesDecoded;
    uint64_t framesDropped;
    uint32_t decodeLatencyUs;
    uint32_t queueDepth;
} PmMsgStats;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#endif /* PM_PROTOCOL_H */
