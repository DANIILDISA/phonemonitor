package com.phonemonitor.client

import java.io.DataOutputStream
import java.io.IOException
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * =============================================================================
 *  Управляющий протокол PhoneMonitor (телефон -> ПК).
 * =============================================================================
 *
 *  Зеркало заголовка common/pm_protocol.h. Любое изменение там обязано быть
 *  повторено здесь, иначе стример разорвёт соединение по несовпадению версии.
 *
 *  Порядок байт - BIG-ENDIAN. Это осознанный выбор: ByteBuffer в Java по
 *  умолчанию работает в big-endian, и отказ от явной смены порядка убирает
 *  целый класс ошибок, которые проявляются только на реальном устройстве.
 *
 *  Направление данных в соединении асимметрично:
 *    телефон -> ПК : управляющие кадры этого файла;
 *    ПК -> телефон : сырой Annex B поток H.264 без обёртки.
 */
object Protocol {

    /** Сигнатура управляющего кадра: байты 'P','M','C','1'. */
    const val CTRL_MAGIC: Int = 0x504D4331

    /** Версия управляющего протокола. */
    const val VERSION: Int = 1

    /** Порт по обе стороны adb-форварда. */
    const val TCP_PORT: Int = 6100

    /** Размер заголовка управляющего кадра в байтах. */
    const val HEADER_SIZE: Int = 12

    // --- Типы сообщений -----------------------------------------------------
    const val MSG_HELLO: Int = 1
    const val MSG_THERMAL: Int = 2
    const val MSG_STATS: Int = 3
    const val MSG_KEYFRAME_REQ: Int = 4
    const val MSG_BYE: Int = 5

    // --- Тепловые состояния (зеркало PowerManager.THERMAL_STATUS_*) ---------
    const val THERMAL_NONE: Int = 0
    const val THERMAL_LIGHT: Int = 1
    const val THERMAL_MODERATE: Int = 2
    const val THERMAL_SEVERE: Int = 3
    const val THERMAL_CRITICAL: Int = 4
    const val THERMAL_EMERGENCY: Int = 5
    const val THERMAL_SHUTDOWN: Int = 6

    /**
     * Геометрия экрана, отправляемая первым пакетом.
     *
     * @param width           физическая ширина экрана в пикселях
     * @param height          физическая высота экрана в пикселях
     * @param refreshMilliHz  частота обновления, умноженная на 1000
     * @param densityDpi      плотность экрана
     * @param maxDecoderWidth максимальная ширина, которую тянет аппаратный декодер
     * @param maxDecoderHeight максимальная высота аппаратного декодера
     */
    data class Hello(
        val width: Int,
        val height: Int,
        val refreshMilliHz: Int,
        val densityDpi: Int,
        val maxDecoderWidth: Int,
        val maxDecoderHeight: Int
    )

    /** Тепловое и батарейное состояние устройства. */
    data class Thermal(
        val thermalStatus: Int,
        val batteryPercent: Int,
        val isCharging: Boolean
    )

    /** Телеметрия декодера для диагностики задержки. */
    data class Stats(
        val framesDecoded: Long,
        val framesDropped: Long,
        val decodeLatencyUs: Int,
        val queueDepth: Int
    )

    /**
     * Собирает заголовок управляющего кадра.
     *
     * Буфер создаётся на каждый вызов намеренно: управляющих сообщений
     * единицы в секунду, и экономия здесь была бы преждевременной
     * оптимизацией ценой потокобезопасности.
     */
    private fun buildHeader(type: Int, payloadLength: Int): ByteArray {
        val buffer = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.BIG_ENDIAN)
        buffer.putInt(CTRL_MAGIC)
        buffer.putShort(VERSION.toShort())
        buffer.putShort(type.toShort())
        buffer.putInt(payloadLength)
        return buffer.array()
    }

    /**
     * Отправка сообщения целиком.
     *
     * Заголовок и тело пишутся одним вызовом write: разбиение на два вызова
     * при выключенном алгоритме Нейгла породило бы два TCP-сегмента вместо
     * одного, а в худшем случае - частичное сообщение на стороне стримера,
     * если соединение оборвётся между вызовами.
     */
    @Throws(IOException::class)
    private fun sendMessage(stream: OutputStream, type: Int, payload: ByteArray) {
        val header = buildHeader(type, payload.size)
        val packet = ByteArray(header.size + payload.size)
        System.arraycopy(header, 0, packet, 0, header.size)
        if (payload.isNotEmpty()) {
            System.arraycopy(payload, 0, packet, header.size, payload.size)
        }
        synchronized(stream) {
            stream.write(packet)
            stream.flush()
        }
    }

    /**
     * Отправляет HELLO - обязательный первый пакет соединения.
     * Стример не начнёт кодирование, пока не получит и не разберёт его.
     */
    @Throws(IOException::class)
    fun sendHello(stream: DataOutputStream, hello: Hello) {
        val payload = ByteBuffer.allocate(28).order(ByteOrder.BIG_ENDIAN).apply {
            putInt(hello.width)
            putInt(hello.height)
            putInt(hello.refreshMilliHz)
            putInt(hello.densityDpi)
            putInt(hello.maxDecoderWidth)
            putInt(hello.maxDecoderHeight)
            putInt(0) // flags, зарезервировано
        }.array()

        sendMessage(stream, MSG_HELLO, payload)
    }

    /** Отправляет отчёт о тепловом состоянии. */
    @Throws(IOException::class)
    fun sendThermal(stream: DataOutputStream, thermal: Thermal) {
        val payload = ByteBuffer.allocate(16).order(ByteOrder.BIG_ENDIAN).apply {
            putInt(thermal.thermalStatus)
            putInt(thermal.batteryPercent)
            putInt(if (thermal.isCharging) 1 else 0)
            putInt(0) // reserved
        }.array()

        sendMessage(stream, MSG_THERMAL, payload)
    }

    /** Отправляет телеметрию декодера. */
    @Throws(IOException::class)
    fun sendStats(stream: DataOutputStream, stats: Stats) {
        val payload = ByteBuffer.allocate(24).order(ByteOrder.BIG_ENDIAN).apply {
            putLong(stats.framesDecoded)
            putLong(stats.framesDropped)
            putInt(stats.decodeLatencyUs)
            putInt(stats.queueDepth)
        }.array()

        sendMessage(stream, MSG_STATS, payload)
    }

    /**
     * Просит стример немедленно выдать ключевой кадр.
     * Вызывается при обнаружении повреждения потока или при старте декодера
     * в середине последовательности.
     */
    @Throws(IOException::class)
    fun sendKeyFrameRequest(stream: DataOutputStream) {
        sendMessage(stream, MSG_KEYFRAME_REQ, ByteArray(0))
    }

    /** Корректное завершение сессии. */
    @Throws(IOException::class)
    fun sendBye(stream: DataOutputStream) {
        sendMessage(stream, MSG_BYE, ByteArray(0))
    }
}
