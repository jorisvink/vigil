package se.sanctorum.vigil

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.MediaDataSource
import android.media.MediaPlayer
import android.media.MediaRecorder
import android.os.Build
import android.os.ParcelFileDescriptor
import android.os.PowerManager

object Voice {
    private var recorder: MediaRecorder? = null
    private var reader: Thread? = null
    private var recorded: ByteArray? = null
    private var length = 0
    private var player: MediaPlayer? = null
    private var playing: (() -> Unit)? = null
    private var clip: ByteArray? = null
    private var context: Context? = null
    private var sensor: SensorEventListener? = null
    private var dark: PowerManager.WakeLock? = null
    private var rate = 1f
    private var ear = false

    fun name(): String = "voice-%tY%<tm%<td-%<tH%<tM%<tS.aac".format(System.currentTimeMillis())

    @Synchronized
    fun record(context: Context) {
        require(recorder == null) { "Already recording." }
        require(!Calls.busy) { "Voice messages cannot be recorded during a call." }

        stop()

        val (source, sink) = ParcelFileDescriptor.createPipe()
        val buf = ByteArray(DATA_MAX)
        val rec = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) MediaRecorder(context) else MediaRecorder()

        try {
            rec.setAudioSource(MediaRecorder.AudioSource.MIC)
            rec.setOutputFormat(MediaRecorder.OutputFormat.AAC_ADTS)
            rec.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
            rec.setAudioChannels(1)
            rec.setAudioSamplingRate(SAMPLE_RATE)
            rec.setAudioEncodingBitRate(BIT_RATE)
            rec.setMaxFileSize(DATA_MAX.toLong())
            rec.setOutputFile(sink.fileDescriptor)
            rec.prepare()
            rec.start()
        } catch (e: Exception) {
            rec.release()
            source.close()
            throw IllegalArgumentException("The microphone cannot be used right now.")
        } finally {
            sink.close()
        }

        length = 0
        recorded = buf
        recorder = rec
        reader = Thread {
            var len = 0

            ParcelFileDescriptor.AutoCloseInputStream(source).use { input ->
                while (len < buf.size) {
                    val n = input.read(buf, len, buf.size - len)
                    if (n == -1) {
                        break
                    }

                    len += n
                }
            }

            length = len
        }.also { it.start() }
    }

    @Synchronized
    fun finish(): ByteArray? {
        val rec = recorder ?: return null
        val buf = recorded ?: return null

        recorder = null
        recorded = null

        val ok = try {
            rec.stop()
            true
        } catch (e: RuntimeException) {
            false
        }

        rec.release()
        reader?.join()
        reader = null

        try {
            if (!ok || length == 0) {
                return null
            }

            return buf.copyOf(length)
        } finally {
            buf.fill(0)
        }
    }

    @Synchronized
    fun play(context: Context, data: ByteArray, at: Int, rate: Float, done: () -> Unit) {
        stop()

        if (Calls.busy) {
            data.fill(0)
            throw IllegalArgumentException("Voice messages cannot be played during a call.")
        }

        this.context = context.applicationContext
        this.rate = rate
        clip = data
        ear = false

        try {
            start(at)
        } catch (e: Exception) {
            clip = null
            data.fill(0)
            throw IllegalArgumentException("That voice message cannot be played.")
        }

        playing = done
        listen()
    }

    @Synchronized
    fun seek(at: Int) {
        player?.seekTo(at)
    }

    @Synchronized
    fun speed(rate: Float) {
        this.rate = rate

        val mp = player ?: return
        if (mp.isPlaying) {
            mp.playbackParams = mp.playbackParams.setSpeed(rate)
        }
    }

    @Synchronized
    fun position(): String {
        val mp = player ?: return ""
        return "${mp.currentPosition} ${mp.duration}"
    }

    @Synchronized
    fun stop() {
        player?.release()
        player = null

        sensor?.let { context?.getSystemService(SensorManager::class.java)?.unregisterListener(it) }
        sensor = null

        dark?.let {
            if (it.isHeld) {
                it.release()
            }
        }
        dark = null

        if (ear) {
            ear = false
            route()
        }

        clip?.fill(0)
        clip = null

        playing?.invoke()
        playing = null
    }

    private fun start(at: Int) {
        val data = clip ?: return

        val source = object : MediaDataSource() {
            override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int {
                if (position >= data.size) {
                    return -1
                }

                val n = minOf(size.toLong(), data.size - position).toInt()
                System.arraycopy(data, position.toInt(), buffer, offset, n)

                return n
            }

            override fun getSize(): Long = data.size.toLong()

            override fun close() {
            }
        }

        val mp = MediaPlayer()

        try {
            mp.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(if (ear) AudioAttributes.USAGE_VOICE_COMMUNICATION else AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            mp.setDataSource(source)
            mp.setOnCompletionListener { stop() }
            mp.setOnErrorListener { _, _, _ ->
                stop()
                true
            }
            mp.prepare()
            mp.seekTo(at)
            mp.playbackParams = mp.playbackParams.setSpeed(rate)
            mp.start()
        } catch (e: Exception) {
            mp.release()
            throw e
        }

        player = mp
    }

    private fun listen() {
        val ctx = context ?: return
        val sensors = ctx.getSystemService(SensorManager::class.java)
        val proximity = sensors.getDefaultSensor(Sensor.TYPE_PROXIMITY) ?: return

        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                near(event.values[0] < proximity.maximumRange)
            }

            override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {
            }
        }

        sensors.registerListener(listener, proximity, SensorManager.SENSOR_DELAY_NORMAL)
        sensor = listener

        val power = ctx.getSystemService(PowerManager::class.java)
        if (power.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK)) {
            dark = power.newWakeLock(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK, "vigil:voice").also {
                it.acquire(DARK_MAX)
            }
        }
    }

    @Synchronized
    private fun near(on: Boolean) {
        val mp = player ?: return

        if (on == ear) {
            return
        }

        val at = mp.currentPosition

        mp.setOnCompletionListener(null)
        mp.release()
        player = null

        ear = on
        route()

        try {
            start(at)
        } catch (e: Exception) {
            stop()
        }
    }

    private fun route() {
        val audio = context?.getSystemService(AudioManager::class.java) ?: return

        if (!ear) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                audio.clearCommunicationDevice()
            }

            audio.mode = AudioManager.MODE_NORMAL
            return
        }

        audio.mode = AudioManager.MODE_IN_COMMUNICATION

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            audio.availableCommunicationDevices
                .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE }
                ?.let { audio.setCommunicationDevice(it) }
        } else {
            @Suppress("DEPRECATION")
            audio.isSpeakerphoneOn = false
        }
    }

    private const val DATA_MAX = 4 * 1024 * 1024
    private const val SAMPLE_RATE = 44100
    private const val BIT_RATE = 32000
    private const val DARK_MAX = 10 * 60 * 1000L
}
