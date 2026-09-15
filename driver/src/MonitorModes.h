#pragma once

/*
 * =============================================================================
 *  Генерация описания виртуального монитора: EDID и списки режимов.
 * =============================================================================
 *
 *  Зачем нужен настоящий EDID, а не заглушка:
 *  IddCx передаёт блок описания монитора менеджеру дисплеев, а тот строит по
 *  нему имя устройства, физический размер и предпочтительный режим. Мусорный
 *  EDID приводит к тому, что Windows либо отказывается создавать монитор, либо
 *  показывает «Универсальный монитор PnP» с неверным DPI и масштабированием.
 *
 *  Так как разрешение у нас динамическое (его диктует телефон), EDID
 *  СОБИРАЕТСЯ НА ЛЕТУ: подробный дескриптор синхронизации (DTD) описывает ровно
 *  тот режим, который запросил клиент, и помечается как предпочтительный.
 */

#include "DriverCommon.h"
#include <vector>
#include <cstdint>

namespace pm
{
    /* Описание одного поддерживаемого режима. */
    struct MonitorMode
    {
        uint32_t width;
        uint32_t height;
        uint32_t refreshHz;
    };

    /*
     * Собирает валидный 128-байтовый блок EDID 1.4 для заданного режима.
     * Контрольная сумма считается в конце, поэтому массив всегда согласован.
     */
    std::vector<uint8_t> BuildEdid(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept;

    /*
     * Формирует список режимов, который драйвер объявит системе.
     * Первым всегда идёт запрошенный режим (он же предпочтительный), далее -
     * набор безопасных запасных вариантов, чтобы Windows было куда откатиться,
     * если клиент отвалится во время смены разрешения.
     */
    std::vector<MonitorMode> BuildModeTable(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept;

    /* Заполнение DISPLAYCONFIG_VIDEO_SIGNAL_INFO по режиму.
       isMonitorMode различает режим монитора и режим цели (target):
       у цели vSyncFreqDivider обязан быть равен 1, у монитора - 0. */
    void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& info,
                        uint32_t width, uint32_t height, uint32_t refreshHz,
                        bool isMonitorMode) noexcept;

    /* Готовые конструкторы структур IddCx. */
    IDDCX_MONITOR_MODE CreateMonitorMode(uint32_t width, uint32_t height, uint32_t refreshHz,
                                         IDDCX_MONITOR_MODE_ORIGIN origin) noexcept;

    IDDCX_TARGET_MODE CreateTargetMode(uint32_t width, uint32_t height, uint32_t refreshHz) noexcept;

    /* Приводит размеры к требованиям видеокодировщиков.
       H.264 работает макроблоками 16x16; кодировщик примет и кратное 2, но
       кратность 16 избавляет от краевых артефактов и лишнего кропа. */
    void AlignForEncoder(uint32_t& width, uint32_t& height) noexcept;
}
