package com.phonemonitor.client

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Process
import android.util.Log
import android.view.Surface
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * =============================================================================
 *  Аппаратный декодер H.264 с выводом напрямую на Surface.
 * =============================================================================
 *
 *  Потоковая модель:
 *
 *      [Сетевой поток] --submit()--> [очередь пакетов] --> [Поток декодера]
 *                                                              |
 *                                                    MediaCodec --> Surface
 *
 *  Почему очередь, а не прямая подача из сетевого потока:
 *  dequeueInputBuffer может заблокироваться, если декодер занят. Блокировать
 *  на этом сетевой поток нельзя - он перестанет вычитывать сокет, TCP-окно
 *  схлопнется, и на стороне ПК начнёт копиться задержка отправки.
 *
 *  Почему очередь МАЛЕНЬКАЯ (см. QUEUE_CAPACITY):
 *  большая очередь превращается в скрытый буфер задержки. Если декодер не
 *  успевает, честнее выбросить кадры и запросить новый опорный, чем показывать
 *  зрителю то, что было полсекунды назад. Глубина очереди - это прямое
 *  слагаемое сквозной задержки.
 *
 *  Про пул буферов:
 *  массивы под пакеты переиспользуются. При 60 кадрах в секунду выделение
 *  нового массива на каждый NAL заставило бы сборщик мусора работать
 *  непрерывно, а каждая его пауза - это пропущенный кадр.
 */
class VideoDecoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int,
    private val listener: Listener
) {

    interface Listener {
        /** Первый успешно показанный кадр - можно убирать накладку состояния. */
        fun onFirstFrameRendered()

        /** Декодер запрашивает ключевой кадр (после сброса или потери данных). */
        fun onKeyFrameNeeded()

        /** Неустранимая ошибка декодера. */
        fun onDecoderError(message: String)

        /** Периодическая телеметрия. */
        fun onStats(framesDecoded: Long, framesDropped: Long, latencyUs: Int, queueDepth: Int)
    }

    companion object {
        private const val TAG = "VideoDecoder"

        private const val MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AVC

        /**
         * Глубина очереди пакетов.
         *
         * Восемь NAL-юнитов - это примерно два-три кадра вместе со служебными
         * юнитами. Больше держать вредно: очередь превращается в задержку.
         */
        private const val QUEUE_CAPACITY = 8

        /**
         * Таймаут ожидания входного буфера, мкс.
         *
         * 10 мс по требованию спецификации. Ноль был бы агрессивнее, но привёл
         * бы к активному опросу и лишней нагрузке на процессор, а она
         * оборачивается нагревом и троттлингом.
         */
        private const val INPUT_TIMEOUT_US = 10_000L

        /**
         * Таймаут ожидания выходного буфера, мкс.
         *
         * Ноль: выходные кадры забираем неблокирующе. Если кадр не готов,
         * цикл тут же вернётся к подаче входных данных - так конвейер
         * заполняется максимально плотно.
         */
        private const val OUTPUT_TIMEOUT_US = 0L

        /** Размер буфера в пуле. Покрывает ключевой кадр 1080p с запасом. */
        private const val POOL_BUFFER_SIZE = 1024 * 1024

        /** Сколько буферов держать в пуле. */
        private const val POOL_SIZE = 12

        /** Периодичность отправки телеметрии, кадров. */
        private const val STATS_INTERVAL_FRAMES = 120L

        /**
         * Определяет максимальное разрешение, которое тянет аппаратный
         * декодер H.264 на этом устройстве.
         *
         * Нужно ДО подключения: стример обязан знать предел, чтобы не прислать
         * поток, который устройство не сможет декодировать аппаратно. Переход
         * на программный декодер даёт задержку в сотни миллисекунд и делает
         * всю систему бессмысленной.
         *
         * @return пара (ширина, высота) либо (0, 0), если определить не удалось
         */
        fun queryMaxHardwareResolution(): Pair<Int, Int> {
            var bestWidth = 0
            var bestHeight = 0

            try {
                val codecList = MediaCodecList(MediaCodecList.REGULAR_CODECS)
                for (info in codecList.codecInfos) {
                    if (info.isEncoder) continue

                    // Программные декодеры отсеиваем: их предел разрешения
                    // формально высок, но практической ценности не имеет.
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && !info.isHardwareAccelerated) {
                        continue
                    }

                    val supportsAvc = info.supportedTypes.any { it.equals(MIME_TYPE, ignoreCase = true) }
                    if (!supportsAvc) continue

                    val videoCaps = info.getCapabilitiesForType(MIME_TYPE).videoCapabilities ?: continue

                    val maxW = videoCaps.supportedWidths.upper
                    val maxH = videoCaps.supportedHeights.upper

                    // Берём декодер с наибольшей площадью кадра.
                    if (maxW.toLong() * maxH > bestWidth.toLong() * bestHeight) {
                        bestWidth = maxW
                        bestHeight = maxH
                    }

                    Log.i(TAG, "Найден аппаратный декодер ${info.name}: до ${maxW}x${maxH}")
                }
            } catch (e: Exception) {
                Log.e(TAG, "Не удалось перечислить декодеры: ${e.message}", e)
            }

            if (bestWidth == 0) {
                Log.w(TAG, "Аппаратный декодер H.264 не найден - предел разрешения неизвестен")
            }

            return Pair(bestWidth, bestHeight)
        }
    }

    /** Один пакет в очереди на декодирование. */
    private class Packet {
        var data: ByteArray = ByteArray(POOL_BUFFER_SIZE)
        var length: Int = 0
        var nalType: Int = 0
        var isConfig: Boolean = false
        var isKeyFrame: Boolean = false
        var arrivalNanos: Long = 0
    }

    private var codec: MediaCodec? = null
    private var decoderThread: Thread? = null

    private val running = AtomicBoolean(false)
    private val firstFrameReported = AtomicBoolean(false)

    /** Очередь на декодирование. */
    private val pendingPackets = ArrayBlockingQueue<Packet>(QUEUE_CAPACITY)

    /** Пул переиспользуемых буферов - защита от пауз сборщика мусора. */
    private val bufferPool = ConcurrentLinkedQueue<Packet>()

    // --- Статистика ---------------------------------------------------------
    private val framesDecoded = AtomicLong(0)
    private val framesDropped = AtomicLong(0)
    private var lastLatencyUs = 0

    /**
     * Запуск декодера.
     * @return true, если MediaCodec успешно сконфигурирован и запущен
     */
    fun start(): Boolean {
        if (running.get()) {
            Log.w(TAG, "Декодер уже запущен")
            return true
        }

        // Наполняем пул заранее: выделение памяти в горячем пути недопустимо.
        repeat(POOL_SIZE) { bufferPool.offer(Packet()) }

        return try {
            val format = buildFormat()

            val decoder = MediaCodec.createDecoderByType(MIME_TYPE)

            // Третий аргумент - crypto, четвёртый - флаги. Ноль означает
            // «режим декодера» (для кодировщика был бы CONFIGURE_FLAG_ENCODE).
            decoder.configure(format, surface, null, 0)
            decoder.start()

            codec = decoder
            running.set(true)

            decoderThread = Thread({ decoderLoop() }, "PhoneMonitor-Decoder").apply {
                // Приоритет уровня аудио: планировщик Android даёт таким
                // потокам минимальную задержку пробуждения.
                priority = Thread.MAX_PRIORITY
                start()
            }

            Log.i(TAG, "Декодер запущен: ${width}x${height}")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Не удалось запустить декодер: ${e.message}", e)
            listener.onDecoderError("Не удалось запустить декодер: ${e.message}")
            releaseCodec()
            false
        }
    }

    /**
     * Формирование MediaFormat с флагами минимальной задержки.
     */
    private fun buildFormat(): MediaFormat {
        val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height)

        // ---------------------------------------------------------------------
        // KEY_LATENCY = 0.
        //
        // Просит декодер не накапливать кадры перед выдачей. Значение 0
        // трактуется как «выдавать кадр сразу, как только он декодирован».
        // Поддерживается не всеми декодерами, но игнорирование безопасно.
        // ---------------------------------------------------------------------
        format.setInteger(MediaFormat.KEY_LATENCY, 0)

        // ---------------------------------------------------------------------
        // KEY_PRIORITY = 0 - режим реального времени.
        //
        // Ноль означает «задача реального времени», единица - «фоновая».
        // Влияет на то, как планировщик распределяет ресурсы аппаратного
        // блока декодирования между приложениями.
        // ---------------------------------------------------------------------
        format.setInteger(MediaFormat.KEY_PRIORITY, 0)

        // ---------------------------------------------------------------------
        // KEY_LOW_LATENCY (Android 11+).
        //
        // Отдельный, более сильный переключатель: он отключает в декодере
        // переупорядочивание выходных кадров целиком. Для потока без B-кадров
        // (а наш кодировщик настроен именно так) это чистый выигрыш.
        // ---------------------------------------------------------------------
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }

        // ---------------------------------------------------------------------
        // KEY_OPERATING_RATE.
        //
        // Просит систему держать блок декодирования на максимальной частоте.
        // Без этого энергосберегающий регулятор может занизить частоту и
        // добавить несколько миллисекунд на каждый кадр.
        // ---------------------------------------------------------------------
        format.setInteger(MediaFormat.KEY_OPERATING_RATE, Short.MAX_VALUE.toInt())

        // Цветовой формат явно не задаём: при выводе на Surface декодер сам
        // выберет свой нативный формат, и любое наше вмешательство привело бы
        // к лишней конверсии.

        Log.i(TAG, "Формат декодера: $format")
        return format
    }

    /**
     * Постановка NAL-юнита в очередь. Вызывается из сетевого потока.
     *
     * Копирование здесь необходимо: буфер разборщика переиспользуется, и к
     * моменту, когда до пакета доберётся поток декодера, его содержимое уже
     * будет затёрто новыми данными из сокета.
     *
     * @return false, если пакет пришлось отбросить из-за переполнения очереди
     */
    fun submit(data: ByteArray, offset: Int, length: Int, nalType: Int): Boolean {
        if (!running.get()) return false

        if (length > POOL_BUFFER_SIZE) {
            Log.w(TAG, "NAL размером $length байт не помещается в буфер пула - отброшен")
            framesDropped.incrementAndGet()
            return false
        }

        val packet = bufferPool.poll() ?: Packet()

        System.arraycopy(data, offset, packet.data, 0, length)
        packet.length = length
        packet.nalType = nalType
        packet.isConfig = (nalType == NalParser.NAL_TYPE_SPS || nalType == NalParser.NAL_TYPE_PPS)
        packet.isKeyFrame = (nalType == NalParser.NAL_TYPE_SLICE_IDR)
        packet.arrivalNanos = System.nanoTime()

        if (pendingPackets.offer(packet)) {
            return true
        }

        // ---------------------------------------------------------------------
        // Очередь переполнена: декодер не успевает.
        //
        // Политика вытеснения: выбрасываем САМЫЙ СТАРЫЙ пакет, а не новый.
        // Показать свежий кадр с артефактами лучше, чем показать корректный,
        // но устаревший: пользователь двигает мышью здесь и сейчас.
        //
        // Исключение - параметры последовательности (SPS/PPS) и ключевые кадры:
        // без них декодер не восстановится вовсе, поэтому их не вытесняем.
        // ---------------------------------------------------------------------
        val oldest = pendingPackets.poll()
        if (oldest != null) {
            if (oldest.isConfig || oldest.isKeyFrame) {
                // Старый пакет слишком ценен - возвращаем его, а новый отбрасываем.
                pendingPackets.offer(oldest)
                bufferPool.offer(packet)
                framesDropped.incrementAndGet()
                return false
            }
            bufferPool.offer(oldest)
            framesDropped.incrementAndGet()
        }

        val queued = pendingPackets.offer(packet)
        if (!queued) {
            bufferPool.offer(packet)
            framesDropped.incrementAndGet()
        }

        // Цепочка опорных кадров нарушена - просим новый ключевой кадр.
        listener.onKeyFrameNeeded()
        return queued
    }

    /**
     * Главный цикл потока декодера.
     *
     * На каждой итерации выполняются две независимые операции:
     *   1) подача входных данных (если они есть);
     *   2) выдача готовых кадров на поверхность.
     *
     * Они намеренно НЕ связаны отношением «один вход - один выход»: декодер
     * может выдать кадр с задержкой в несколько входных буферов, и ожидание
     * выхода после каждого входа создало бы искусственную сериализацию.
     */
    private fun decoderLoop() {
        // Приоритет потока на уровне ОС. THREAD_PRIORITY_URGENT_AUDIO - самый
        // высокий приоритет, доступный обычному приложению; он ставит поток
        // в один класс с воспроизведением звука, где пропуск кванта недопустим.
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)

        val bufferInfo = MediaCodec.BufferInfo()
        val decoder = codec ?: return

        /**
         * Пакет, который не удалось подать в прошлой итерации.
         *
         * Возвращать его в очередь через offer() НЕЛЬЗЯ: ArrayBlockingQueue
         * добавляет в хвост, и NAL-юниты поменялись бы местами. Для H.264
         * это означает повреждение потока - декодер получил бы слайс раньше
         * его SPS или кадры в неверном порядке. Поэтому пакет удерживается
         * здесь, в локальной переменной потока, и подаётся первым на
         * следующей итерации.
         */
        var retryPacket: Packet? = null

        Log.i(TAG, "Поток декодера вошёл в рабочий цикл")

        while (running.get()) {
            try {
                // -------------------------------------------------------------
                // Фаза 1. Подача входных данных.
                // -------------------------------------------------------------
                val packet = retryPacket ?: pendingPackets.poll(2, TimeUnit.MILLISECONDS)
                retryPacket = null

                if (packet != null) {
                    val inputIndex = decoder.dequeueInputBuffer(INPUT_TIMEOUT_US)

                    if (inputIndex >= 0) {
                        val inputBuffer = decoder.getInputBuffer(inputIndex)

                        if (inputBuffer != null) {
                            inputBuffer.clear()
                            inputBuffer.put(packet.data, 0, packet.length)

                            // Флаг CODEC_CONFIG для SPS/PPS: декодер применит
                            // их к конфигурации, а не попытается показать.
                            val flags = if (packet.isConfig) {
                                MediaCodec.BUFFER_FLAG_CODEC_CONFIG
                            } else {
                                0
                            }

                            // Временная метка: используем время прихода пакета
                            // в микросекундах. Точное значение неважно - важна
                            // монотонность, по которой декодер упорядочивает
                            // кадры и по которой мы измеряем задержку.
                            val presentationTimeUs = packet.arrivalNanos / 1000

                            decoder.queueInputBuffer(
                                inputIndex,
                                0,
                                packet.length,
                                presentationTimeUs,
                                flags
                            )
                        } else {
                            Log.w(TAG, "getInputBuffer вернул null для индекса $inputIndex")
                        }
                    } else {
                        // Входного буфера не дождались за 10 мс - декодер занят.
                        // Удерживаем пакет до следующей итерации, сохраняя
                        // порядок NAL-юнитов, и переходим к выдаче готовых
                        // кадров: освободив выход, мы, скорее всего, освободим
                        // и вход.
                        retryPacket = packet
                    }

                    // Буфер возвращается в пул только когда он реально отдан
                    // декодеру. Удерживаемый пакет владеет своим массивом.
                    if (retryPacket == null) {
                        bufferPool.offer(packet)
                    }
                }

                // -------------------------------------------------------------
                // Фаза 2. Выдача готовых кадров.
                //
                // Цикл, а не одиночный вызов: декодер мог накопить несколько
                // кадров, и забрать нужно все - иначе они добавятся к задержке.
                // -------------------------------------------------------------
                var outputIndex = decoder.dequeueOutputBuffer(bufferInfo, OUTPUT_TIMEOUT_US)

                while (outputIndex >= 0) {
                    // ---------------------------------------------------------
                    // releaseOutputBuffer(index, render = true).
                    //
                    // Ключевой вызов всего клиента: он отдаёт декодированный
                    // кадр напрямую композитору, минуя приложение. Никакого
                    // копирования в память приложения не происходит - кадр
                    // остаётся в видеопамяти от декодирования до вывода.
                    //
                    // Вариант с временной меткой (releaseOutputBuffer(index, ts))
                    // НЕ используется намеренно: он задерживает вывод до
                    // указанного момента, а нам нужно показать кадр немедленно.
                    // ---------------------------------------------------------
                    decoder.releaseOutputBuffer(outputIndex, true)

                    val decoded = framesDecoded.incrementAndGet()

                    // Задержка декодирования: от постановки в декодер до вывода.
                    val nowUs = System.nanoTime() / 1000
                    if (bufferInfo.presentationTimeUs > 0) {
                        lastLatencyUs = (nowUs - bufferInfo.presentationTimeUs).toInt()
                    }

                    if (firstFrameReported.compareAndSet(false, true)) {
                        Log.i(TAG, "Показан первый кадр")
                        listener.onFirstFrameRendered()
                    }

                    if (decoded % STATS_INTERVAL_FRAMES == 0L) {
                        listener.onStats(
                            decoded,
                            framesDropped.get(),
                            lastLatencyUs,
                            pendingPackets.size
                        )
                    }

                    if ((bufferInfo.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        Log.i(TAG, "Получен признак конца потока")
                        running.set(false)
                        break
                    }

                    outputIndex = decoder.dequeueOutputBuffer(bufferInfo, OUTPUT_TIMEOUT_US)
                }

                when (outputIndex) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        // Приходит один раз после разбора SPS. Полезно для
                        // журнала: здесь видно, какой размер декодер реально
                        // вывел из потока.
                        Log.i(TAG, "Формат вывода декодера: ${decoder.outputFormat}")
                    }

                    MediaCodec.INFO_TRY_AGAIN_LATER -> {
                        // Норма: готовых кадров нет.
                    }

                    MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED -> {
                        // Устаревший код, оставлен для полноты обработки.
                    }
                }

            } catch (e: MediaCodec.CodecException) {
                // ---------------------------------------------------------
                // Ошибки MediaCodec делятся на восстановимые и фатальные.
                //
                // Восстановимая (isRecoverable) лечится вызовом reset() и
                // повторной конфигурацией. Транзиентная (isTransient) проходит
                // сама - достаточно подождать. Всё остальное фатально.
                // ---------------------------------------------------------
                when {
                    e.isTransient -> {
                        Log.w(TAG, "Временная ошибка декодера, продолжаем: ${e.diagnosticInfo}")
                    }
                    e.isRecoverable -> {
                        Log.w(TAG, "Восстановимая ошибка декодера, выполняем сброс: ${e.diagnosticInfo}")
                        if (!recoverCodec()) {
                            listener.onDecoderError("Не удалось восстановить декодер")
                            running.set(false)
                        }
                    }
                    else -> {
                        Log.e(TAG, "Фатальная ошибка декодера: ${e.diagnosticInfo}", e)
                        listener.onDecoderError("Ошибка декодера: ${e.diagnosticInfo}")
                        running.set(false)
                    }
                }
            } catch (e: IllegalStateException) {
                Log.e(TAG, "Декодер в недопустимом состоянии: ${e.message}", e)
                listener.onDecoderError("Декодер в недопустимом состоянии")
                running.set(false)
            } catch (e: InterruptedException) {
                Log.i(TAG, "Поток декодера прерван")
                Thread.currentThread().interrupt()
                running.set(false)
            }
        }

        Log.i(TAG, "Поток декодера завершён. Декодировано: ${framesDecoded.get()}, " +
                "отброшено: ${framesDropped.get()}")
    }

    /**
     * Сброс и повторная конфигурация декодера после восстановимой ошибки.
     * После сброса декодер не имеет опорных кадров, поэтому обязательно
     * запрашивается новый ключевой кадр.
     */
    private fun recoverCodec(): Boolean {
        val decoder = codec ?: return false

        return try {
            decoder.reset()
            decoder.configure(buildFormat(), surface, null, 0)
            decoder.start()

            pendingPackets.clear()
            firstFrameReported.set(false)

            listener.onKeyFrameNeeded()
            Log.i(TAG, "Декодер успешно восстановлен")
            true
        } catch (e: Exception) {
            Log.e(TAG, "Восстановление декодера не удалось: ${e.message}", e)
            false
        }
    }

    /** Остановка и освобождение ресурсов. Идемпотентна. */
    fun stop() {
        if (!running.compareAndSet(true, false)) {
            return
        }

        decoderThread?.let { thread ->
            thread.interrupt()
            // Полсекунды - с запасом: цикл не выполняет операций длиннее
            // одного вызова MediaCodec.
            thread.join(500)
        }
        decoderThread = null

        releaseCodec()

        pendingPackets.clear()
        bufferPool.clear()

        Log.i(TAG, "Декодер остановлен")
    }

    private fun releaseCodec() {
        codec?.let {
            try {
                it.stop()
            } catch (e: Exception) {
                Log.w(TAG, "Ошибка при остановке MediaCodec: ${e.message}")
            }
            try {
                it.release()
            } catch (e: Exception) {
                Log.w(TAG, "Ошибка при освобождении MediaCodec: ${e.message}")
            }
        }
        codec = null
    }

    val decodedFrameCount: Long get() = framesDecoded.get()
    val droppedFrameCount: Long get() = framesDropped.get()
    val currentLatencyUs: Int get() = lastLatencyUs
    val queueDepth: Int get() = pendingPackets.size
}
