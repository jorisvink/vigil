package se.sanctorum.vigil

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaDataSource
import android.media.MediaPlayer
import android.media.MediaRecorder
import android.os.Build
import android.os.ParcelFileDescriptor

object Voice {
    private var recorder: MediaRecorder? = null
    private var reader: Thread? = null
    private var recorded: ByteArray? = null
    private var length = 0
    private var player: MediaPlayer? = null
    private var playing: (() -> Unit)? = null

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
    fun play(data: ByteArray, done: () -> Unit) {
        stop()

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
                data.fill(0)
            }
        }

        val mp = MediaPlayer()

        try {
            mp.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
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
            mp.start()
        } catch (e: Exception) {
            mp.release()
            data.fill(0)
            throw IllegalArgumentException("That voice message cannot be played.")
        }

        player = mp
        playing = done
    }

    @Synchronized
    fun stop() {
        player?.release()
        player = null

        playing?.invoke()
        playing = null
    }

    private const val DATA_MAX = 4 * 1024 * 1024
    private const val SAMPLE_RATE = 44100
    private const val BIT_RATE = 32000
}
