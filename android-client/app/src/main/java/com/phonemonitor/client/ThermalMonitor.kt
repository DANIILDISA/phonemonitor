package com.phonemonitor.client

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import android.util.Log
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * =============================================================================
 *  Отслеживание теплового состояния и заряда устройства.
 * =============================================================================
 *
 *  Зачем это нужно потоковому монитору:
 *  аппаратный декодер H.264 при 1080p60 нагревает телефон. Когда устройство
 *  входит в троттлинг, частота декодера падает, кадры начинают выпадать, и
 *  задержка растёт лавинообразно - причём восстановиться сама система уже
 *  не может, пока не остынет.
 *
 *  Правильная стратегия - реагировать НА РАННЕМ этапе, при первом же сигнале
 *  THERMAL_STATUS_LIGHT: попросить ПК снизить разрешение до 720p. Нагрузка
 *  на декодер падает примерно вчетверо, устройство остывает, и до жёсткого
 *  троттлинга дело не доходит.
 *
 *  Источники данных:
 *    * PowerManager.getCurrentThermalStatus() - API 29+. На более старых
 *      версиях API нет, и используется косвенная оценка по температуре
 *      батареи из ACTION_BATTERY_CHANGED.
 *    * PowerManager.addThermalStatusListener() - API 29+, мгновенные
 *      уведомления об изменении. Периодический опрос всё равно оставлен как
 *      страховка: часть прошивок не вызывает слушателя надёжно.
 */
class ThermalMonitor(
    private val context: Context,
    private val listener: (Protocol.Thermal) -> Unit
) {

    companion object {
        private const val TAG = "ThermalMonitor"

        /** Период принудительного опроса состояния, секунды. */
        private const val POLL_INTERVAL_SECONDS = 10L

        /**
         * Пороги оценки нагрева по температуре батареи для API < 29.
         * Значения в десятых долях градуса Цельсия - именно в таком виде их
         * отдаёт BatteryManager.EXTRA_TEMPERATURE.
         *
         * Батарея греется медленнее процессора, поэтому пороги подобраны
         * с запасом: 39 °C на батарее обычно соответствует уже заметному
         * нагреву системы на кристалле.
         */
        private const val BATTERY_TEMP_LIGHT_DECI_C = 390     // 39.0 °C
        private const val BATTERY_TEMP_MODERATE_DECI_C = 420  // 42.0 °C
        private const val BATTERY_TEMP_SEVERE_DECI_C = 450    // 45.0 °C
    }

    private var scheduler: ScheduledExecutorService? = null
    private var powerManager: PowerManager? = null
    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null
    private var batteryReceiver: BroadcastReceiver? = null

    /** Последнее отправленное состояние - чтобы не слать одно и то же. */
    private var lastReportedStatus: Int = -1

    /** Последняя известная температура батареи, десятые доли °C. */
    @Volatile
    private var batteryTemperatureDeciC: Int = 0

    @Volatile
    private var batteryPercent: Int = -1

    @Volatile
    private var isCharging: Boolean = false

    fun start() {
        powerManager = context.getSystemService(Context.POWER_SERVICE) as? PowerManager

        registerBatteryReceiver()

        // ---------------------------------------------------------------------
        // Подписка на изменения теплового состояния (Android 10+).
        //
        // Слушатель вызывается системой в главном потоке приложения, поэтому
        // внутри него нельзя делать ничего тяжёлого - только передать значение
        // дальше.
        // ---------------------------------------------------------------------
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val pm = powerManager
            if (pm != null) {
                val callback = PowerManager.OnThermalStatusChangedListener { status ->
                    Log.i(TAG, "Система сообщила о смене теплового состояния: $status")
                    report(status)
                }
                thermalListener = callback
                try {
                    pm.addThermalStatusListener(callback)
                    Log.i(TAG, "Подписка на тепловые уведомления оформлена")
                } catch (e: Exception) {
                    Log.w(TAG, "Не удалось подписаться на тепловые уведомления: ${e.message}")
                }
            }
        } else {
            Log.i(TAG, "Android ${Build.VERSION.SDK_INT}: API теплового состояния недоступен, " +
                    "используется оценка по температуре батареи")
        }

        // Периодический опрос - страховка на случай, если слушатель не сработает.
        scheduler = Executors.newSingleThreadScheduledExecutor { runnable ->
            Thread(runnable, "PhoneMonitor-Thermal").apply {
                // Низкий приоритет: задача фоновая и не должна конкурировать
                // с декодером за процессорное время.
                priority = Thread.MIN_PRIORITY
            }
        }.also { executor ->
            executor.scheduleWithFixedDelay(
                { pollAndReport() },
                2, POLL_INTERVAL_SECONDS, TimeUnit.SECONDS
            )
        }
    }

    fun stop() {
        scheduler?.shutdownNow()
        scheduler = null

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            thermalListener?.let { callback ->
                try {
                    powerManager?.removeThermalStatusListener(callback)
                } catch (e: Exception) {
                    Log.d(TAG, "Снятие подписки не удалось: ${e.message}")
                }
            }
        }
        thermalListener = null

        batteryReceiver?.let { receiver ->
            try {
                context.unregisterReceiver(receiver)
            } catch (e: IllegalArgumentException) {
                // Приёмник уже снят - не ошибка.
            }
        }
        batteryReceiver = null

        Log.i(TAG, "Мониторинг нагрева остановлен")
    }

    /** Принудительная отправка текущего состояния (например, при подключении). */
    fun reportCurrentState() {
        lastReportedStatus = -1   // сбрасываем дедупликацию
        pollAndReport()
    }

    // -------------------------------------------------------------------------
    private fun registerBatteryReceiver() {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                intent ?: return

                batteryTemperatureDeciC = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0)

                val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
                batteryPercent = if (level >= 0 && scale > 0) {
                    (level * 100) / scale
                } else {
                    -1
                }

                val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
                isCharging = (status == BatteryManager.BATTERY_STATUS_CHARGING ||
                        status == BatteryManager.BATTERY_STATUS_FULL)
            }
        }

        batteryReceiver = receiver

        // ACTION_BATTERY_CHANGED - «липкое» широковещательное сообщение:
        // registerReceiver сразу возвращает последнее значение, поэтому
        // данные о батарее доступны немедленно, без ожидания события.
        val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
        val sticky = context.registerReceiver(receiver, filter)
        sticky?.let { receiver.onReceive(context, it) }
    }

    private fun pollAndReport() {
        val status = currentThermalStatus()
        report(status)
    }

    /**
     * Текущее тепловое состояние.
     * На API 29+ берётся напрямую у системы, иначе оценивается по батарее.
     */
    private fun currentThermalStatus(): Int {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val pm = powerManager
            if (pm != null) {
                return try {
                    pm.currentThermalStatus
                } catch (e: Exception) {
                    Log.w(TAG, "Чтение теплового состояния не удалось: ${e.message}")
                    estimateFromBatteryTemperature()
                }
            }
        }
        return estimateFromBatteryTemperature()
    }

    /**
     * Приближённая оценка нагрева по температуре батареи.
     *
     * Это заведомо грубая эвристика, но на устройствах до Android 10 иного
     * источника нет, а полное отсутствие реакции на нагрев хуже, чем
     * приблизительная.
     */
    private fun estimateFromBatteryTemperature(): Int {
        val temperature = batteryTemperatureDeciC
        if (temperature <= 0) {
            return Protocol.THERMAL_NONE
        }

        return when {
            temperature >= BATTERY_TEMP_SEVERE_DECI_C -> Protocol.THERMAL_SEVERE
            temperature >= BATTERY_TEMP_MODERATE_DECI_C -> Protocol.THERMAL_MODERATE
            temperature >= BATTERY_TEMP_LIGHT_DECI_C -> Protocol.THERMAL_LIGHT
            else -> Protocol.THERMAL_NONE
        }
    }

    /**
     * Отправляет состояние наружу, если оно изменилось.
     *
     * Дедупликация важна: опрос идёт каждые 10 секунд, а состояние меняется
     * редко. Отправлять одно и то же значение - значит без нужды дёргать
     * стример и засорять его журнал.
     */
    @Synchronized
    private fun report(status: Int) {
        if (status == lastReportedStatus) {
            return
        }
        lastReportedStatus = status

        val thermal = Protocol.Thermal(
            thermalStatus = status,
            batteryPercent = batteryPercent,
            isCharging = isCharging
        )

        Log.i(TAG, "Тепловое состояние: $status (${thermalName(status)}), " +
                "батарея ${batteryPercent}%, температура ${batteryTemperatureDeciC / 10.0} °C, " +
                "зарядка: ${if (isCharging) "да" else "нет"}")

        listener(thermal)
    }

    private fun thermalName(status: Int): String = when (status) {
        Protocol.THERMAL_NONE -> "норма"
        Protocol.THERMAL_LIGHT -> "лёгкий нагрев"
        Protocol.THERMAL_MODERATE -> "умеренный нагрев"
        Protocol.THERMAL_SEVERE -> "сильный нагрев"
        Protocol.THERMAL_CRITICAL -> "критический нагрев"
        Protocol.THERMAL_EMERGENCY -> "аварийный нагрев"
        Protocol.THERMAL_SHUTDOWN -> "перегрев"
        else -> "неизвестно"
    }
}
