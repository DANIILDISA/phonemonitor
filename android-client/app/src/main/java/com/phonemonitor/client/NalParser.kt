package com.phonemonitor.client

import android.util.Log

/**
 * =============================================================================
 *  Разборщик потока H.264 Annex B на отдельные NAL-юниты.
 * =============================================================================
 *
 *  Задача выглядит простой, но имеет три неочевидных требования:
 *
 *  1. TCP не сохраняет границы записей. Один вызов read может вернуть половину
 *     NAL-юнита, полтора юнита или сорок юнитов сразу. Разборщик обязан
 *     работать с произвольной нарезкой.
 *
 *  2. Стартовый код бывает трёхбайтовым (00 00 01) и четырёхбайтовым
 *     (00 00 00 01). Оба встречаются в одном потоке: кодировщики обычно
 *     ставят четырёхбайтовый перед SPS/PPS/IDR и трёхбайтовый перед остальными.
 *
 *  3. Стартовый код может быть разорван границей чтения: три нуля пришли в
 *     одном пакете, единица - в следующем. Поэтому нельзя просто искать
 *     сигнатуру внутри свежего куска - нужен скользящий поиск по накопленному
 *     буферу с сохранением хвоста.
 *
 *  Алгоритм - скользящее окно поверх растущего кольцевого массива:
 *
 *      [ ...обработано... | НАЧАЛО текущего NAL ... хвост... ]
 *                          ^searchStart          ^dataEnd
 *
 *  Позиция searchStart указывает на начало полезных данных текущего NAL
 *  (сразу после его стартового кода). Поиск следующего стартового кода
 *  начинается с searchPosition и продолжается по мере поступления данных,
 *  никогда не перепроверяя уже просмотренные байты - это даёт линейную
 *  сложность O(n) от объёма потока.
 *
 *  Восстановление после повреждения:
 *  если NAL оказался бессмысленным (нулевая длина, запрещённый бит forbidden_zero,
 *  превышение лимита размера), он отбрасывается, а разборщик просто продолжает
 *  искать следующий стартовый код. Никакого сложного восстановления не нужно -
 *  стартовые коды и есть точки ресинхронизации, заложенные в сам формат.
 */
class NalParser(
    /** Максимально допустимый размер одного NAL-юнита. */
    private val maxNalSize: Int = 8 * 1024 * 1024,

    /** Колбэк для готового NAL. Буфер переиспользуется - копировать при необходимости. */
    private val onNalUnit: (data: ByteArray, offset: Int, length: Int, nalType: Int) -> Unit
) {

    companion object {
        private const val TAG = "NalParser"

        /** Начальная ёмкость буфера: с запасом на один кадр 1080p. */
        private const val INITIAL_CAPACITY = 512 * 1024

        // Типы NAL-юнитов H.264, которые нас интересуют.
        const val NAL_TYPE_SLICE_NON_IDR = 1
        const val NAL_TYPE_SLICE_IDR = 5
        const val NAL_TYPE_SEI = 6
        const val NAL_TYPE_SPS = 7
        const val NAL_TYPE_PPS = 8
        const val NAL_TYPE_AUD = 9
    }

    /** Накопительный буфер. Растёт при необходимости, но никогда не уменьшается. */
    private var buffer = ByteArray(INITIAL_CAPACITY)

    /** Количество валидных байт в буфере. */
    private var dataEnd = 0

    /** Смещение начала данных текущего NAL (после стартового кода). */
    private var nalStart = -1

    /** Длина стартового кода текущего NAL: 3 или 4. */
    private var currentStartCodeLength = 0

    /**
     * Позиция, с которой продолжается поиск стартового кода.
     * Гарантирует, что каждый байт просматривается ровно один раз.
     */
    private var searchPosition = 0

    // --- Диагностика --------------------------------------------------------
    var totalNalUnits: Long = 0
        private set
    var droppedNalUnits: Long = 0
        private set
    var totalBytes: Long = 0
        private set

    /**
     * Скармливает разборщику очередной кусок данных из сокета.
     *
     * @param chunk  массив с данными
     * @param offset смещение начала данных
     * @param length количество байт
     */
    fun append(chunk: ByteArray, offset: Int, length: Int) {
        if (length <= 0) return

        totalBytes += length
        ensureCapacity(length)

        System.arraycopy(chunk, offset, buffer, dataEnd, length)
        dataEnd += length

        parseAvailable()
    }

    /**
     * Сбрасывает состояние. Вызывается при переподключении: остатки старого
     * потока не должны склеиться с началом нового.
     */
    fun reset() {
        dataEnd = 0
        nalStart = -1
        currentStartCodeLength = 0
        searchPosition = 0
        Log.d(TAG, "Разборщик NAL сброшен")
    }

    /**
     * Обеспечивает место под additionalBytes новых байт.
     *
     * Стратегия из двух шагов:
     *   1) если в начале буфера накопились обработанные данные - сдвигаем
     *      полезный хвост влево (это дешевле, чем выделять память);
     *   2) только если и после сдвига места не хватает - удваиваем ёмкость.
     */
    private fun ensureCapacity(additionalBytes: Int) {
        if (dataEnd + additionalBytes <= buffer.size) {
            return
        }

        // Шаг 1: уплотнение. Всё, что левее начала текущего NAL (с учётом его
        // стартового кода), уже отдано наружу и больше не нужно.
        val discardable = if (nalStart >= 0) {
            (nalStart - currentStartCodeLength).coerceAtLeast(0)
        } else {
            // Текущего NAL нет - можно выбросить всё, кроме трёх последних
            // байт: в них может лежать начало разорванного стартового кода.
            (dataEnd - 3).coerceAtLeast(0)
        }

        if (discardable > 0) {
            System.arraycopy(buffer, discardable, buffer, 0, dataEnd - discardable)
            dataEnd -= discardable
            searchPosition = (searchPosition - discardable).coerceAtLeast(0)
            if (nalStart >= 0) {
                nalStart -= discardable
            }
        }

        if (dataEnd + additionalBytes <= buffer.size) {
            return
        }

        // Шаг 2: рост буфера.
        var newCapacity = buffer.size
        while (newCapacity < dataEnd + additionalBytes) {
            newCapacity *= 2
            if (newCapacity > maxNalSize * 2) {
                // Поток явно сбит: столько данных без единого стартового кода
                // быть не может. Сбрасываемся и ждём следующего стартового кода.
                Log.w(TAG, "Буфер вырос до $newCapacity байт без стартового кода - сброс")
                droppedNalUnits++
                reset()
                newCapacity = INITIAL_CAPACITY
                break
            }
        }

        if (newCapacity != buffer.size) {
            buffer = buffer.copyOf(newCapacity)
            Log.d(TAG, "Буфер разборщика увеличен до ${newCapacity / 1024} КиБ")
        }
    }

    /**
     * Основной цикл разбора: ищет стартовые коды и выдаёт завершённые NAL.
     *
     * Ключевая деталь: NAL считается завершённым только когда найден СЛЕДУЮЩИЙ
     * стартовый код. Последний NAL в буфере всегда остаётся незавершённым и
     * ждёт продолжения - иначе мы отдали бы декодеру обрезанный кадр.
     */
    private fun parseAvailable() {
        var position = searchPosition

        // Для поиска нужно минимум 3 байта впереди.
        while (position + 2 < dataEnd) {
            // Быстрый отсев: подавляющее большинство байт видеопотока не нули,
            // поэтому проверка первого байта отсекает почти все итерации
            // до обращения к остальным.
            if (buffer[position] != 0.toByte()) {
                position++
                continue
            }
            if (buffer[position + 1] != 0.toByte()) {
                // Следующий байт не ноль - значит стартового кода не может быть
                // ни здесь, ни начиная со следующей позиции. Прыгаем через два.
                position += 2
                continue
            }

            val startCodeLength: Int = when {
                buffer[position + 2] == 1.toByte() -> 3
                position + 3 < dataEnd &&
                    buffer[position + 2] == 0.toByte() &&
                    buffer[position + 3] == 1.toByte() -> 4
                else -> {
                    // Нули есть, единицы нет. Возможно, стартовый код разорван
                    // границей чтения - останавливаемся и ждём данных.
                    if (position + 3 >= dataEnd) break
                    position++
                    continue
                }
            }

            if (nalStart >= 0) {
                // Найден конец предыдущего NAL.
                val nalLength = position - nalStart
                emitNal(nalStart, nalLength)
            }

            nalStart = position + startCodeLength
            currentStartCodeLength = startCodeLength
            position = nalStart
        }

        // Запоминаем, докуда просмотрели. Отступаем на 3 байта назад: там может
        // начинаться стартовый код, чей хвост придёт со следующим куском.
        searchPosition = (position - 3).coerceAtLeast(nalStart.coerceAtLeast(0))
        if (searchPosition < 0) searchPosition = 0
    }

    /**
     * Проверяет и отдаёт наружу один NAL-юнит.
     *
     * Проверки на валидность дешёвые, но отсекают весь мусор, который может
     * появиться при рассинхронизации: пустые юниты, юниты с установленным
     * запрещённым битом, юниты неправдоподобного размера.
     */
    private fun emitNal(start: Int, length: Int) {
        if (length <= 0) {
            droppedNalUnits++
            return
        }

        if (length > maxNalSize) {
            Log.w(TAG, "NAL размером $length байт превышает лимит - отброшен")
            droppedNalUnits++
            return
        }

        val headerByte = buffer[start].toInt() and 0xFF

        // Бит forbidden_zero_bit обязан быть нулевым по стандарту H.264.
        // Единица означает, что мы читаем не заголовок NAL, а мусор.
        if ((headerByte and 0x80) != 0) {
            Log.w(TAG, "NAL с установленным forbidden_zero_bit - отброшен")
            droppedNalUnits++
            return
        }

        val nalType = headerByte and 0x1F

        // Тип 0 зарезервирован и не должен встречаться, типы 24-31 относятся
        // к RTP-агрегации и в Annex B потоке невозможны.
        if (nalType == 0 || nalType >= 24) {
            Log.w(TAG, "NAL недопустимого типа $nalType - отброшен")
            droppedNalUnits++
            return
        }

        totalNalUnits++

        // Отдаём вместе со стартовым кодом: MediaCodec ожидает Annex B,
        // то есть данные, начинающиеся со стартового кода.
        val startWithCode = start - currentStartCodeLength
        onNalUnit(buffer, startWithCode, length + currentStartCodeLength, nalType)
    }

    /** Человекочитаемое имя типа NAL - только для журнала. */
    fun nalTypeName(nalType: Int): String = when (nalType) {
        NAL_TYPE_SLICE_NON_IDR -> "слайс (не IDR)"
        NAL_TYPE_SLICE_IDR -> "слайс IDR"
        NAL_TYPE_SEI -> "SEI"
        NAL_TYPE_SPS -> "SPS"
        NAL_TYPE_PPS -> "PPS"
        NAL_TYPE_AUD -> "разделитель доступа"
        else -> "тип $nalType"
    }
}
