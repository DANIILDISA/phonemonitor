package com.phonemonitor.client

import android.annotation.SuppressLint
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import com.phonemonitor.client.databinding.ActivityMainBinding
import java.util.concurrent.atomic.AtomicBoolean

/**
 * =============================================================================
 *  Главный (и единственный) экран клиента.
 * =============================================================================
 *
 *  Роль главного потока сведена к минимуму. Он делает ровно три вещи:
 *    * держит полноэкранный SurfaceView и не даёт экрану гаснуть;
 *    * запускает и останавливает сетевой поток и декодер по жизненному циклу
 *      поверхности;
 *    * обновляет накладку состояния.
 *
 *  Ни один байт видеопотока через главный поток не проходит: данные идут
 *  по маршруту «сокет -> разборщик -> декодер -> композитор», полностью в
 *  фоновых потоках и видеопамяти.
 *
 *  Жизненный цикл поверхности важнее жизненного цикла активности:
 *  Surface может быть уничтожена и создана заново без пересоздания активности
 *  (например, при повороте с configChanges). Поэтому запуск конвейера привязан
 *  к колбэкам SurfaceHolder, а не к onStart/onStop.
 */
class MainActivity : AppCompatActivity(), SurfaceHolder.Callback,
    StreamServer.Listener, VideoDecoder.Listener {

    companion object {
        private const val TAG = "PhoneMonitor"
    }

    private lateinit var binding: ActivityMainBinding

    private var streamServer: StreamServer? = null
    private var decoder: VideoDecoder? = null
    private var thermalMonitor: ThermalMonitor? = null

    /** Геометрия, которую мы сообщили стримеру. */
    private var announcedHello: Protocol.Hello? = null

    /** Поверхность готова к выводу. */
    private val surfaceReady = AtomicBoolean(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // ---------------------------------------------------------------------
        // Экран не должен гаснуть.
        //
        // FLAG_KEEP_SCREEN_ON предпочтительнее WakeLock: он привязан к окну,
        // а значит автоматически снимается при сворачивании приложения и не
        // может «утечь» и посадить батарею.
        // ---------------------------------------------------------------------
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        enterImmersiveMode()

        // Регистрируем колбэк ДО первой компоновки: surfaceCreated придёт
        // сразу, как только система выделит поверхность.
        binding.videoSurface.holder.addCallback(this)

        // ---------------------------------------------------------------------
        // Тип поверхности.
        //
        // SURFACE_TYPE_PUSH_BUFFERS устарел, но важно другое: мы НЕ рисуем на
        // этой поверхности сами. Единственный её потребитель - MediaCodec,
        // который пишет туда напрямую из аппаратного декодера.
        // ---------------------------------------------------------------------
        binding.videoSurface.holder.setKeepScreenOn(true)

        thermalMonitor = ThermalMonitor(this) { thermal ->
            // Колбэк приходит из фонового потока планировщика.
            streamServer?.sendThermalState(thermal)
        }

        updateStatus(getString(R.string.status_waiting), showOverlay = true)
        binding.diagnosticsText.text = getString(R.string.status_port, Protocol.TCP_PORT)

        Log.i(TAG, "Активность создана. Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
    }

    override fun onResume() {
        super.onResume()
        enterImmersiveMode()
        // При возврате из фона состояние нагрева могло измениться - сообщаем.
        thermalMonitor?.reportCurrentState()
    }

    override fun onDestroy() {
        super.onDestroy()
        stopPipeline()
        thermalMonitor?.stop()
        thermalMonitor = null
        Log.i(TAG, "Активность уничтожена")
    }

    /**
     * Полноэкранный режим без системных панелей.
     *
     * Панели навигации и статуса занимают место и, что важнее, вызывают
     * перекомпоновку окна при появлении - а это лишняя работа композитора
     * прямо в момент показа кадра.
     */
    @Suppress("DEPRECATION")
    private fun enterImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let { controller ->
                controller.hide(WindowInsets.Type.systemBars())
                // BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE: панели показываются
                // по свайпу и сами прячутся - пользователь не теряет доступ
                // к системе, но панели не висят постоянно.
                controller.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                )
        }
    }

    // =========================================================================
    //  Жизненный цикл поверхности
    // =========================================================================
    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "Поверхность создана")
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "Поверхность изменена: ${width}x${height}, формат $format")

        // surfaceChanged приходит и при первом создании, и при каждом изменении
        // размера. Перезапускать конвейер на каждое изменение нельзя - это
        // разорвало бы соединение при любом повороте. Запускаем только один раз.
        if (surfaceReady.compareAndSet(false, true)) {
            startPipeline(holder)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(TAG, "Поверхность уничтожена")

        // ВАЖНО: остановка декодера здесь обязательна и должна быть
        // синхронной. После возврата из surfaceDestroyed поверхность
        // недействительна, и любое обращение к ней из MediaCodec приведёт
        // к аварийному завершению процесса в нативном коде.
        surfaceReady.set(false)
        stopPipeline()
    }

    // =========================================================================
    //  Управление конвейером
    // =========================================================================
    private fun startPipeline(holder: SurfaceHolder) {
        val hello = DisplayInfo.buildHello(this)
        announcedHello = hello

        // ---------------------------------------------------------------------
        // Размер, который получит декодер.
        //
        // Это ОЖИДАЕМЫЙ размер: фактические размеры декодер всё равно возьмёт
        // из SPS во входном потоке. Значения здесь нужны MediaCodec только для
        // предварительного выделения буферов правильного объёма - ошибка на
        // этом этапе стоит одного перевыделения, а не сбоя.
        // ---------------------------------------------------------------------
        val decoderWidth = hello.width
        val decoderHeight = hello.height

        val videoDecoder = VideoDecoder(holder.surface, decoderWidth, decoderHeight, this)
        if (!videoDecoder.start()) {
            updateStatus(getString(R.string.status_error, "не удалось запустить декодер"),
                showOverlay = true)
            return
        }
        decoder = videoDecoder

        val server = StreamServer(
            port = Protocol.TCP_PORT,
            helloProvider = { announcedHello ?: DisplayInfo.buildHello(this) },
            listener = this
        )
        server.start()
        streamServer = server

        thermalMonitor?.start()

        Log.i(TAG, "Конвейер запущен: экран ${hello.width}x${hello.height}, " +
                "порт ${Protocol.TCP_PORT}")
    }

    private fun stopPipeline() {
        // Порядок остановки обратный порядку запуска: сначала прекращаем
        // приток данных, затем останавливаем их потребителя. Обратный порядок
        // привёл бы к тому, что сетевой поток продолжал бы слать пакеты
        // в уже уничтоженный декодер.
        streamServer?.stop()
        streamServer = null

        decoder?.stop()
        decoder = null

        Log.i(TAG, "Конвейер остановлен")
    }

    // =========================================================================
    //  StreamServer.Listener
    // =========================================================================
    override fun onListening(port: Int) {
        runOnUiThread {
            updateStatus(getString(R.string.status_waiting), showOverlay = true)
            binding.diagnosticsText.text = getString(R.string.status_port, port)
        }
    }

    override fun onConnected(remoteAddress: String) {
        runOnUiThread {
            updateStatus(getString(R.string.status_connected, remoteAddress), showOverlay = true)
        }
        // При подключении сразу сообщаем текущее тепловое состояние: стример
        // должен знать его до выбора разрешения, а не через десять секунд.
        thermalMonitor?.reportCurrentState()
    }

    override fun onDisconnected(reason: String) {
        runOnUiThread {
            updateStatus(getString(R.string.status_disconnected), showOverlay = true)
            binding.diagnosticsText.text = reason
        }
        Log.i(TAG, "Соединение разорвано: $reason")
    }

    override fun onNalUnit(data: ByteArray, offset: Int, length: Int, nalType: Int) {
        // Горячий путь. Вызывается из сетевого потока десятки раз в секунду,
        // поэтому здесь нет ни логирования, ни обращений к UI.
        decoder?.submit(data, offset, length, nalType)
    }

    override fun onFatalError(message: String) {
        runOnUiThread {
            updateStatus(getString(R.string.status_error, message), showOverlay = true)
        }
        Log.e(TAG, "Неустранимая ошибка сервера: $message")
    }

    // =========================================================================
    //  VideoDecoder.Listener
    // =========================================================================
    override fun onFirstFrameRendered() {
        runOnUiThread {
            // Убираем накладку: с этого момента поверхность занята видео.
            binding.statusOverlay.visibility = View.GONE
        }
        Log.i(TAG, "Первый кадр показан - накладка скрыта")
    }

    override fun onKeyFrameNeeded() {
        // Вызывается из потока декодера. Отправка потокобезопасна.
        streamServer?.requestKeyFrame()
    }

    override fun onDecoderError(message: String) {
        runOnUiThread {
            updateStatus(getString(R.string.status_error, message), showOverlay = true)
        }
        Log.e(TAG, "Ошибка декодера: $message")
    }

    override fun onStats(framesDecoded: Long, framesDropped: Long, latencyUs: Int, queueDepth: Int) {
        // Отправляем телеметрию стримеру - она попадёт в его журнал и поможет
        // понять, на чьей стороне возникла задержка.
        streamServer?.sendStats(
            Protocol.Stats(
                framesDecoded = framesDecoded,
                framesDropped = framesDropped,
                decodeLatencyUs = latencyUs,
                queueDepth = queueDepth
            )
        )

        Log.d(TAG, "Декодировано $framesDecoded, отброшено $framesDropped, " +
                "задержка ${latencyUs / 1000.0} мс, очередь $queueDepth")
    }

    // =========================================================================
    @SuppressLint("SetTextI18n")
    private fun updateStatus(text: String, showOverlay: Boolean) {
        binding.statusText.text = text
        binding.statusOverlay.visibility = if (showOverlay) View.VISIBLE else View.GONE
    }
}
