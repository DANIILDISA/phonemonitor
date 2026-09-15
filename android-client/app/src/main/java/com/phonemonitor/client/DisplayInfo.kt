package com.phonemonitor.client

import android.app.Activity
import android.content.Context
import android.os.Build
import android.util.DisplayMetrics
import android.util.Log
import android.view.Display
import android.view.WindowManager

/**
 * =============================================================================
 *  Определение физической геометрии экрана.
 * =============================================================================
 *
 *  Задача звучит тривиально, но на Android имеет три подводных камня:
 *
 *  1. API получения размеров менялся трижды. До Android 11 работал
 *     Display.getRealMetrics, с Android 11 он объявлен устаревшим в пользу
 *     WindowManager.getCurrentWindowMetrics. Оба нужно поддерживать, так как
 *     minSdk у нас 26.
 *
 *  2. «Размер экрана» бывает трёх видов: рабочая область (без системных
 *     панелей), размер окна и ФИЗИЧЕСКОЕ разрешение панели. Нам нужно
 *     последнее - именно оно определяет, сколько пикселей реально может
 *     показать устройство, и именно под него настраивается виртуальный
 *     монитор в Windows.
 *
 *  3. Вырезы под камеру и скруглённые углы не уменьшают физическое
 *     разрешение, но уменьшают полезную площадь. Мы сознательно берём полное
 *     физическое разрешение: обрезка под вырез - дело композитора Android,
 *     а не источника изображения.
 */
object DisplayInfo {

    private const val TAG = "DisplayInfo"

    /** Полное описание экрана, отправляемое стримеру. */
    data class ScreenGeometry(
        val width: Int,
        val height: Int,
        val refreshMilliHz: Int,
        val densityDpi: Int
    )

    /**
     * Возвращает физическую геометрию экрана.
     *
     * @param activity активность нужна для доступа к WindowManager,
     *                 привязанному к конкретному дисплею (важно для устройств
     *                 с двумя экранами и для режима рабочего стола)
     */
    fun queryPhysicalGeometry(activity: Activity): ScreenGeometry {
        // val без инициализатора: обе ветки ниже присваивают значение ровно
        // один раз, и компилятор это проверяет. Инициализация нулём была бы
        // не просто лишней - она замаскировала бы ветку, забывшую присвоить.
        val width: Int
        val height: Int

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // ---------------------------------------------------------------
            // Android 11+: современный путь.
            //
            // getMaximumWindowMetrics возвращает границы, доступные приложению
            // при максимальном размере окна, - на обычном телефоне это и есть
            // физическое разрешение панели.
            // ---------------------------------------------------------------
            val windowManager = activity.getSystemService(Context.WINDOW_SERVICE) as WindowManager
            val bounds = windowManager.maximumWindowMetrics.bounds
            width = bounds.width()
            height = bounds.height()

            Log.i(TAG, "Геометрия по WindowMetrics (API ${Build.VERSION.SDK_INT}): ${width}x${height}")
        } else {
            // ---------------------------------------------------------------
            // Android 8.0 - 10: getRealMetrics.
            //
            // Именно getRealMetrics, а не getMetrics: второй вычитает
            // системные панели и вернул бы заниженное значение.
            // ---------------------------------------------------------------
            @Suppress("DEPRECATION")
            val display = activity.windowManager.defaultDisplay
            val metrics = DisplayMetrics()
            @Suppress("DEPRECATION")
            display.getRealMetrics(metrics)

            width = metrics.widthPixels
            height = metrics.heightPixels

            Log.i(TAG, "Геометрия по getRealMetrics (API ${Build.VERSION.SDK_INT}): ${width}x${height}")
        }

        val display = currentDisplay(activity)

        // Частота обновления. Умножаем на 1000, чтобы передать дробное значение
        // целым числом: реальные панели имеют частоты вроде 59.94 или 119.98 Гц.
        val refreshRate = display?.refreshRate ?: 60.0f
        val refreshMilliHz = (refreshRate * 1000f).toInt()

        val densityDpi = activity.resources.displayMetrics.densityDpi

        Log.i(TAG, "Экран: ${width}x${height}, ${refreshRate} Гц, ${densityDpi} dpi")

        return ScreenGeometry(
            width = width,
            height = height,
            refreshMilliHz = refreshMilliHz,
            densityDpi = densityDpi
        )
    }

    /**
     * Текущий дисплей активности с учётом различий API.
     */
    private fun currentDisplay(activity: Activity): Display? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // На API 30+ display активности - правильный источник: он
            // учитывает, на каком именно экране открыто окно.
            activity.display
        } else {
            @Suppress("DEPRECATION")
            activity.windowManager.defaultDisplay
        }
    }

    /**
     * Собирает пакет HELLO: геометрия экрана плюс пределы аппаратного декодера.
     *
     * Пределы декодера запрашиваются здесь же, потому что они нужны стримеру
     * одновременно с разрешением: без них он может прислать поток, который
     * устройство не сможет декодировать аппаратно.
     */
    fun buildHello(activity: Activity): Protocol.Hello {
        val geometry = queryPhysicalGeometry(activity)
        val (maxDecoderWidth, maxDecoderHeight) = VideoDecoder.queryMaxHardwareResolution()

        // ---------------------------------------------------------------------
        // Нормализация ориентации пределов декодера.
        //
        // Декодеры объявляют пределы в «альбомной» системе координат
        // (например, 4096x2304), а экран телефона обычно портретный
        // (1080x2400). Прямое сравнение дало бы ложное превышение по высоте.
        // Поэтому приводим предел к ориентации экрана.
        // ---------------------------------------------------------------------
        val screenIsPortrait = geometry.height > geometry.width

        val (limitWidth, limitHeight) = if (maxDecoderWidth > 0 && maxDecoderHeight > 0) {
            val decoderIsLandscape = maxDecoderWidth > maxDecoderHeight
            if (screenIsPortrait && decoderIsLandscape) {
                // Разворачиваем предел под портретную ориентацию.
                Pair(maxDecoderHeight, maxDecoderWidth)
            } else {
                Pair(maxDecoderWidth, maxDecoderHeight)
            }
        } else {
            // Предел неизвестен - сообщаем нули, стример воспримет это как
            // «ограничений нет» и возьмёт разрешение экрана как есть.
            Pair(0, 0)
        }

        Log.i(TAG, "Предел аппаратного декодера после нормализации: ${limitWidth}x${limitHeight}")

        return Protocol.Hello(
            width = geometry.width,
            height = geometry.height,
            refreshMilliHz = geometry.refreshMilliHz,
            densityDpi = geometry.densityDpi,
            maxDecoderWidth = limitWidth,
            maxDecoderHeight = limitHeight
        )
    }
}
