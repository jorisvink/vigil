package se.sanctorum.vigil

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import androidx.annotation.Keep
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

object Video {
    private const val TAG = "vigil"
    private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
    private const val WIDTH = 960
    private const val HEIGHT = 540
    private const val WIDTH_MAX = 1280
    private const val HEIGHT_MAX = 720
    private const val FPS = 30
    private const val KEY_INTERVAL = 2
    private const val FRAME_MAX = 1024 * 1024
    private const val FLAG_KEY = 0x01
    private const val ROTATE_SHIFT = 1
    private const val ROTATE_MASK = 0x03
    private const val NAL_SPS = 7
    private const val STALL_MS = 1000L
    private const val STALL_FRAMES = 15
    private const val REPORT_MS = 5000L
    private const val CAMERA_STALL_MS = 3000L
    private const val CAMERA_WATCH_MS = 1000L
    private const val CROP_LEFT = "crop-left"
    private const val CROP_RIGHT = "crop-right"
    private const val CROP_TOP = "crop-top"
    private const val CROP_BOTTOM = "crop-bottom"

    @Volatile
    var context: Context? = null

    @Volatile
    var front = true
        private set

    @Volatile
    var remoteRotation = 0
        private set

    @Volatile
    var remoteWidth = 0
        private set

    @Volatile
    var remoteHeight = 0
        private set

    @Volatile
    var localWidth = WIDTH
        private set

    @Volatile
    var localHeight = HEIGHT
        private set

    @Volatile
    var decoding = false
        private set

    @Volatile
    var capturing = false
        private set

    @Volatile
    private var bitrate = 1_000_000

    @Volatile
    private var produced = 0L

    private var shown = 0L
    private var rendered = 0

    private val lock = Object()
    private val control = Executors.newSingleThreadExecutor()
    private var remote: Surface? = null
    private var local: Surface? = null
    private var localTexture: SurfaceTexture? = null

    private var decoder: Thread? = null

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var encoderSurface: Surface? = null
    private var config: ByteArray? = null
    private var frame: ByteBuffer? = null
    private var rotation = 0
    private var capture = Size(WIDTH, HEIGHT)
    private var fps: Range<Int>? = null
    private var closed: CountDownLatch? = null

    external fun input(buf: ByteBuffer, len: Int, flags: Int): Boolean

    external fun take(buf: ByteBuffer): Long

    external fun camera(on: Boolean)

    external fun refresh()

    @Keep
    @JvmStatic
    fun coreStart(): Boolean {
        synchronized(this) {
            if (decoding) return true

            decoding = true
            decoder = Thread({ decode() }, "vigil-video").apply { start() }
        }

        return true
    }

    @Keep
    @JvmStatic
    fun coreStop() {
        control.submit { close() }.get()

        val running: Thread?

        synchronized(this) {
            decoding = false
            running = decoder
            decoder = null
        }

        running?.join()

        remoteWidth = 0
        remoteHeight = 0
    }

    @Keep
    @JvmStatic
    fun coreCamera(on: Boolean) {
        control.execute {
            if (on) {
                open()
            } else {
                close()
            }
        }
    }

    @Keep
    @JvmStatic
    fun coreKeyframe() {
        handler?.post {
            try {
                encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
            } catch (e: Exception) {
                Log.w(TAG, "keyframe request failed", e)
            }
        }
    }

    @Keep
    @JvmStatic
    fun coreBitrate(bps: Int) {
        bitrate = bps

        handler?.post {
            try {
                encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps) })
            } catch (e: Exception) {
                Log.w(TAG, "bitrate change failed", e)
            }
        }
    }

    fun remoteSurface(surface: Surface?) {
        synchronized(lock) { remote = surface }
    }

    fun localSurface(texture: SurfaceTexture?) {
        synchronized(lock) {
            local?.release()
            localTexture = texture
            local = texture?.let { Surface(it) }
        }

        handler?.post {
            if (device != null) session()
        }
    }

    fun flip() {
        control.execute {
            front = !front

            if (capturing) {
                close()
                open()
            }
        }
    }

    private fun open() {
        val context = context ?: return

        synchronized(this) {
            if (capturing) return
            capturing = true

            thread = HandlerThread("vigil-camera").apply { start() }
            handler = Handler(thread!!.looper)
        }

        produced = System.currentTimeMillis()
        handler?.let { watch(it) }

        handler?.post {
            try {
                camera(context)
            } catch (e: Exception) {
                Log.w(TAG, "camera failed", e)
                closed?.countDown()
                release()
            }
        }
    }

    private fun watch(handler: Handler) {
        handler.postDelayed({
            val current = synchronized(this) { this.handler }

            if (current === handler) {
                if (System.currentTimeMillis() - produced < CAMERA_STALL_MS) {
                    watch(handler)
                } else {
                    Log.w(TAG, "camera stalled, reopening")

                    control.execute {
                        if (capturing) {
                            close()
                            open()
                        }
                    }
                }
            }
        }, CAMERA_WATCH_MS)
    }

    private fun close() {
        val handler: Handler
        val thread: HandlerThread

        synchronized(this) {
            if (!capturing) return
            capturing = false

            handler = this.handler ?: return
            thread = this.thread ?: return
            this.handler = null
            this.thread = null
        }

        val done = CountDownLatch(1)
        var closing: CountDownLatch? = null

        handler.post {
            closing = closed
            release()
            done.countDown()
        }

        if (!done.await(2, TimeUnit.SECONDS)) Log.w(TAG, "camera did not close in time")
        if (closing?.await(2, TimeUnit.SECONDS) == false) Log.w(TAG, "camera device did not close in time")

        thread.quitSafely()
        thread.join()
    }

    private fun camera(context: Context) {
        val manager = context.getSystemService(CameraManager::class.java)
        val id = pick(manager) ?: throw IllegalStateException("no camera")
        val characteristics = manager.getCameraCharacteristics(id)
        val size = size(characteristics)

        rotation = ((characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0) / 90) and ROTATE_MASK
        capture = size

        if (rotation and 1 == 1) {
            localWidth = size.height
            localHeight = size.width
        } else {
            localWidth = size.width
            localHeight = size.height
        }

        fps = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.filter { it.upper == FPS }
            ?.maxByOrNull { it.lower }

        encode(size)

        val latch = CountDownLatch(1)
        closed = latch

        manager.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                if (closed !== latch) {
                    camera.close()
                    return
                }

                device = camera
                session()
            }

            override fun onClosed(camera: CameraDevice) {
                latch.countDown()
            }

            override fun onDisconnected(camera: CameraDevice) {
                camera.close()
                if (device === camera) device = null
            }

            override fun onError(camera: CameraDevice, error: Int) {
                Log.w(TAG, "camera error $error")
                camera.close()
                if (device === camera) device = null
            }
        }, handler)
    }

    private fun pick(manager: CameraManager): String? {
        val facing = if (front) CameraMetadata.LENS_FACING_FRONT else CameraMetadata.LENS_FACING_BACK

        return manager.cameraIdList.firstOrNull {
            manager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == facing
        } ?: manager.cameraIdList.firstOrNull()
    }

    private fun size(characteristics: CameraCharacteristics): Size {
        val sizes = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(MediaCodec::class.java)
            ?: return Size(WIDTH, HEIGHT)

        val wide = sizes.filter { it.width * 9 == it.height * 16 }

        return wide.filter { it.width <= WIDTH && it.height <= HEIGHT }.maxByOrNull { it.width * it.height }
            ?: wide.filter { it.width <= WIDTH_MAX && it.height <= HEIGHT_MAX }.minByOrNull { it.width * it.height }
            ?: sizes.filter { it.width <= WIDTH_MAX && it.height <= HEIGHT_MAX }.maxByOrNull { it.width * it.height }
            ?: Size(WIDTH, HEIGHT)
    }

    private fun encode(size: Size) {
        val format = MediaFormat.createVideoFormat(MIME, size.width, size.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, KEY_INTERVAL)
        }

        config = null
        frame = frame ?: ByteBuffer.allocateDirect(FRAME_MAX)

        val codec = MediaCodec.createEncoderByType(MIME)

        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
            }

            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                try {
                    encoded(codec, index, info)
                } finally {
                    codec.releaseOutputBuffer(index, false)
                }
            }

            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                Log.w(TAG, "encoder error", e)
            }

            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                val sps = format.getByteBuffer("csd-0") ?: return
                val pps = format.getByteBuffer("csd-1") ?: return
                val csd = ByteArray(sps.remaining() + pps.remaining())

                sps.get(csd, 0, sps.remaining())
                pps.get(csd, csd.size - pps.remaining(), pps.remaining())
                config = csd
            }
        }, handler)

        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        encoderSurface = codec.createInputSurface()
        codec.start()

        encoder = codec
    }

    private fun encoded(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
        val buffer = codec.getOutputBuffer(index) ?: return
        val out = frame ?: return

        buffer.position(info.offset)
        buffer.limit(info.offset + info.size)

        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
            config = ByteArray(info.size).also { buffer.get(it) }
            return
        }

        if (info.size == 0) return

        val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
        val prefix = if (key) config else null
        val total = (prefix?.size ?: 0) + info.size

        if (total > FRAME_MAX) return

        out.clear()
        if (prefix != null) out.put(prefix)
        out.put(buffer)

        var flags = (rotation and ROTATE_MASK) shl ROTATE_SHIFT
        if (key) flags = flags or FLAG_KEY

        produced = System.currentTimeMillis()
        input(out, total, flags)
    }

    private fun session() {
        val camera = device ?: return
        val encoded = encoderSurface ?: return
        val preview = synchronized(lock) {
            localTexture?.setDefaultBufferSize(capture.width, capture.height)
            local
        }
        val targets = listOfNotNull(encoded, preview)

        session?.close()
        session = null

        @Suppress("DEPRECATION")
        camera.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(configured: CameraCaptureSession) {
                if (device !== camera) {
                    configured.close()
                    return
                }

                session = configured

                val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                    targets.forEach { addTarget(it) }
                    fps?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
                }

                try {
                    configured.setRepeatingRequest(request.build(), null, handler)
                } catch (e: Exception) {
                    Log.w(TAG, "camera request failed", e)
                }
            }

            override fun onConfigureFailed(configured: CameraCaptureSession) {
                Log.w(TAG, "camera session failed")
            }
        }, handler)
    }

    private fun release() {
        try {
            session?.close()
        } catch (e: Exception) {
            Log.w(TAG, "camera session close failed", e)
        }

        session = null
        closed = null

        device?.close()
        device = null

        try {
            encoder?.stop()
        } catch (e: Exception) {
            Log.w(TAG, "encoder stop failed", e)
        }

        encoder?.release()
        encoder = null

        encoderSurface?.release()
        encoderSurface = null
    }

    private fun decode() {
        val buffer = ByteBuffer.allocateDirect(FRAME_MAX)
        val info = MediaCodec.BufferInfo()
        var codec: MediaCodec? = null
        var surface: Surface? = null
        var waiting = true
        var pts = 0L
        var sps: ByteArray? = null
        val rotations = HashMap<Long, Int>()
        var taken = 0
        var fed = 0
        var report = System.currentTimeMillis() + REPORT_MS

        while (decoding) {
            if (System.currentTimeMillis() >= report) {
                Log.i(TAG, "decoder took $taken fed $fed showed $rendered surface ${surface != null} codec ${codec != null} waiting $waiting")
                report = System.currentTimeMillis() + REPORT_MS
                taken = 0
                fed = 0
                rendered = 0
            }

            val wanted = synchronized(lock) { remote }

            if (wanted !== surface) {
                codec = stop(codec)
                rotations.clear()
                surface = wanted
                waiting = true
                if (surface != null) refresh()
            }

            val next = take(buffer)

            if (next == 0L) {
                if (codec != null) drain(codec, info, rotations)
                Thread.sleep(5)
                continue
            }

            taken++

            val len = (next and 0xffffffffL).toInt()
            val flags = (next ushr 32).toInt()
            val key = flags and FLAG_KEY != 0

            if (surface == null) continue
            if (waiting && !key) continue

            if (key) {
                val changed = sps(buffer, len)

                if (codec != null && changed != null && !changed.contentEquals(sps)) {
                    Log.i(TAG, "decoder stream changed, starting over")
                    codec = stop(codec)
                    rotations.clear()
                }

                if (changed != null) sps = changed
            }

            try {
                if (codec == null) {
                    codec = MediaCodec.createDecoderByType(MIME).apply {
                        val format = MediaFormat.createVideoFormat(MIME, WIDTH_MAX, HEIGHT_MAX).apply {
                            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, FRAME_MAX)
                        }

                        configure(format, surface, null, 0)
                        start()
                    }

                    shown = System.currentTimeMillis()
                }

                waiting = false

                val asked = System.currentTimeMillis()
                var index = codec.dequeueInputBuffer(10_000)

                while (index < 0 && decoding && System.currentTimeMillis() - asked < STALL_MS) {
                    drain(codec, info, rotations)
                    index = codec.dequeueInputBuffer(10_000)
                }

                if (index < 0) {
                    Log.w(TAG, "decoder stalled, starting over")
                    codec = stop(codec)
                    rotations.clear()
                    waiting = true
                    refresh()
                    continue
                }

                val input = codec.getInputBuffer(index) ?: throw IllegalStateException("no decoder input")

                buffer.position(0)
                buffer.limit(len)
                input.clear()
                input.put(buffer)
                buffer.clear()

                rotations[pts] = (flags shr ROTATE_SHIFT) and ROTATE_MASK
                codec.queueInputBuffer(index, 0, len, pts, 0)
                pts += 1_000_000L / FPS
                fed++

                drain(codec, info, rotations)

                if (rotations.size >= STALL_FRAMES && System.currentTimeMillis() - shown >= STALL_MS) {
                    Log.w(TAG, "decoder shows nothing for ${rotations.size} frames, starting over")
                    codec = stop(codec)
                    rotations.clear()
                    waiting = true
                    refresh()
                }
            } catch (e: Exception) {
                Log.w(TAG, "decoder failed, starting over", e)
                codec = stop(codec)
                rotations.clear()
                waiting = true
                refresh()
            }
        }

        stop(codec)
    }

    private fun sps(buffer: ByteBuffer, len: Int): ByteArray? {
        var start = -1
        var idx = 0

        while (idx + 3 <= len) {
            if (buffer.get(idx) != 0.toByte() || buffer.get(idx + 1) != 0.toByte() || buffer.get(idx + 2) != 1.toByte()) {
                idx++
                continue
            }

            if (start != -1) {
                var end = idx
                if (end > start && buffer.get(end - 1) == 0.toByte()) end--
                return ByteArray(end - start) { buffer.get(start + it) }
            }

            idx += 3

            if (idx < len && (buffer.get(idx).toInt() and 0x1f) == NAL_SPS) start = idx
        }

        if (start == -1) return null

        return ByteArray(len - start) { buffer.get(start + it) }
    }

    private fun drain(codec: MediaCodec, info: MediaCodec.BufferInfo, rotations: HashMap<Long, Int>) {
        while (true) {
            val index = codec.dequeueOutputBuffer(info, 0)

            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                val format = codec.outputFormat

                if (format.containsKey(CROP_RIGHT) && format.containsKey(CROP_BOTTOM)) {
                    remoteWidth = format.getInteger(CROP_RIGHT) - format.getInteger(CROP_LEFT) + 1
                    remoteHeight = format.getInteger(CROP_BOTTOM) - format.getInteger(CROP_TOP) + 1
                } else {
                    remoteWidth = format.getInteger(MediaFormat.KEY_WIDTH)
                    remoteHeight = format.getInteger(MediaFormat.KEY_HEIGHT)
                }
                continue
            }

            if (index < 0) return

            rotations.remove(info.presentationTimeUs)?.let { remoteRotation = it }
            codec.releaseOutputBuffer(index, true)
            shown = System.currentTimeMillis()
            rendered++
        }
    }

    private fun stop(codec: MediaCodec?): MediaCodec? {
        if (codec == null) return null

        try {
            codec.stop()
        } catch (e: Exception) {
            Log.w(TAG, "decoder stop failed", e)
        }

        codec.release()

        return null
    }
}
