package com.phonemonitor.client

import android.os.Process
import android.util.Log
import java.io.DataOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * =============================================================================
 *  Сетевой поток: приём H.264 от Windows-стримера.
 * =============================================================================
 *
 *  Роли в соединении:
 *    телефон - СЕРВЕР (ServerSocket на 127.0.0.1:6100 внутри устройства);
 *    ПК      - КЛИЕНТ (подключается к 127.0.0.1:6100 на хосте).
 *
 *  Связывает их adb:
 *      adb forward tcp:6100 tcp:6100
 *  Всё, что ПК пишет в свой локальный порт 6100, выходит из порта 6100 внутри
 *  телефона. Наружу, в сеть, ничего не уходит - канал целиком внутри USB.
 *
 *  Именно поэтому сокет привязывается к ЛОКАЛЬНОМУ адресу (127.0.0.1), а не к
 *  0.0.0.0: слушать все интерфейсы означало бы открыть порт для Wi-Fi-сети,
 *  что не нужно функционально и небезопасно.
 *
 *  Обязанности потока:
 *    1. Слушать порт и принимать подключение.
 *    2. Немедленно отправить HELLO с геометрией экрана - без него стример
 *       не начнёт кодирование.
 *    3. В цикле читать сокет, скармливать данные разборщику NAL, а готовые
 *       юниты - декодеру.
 *    4. Обрабатывать разрыв и возвращаться к ожиданию нового подключения.
 */
class StreamServer(
    private val port: Int,
    private val helloProvider: () -> Protocol.Hello,
    private val listener: Listener
) {

    interface Listener {
        /** Порт открыт, ждём подключения. */
        fun onListening(port: Int)

        /** Установлено подключение с ПК. */
        fun onConnected(remoteAddress: String)

        /** Соединение разорвано. */
        fun onDisconnected(reason: String)

        /** Готовый NAL-юнит для декодера. */
        fun onNalUnit(data: ByteArray, offset: Int, length: Int, nalType: Int)

        /** Ошибка, после которой сервер не может продолжать. */
        fun onFatalError(message: String)
    }

    companion object {
        private const val TAG = "StreamServer"

        /**
         * Размер буфера чтения из сокета.
         *
         * 256 КиБ покрывает ключевой кадр 1080p целиком, что позволяет забрать
         * его за один read. Меньший буфер означал бы больше системных вызовов,
         * каждый из которых - переключение контекста.
         */
        private const val READ_BUFFER_SIZE = 256 * 1024

        /**
         * Таймаут accept, мс. Нужен, чтобы поток периодически просыпался и
         * проверял флаг остановки, а не висел в accept навсегда.
         */
        private const val ACCEPT_TIMEOUT_MS = 1000

        /**
         * Таймаут чтения, мс. Больше одной секунды - значит поток встал:
         * либо стример завис, либо кабель выдернули.
         */
        private const val READ_TIMEOUT_MS = 5000

        /** Длина очереди ожидающих подключений. Нам хватит одного. */
        private const val BACKLOG = 1
    }

    private val running = AtomicBoolean(false)
    private var serverThread: Thread? = null
    private var serverSocket: ServerSocket? = null

    /**
     * Поток вывода активного соединения.
     *
     * Хранится в AtomicReference, потому что из него пишут сразу несколько
     * потоков: главный (телеметрия нагрева) и поток декодера (запросы
     * ключевого кадра). Сам Protocol синхронизирует запись по объекту потока.
     */
    private val outputStream = AtomicReference<DataOutputStream?>(null)

    /** Разборщик NAL. Создаётся один раз - он держит большой буфер. */
    private val nalParser = NalParser { data, offset, length, nalType ->
        listener.onNalUnit(data, offset, length, nalType)
    }

    fun start() {
        if (running.get()) {
            Log.w(TAG, "Сервер уже запущен")
            return
        }

        running.set(true)
        serverThread = Thread({ serverLoop() }, "PhoneMonitor-Network").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        if (!running.compareAndSet(true, false)) {
            return
        }

        // Корректно прощаемся, если соединение ещё живо.
        outputStream.get()?.let { stream ->
            try {
                Protocol.sendBye(stream)
            } catch (e: IOException) {
                // Соединение уже могло быть разорвано - это не ошибка.
                Log.d(TAG, "Отправить BYE не удалось: ${e.message}")
            }
        }

        // Закрытие сокета разблокирует accept/read в рабочем потоке.
        try {
            serverSocket?.close()
        } catch (e: IOException) {
            Log.d(TAG, "Ошибка при закрытии серверного сокета: ${e.message}")
        }

        serverThread?.let { thread ->
            thread.interrupt()
            thread.join(2000)
        }
        serverThread = null

        Log.i(TAG, "Сервер остановлен")
    }

    /** Запрос ключевого кадра у стримера. Потокобезопасен. */
    fun requestKeyFrame() {
        val stream = outputStream.get() ?: return
        try {
            Protocol.sendKeyFrameRequest(stream)
        } catch (e: IOException) {
            Log.w(TAG, "Не удалось отправить запрос ключевого кадра: ${e.message}")
        }
    }

    /** Отправка теплового состояния. Потокобезопасна. */
    fun sendThermalState(thermal: Protocol.Thermal) {
        val stream = outputStream.get() ?: return
        try {
            Protocol.sendThermal(stream, thermal)
        } catch (e: IOException) {
            Log.w(TAG, "Не удалось отправить тепловое состояние: ${e.message}")
        }
    }

    /** Отправка телеметрии декодера. Потокобезопасна. */
    fun sendStats(stats: Protocol.Stats) {
        val stream = outputStream.get() ?: return
        try {
            Protocol.sendStats(stream, stats)
        } catch (e: IOException) {
            Log.w(TAG, "Не удалось отправить телеметрию: ${e.message}")
        }
    }

    val isConnected: Boolean get() = outputStream.get() != null

    // -------------------------------------------------------------------------
    private fun serverLoop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)

        try {
            // Привязка только к петлевому интерфейсу: наружу порт не выходит.
            val socket = ServerSocket()
            // SO_REUSEADDR обязателен: после разрыва порт остаётся в TIME_WAIT,
            // и повторный запуск приложения иначе получит «адрес уже занят».
            socket.reuseAddress = true
            socket.bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), port), BACKLOG)
            socket.soTimeout = ACCEPT_TIMEOUT_MS

            serverSocket = socket

            Log.i(TAG, "Слушаем 127.0.0.1:$port")
            listener.onListening(port)

        } catch (e: IOException) {
            Log.e(TAG, "Не удалось открыть порт $port: ${e.message}", e)
            listener.onFatalError("Не удалось открыть порт $port: ${e.message}")
            running.set(false)
            return
        }

        while (running.get()) {
            var client: Socket? = null
            try {
                client = serverSocket?.accept() ?: break
                handleConnection(client)

            } catch (e: java.net.SocketTimeoutException) {
                // Штатный выход из accept по таймауту - просто продолжаем.
                continue
            } catch (e: SocketException) {
                if (running.get()) {
                    Log.w(TAG, "Ошибка сокета: ${e.message}")
                }
                // При остановке закрытие сокета тоже приходит сюда - это норма.
            } catch (e: IOException) {
                Log.e(TAG, "Ошибка приёма подключения: ${e.message}", e)
            } finally {
                outputStream.set(null)
                try {
                    client?.close()
                } catch (e: IOException) {
                    Log.d(TAG, "Ошибка при закрытии клиентского сокета: ${e.message}")
                }
            }
        }

        try {
            serverSocket?.close()
        } catch (e: IOException) {
            // Уже закрыт - не важно.
        }
        serverSocket = null

        Log.i(TAG, "Серверный цикл завершён")
    }

    /**
     * Обслуживание одного подключения от начала до разрыва.
     */
    private fun handleConnection(client: Socket) {
        val remote = "${client.inetAddress.hostAddress}:${client.port}"
        Log.i(TAG, "Подключение принято: $remote")

        // ---------------------------------------------------------------------
        // Настройки сокета, влияющие на задержку.
        // ---------------------------------------------------------------------

        // Отключение алгоритма Нейгла. На приёмной стороне он влияет на
        // отправку наших управляющих сообщений: без этого запрос ключевого
        // кадра мог бы пролежать до 200 мс.
        client.tcpNoDelay = true

        // Подсказка стеку: для нас важнее всего низкая задержка.
        // Аргументы: (connectionTime, latency, bandwidth) - относительные веса.
        client.setPerformancePreferences(0, 2, 1)

        client.soTimeout = READ_TIMEOUT_MS

        // Приёмный буфер. Больше, чем буфер чтения: пусть ядро накапливает
        // данные, пока мы разбираем предыдущую порцию.
        client.receiveBufferSize = READ_BUFFER_SIZE * 2

        client.keepAlive = true

        val stream = DataOutputStream(client.getOutputStream())
        outputStream.set(stream)

        // ---------------------------------------------------------------------
        // HELLO - обязательный первый пакет.
        //
        // Стример ждёт именно его и не начнёт кодирование, пока не получит.
        // Отправляем немедленно после установления соединения.
        // ---------------------------------------------------------------------
        val hello = helloProvider()
        try {
            Protocol.sendHello(stream, hello)
            Log.i(TAG, "HELLO отправлен: ${hello.width}x${hello.height}, " +
                    "${hello.refreshMilliHz / 1000.0} Гц, предел декодера " +
                    "${hello.maxDecoderWidth}x${hello.maxDecoderHeight}")
        } catch (e: IOException) {
            Log.e(TAG, "Не удалось отправить HELLO: ${e.message}", e)
            listener.onDisconnected("Не удалось отправить HELLO")
            return
        }

        listener.onConnected(remote)

        // Новый поток - новое состояние разборщика.
        nalParser.reset()

        readLoop(client.getInputStream())

        Log.i(TAG, "Соединение $remote завершено. Принято ${nalParser.totalBytes / 1024} КиБ, " +
                "NAL-юнитов: ${nalParser.totalNalUnits}, отброшено: ${nalParser.droppedNalUnits}")

        outputStream.set(null)
        listener.onDisconnected("Стример отключился")
    }

    /**
     * Цикл чтения видеопотока.
     *
     * Никакой обработки, кроме передачи данных разборщику: чем короче этот
     * цикл, тем быстрее сокет освобождается для следующей порции, и тем меньше
     * шансов, что TCP-окно схлопнется и стример начнёт блокироваться на отправке.
     */
    private fun readLoop(input: InputStream) {
        val buffer = ByteArray(READ_BUFFER_SIZE)

        while (running.get()) {
            try {
                val bytesRead = input.read(buffer)

                if (bytesRead < 0) {
                    Log.i(TAG, "Удалённая сторона закрыла соединение")
                    break
                }
                if (bytesRead == 0) {
                    continue
                }

                // Разборщик сам вызовет onNalUnit для каждого завершённого
                // юнита - возможно, несколько раз за один вызов.
                nalParser.append(buffer, 0, bytesRead)

            } catch (e: java.net.SocketTimeoutException) {
                // Пять секунд тишины. Поток видео мог просто остановиться
                // (статичный рабочий стол), поэтому это не повод рвать
                // соединение - продолжаем ждать.
                Log.d(TAG, "Данных нет ${READ_TIMEOUT_MS} мс - рабочий стол статичен")
                continue
            } catch (e: SocketException) {
                if (running.get()) {
                    Log.w(TAG, "Соединение потеряно: ${e.message}")
                }
                break
            } catch (e: IOException) {
                Log.w(TAG, "Ошибка чтения: ${e.message}")
                break
            }
        }
    }
}
