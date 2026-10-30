package se.sanctorum.vigil

import android.graphics.BitmapFactory
import android.graphics.Rect
import android.graphics.SurfaceTexture
import android.util.Size
import android.view.Surface
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import org.json.JSONObject
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import zxingcpp.BarcodeReader

object Scanner {
    private var provider: ProcessCameraProvider? = null
    private var analyzer: ExecutorService? = null
    private var run = 0

    @Volatile
    private var found: String? = null

    @Volatile
    private var width = 0

    @Volatile
    private var height = 0

    fun start(activity: MainActivity, texture: SurfaceTexture, done: (Exception?) -> Unit) {
        stop()

        val started = ++run
        val future = ProcessCameraProvider.getInstance(activity)

        future.addListener({
            if (started != run) {
                done(null)
                return@addListener
            }

            try {
                bind(activity, future.get(), texture)
                done(null)
            } catch (e: Exception) {
                stop()
                done(e)
            }
        }, activity.mainExecutor)
    }

    fun state(): JSONObject {
        val value = found
        found = null

        return JSONObject().put("value", value).put("width", width).put("height", height)
    }

    fun stop() {
        run++
        provider?.unbindAll()
        provider = null
        analyzer?.shutdown()
        analyzer = null
        found = null
        width = 0
        height = 0
    }

    fun image(path: String): String? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(path, bounds)

        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > IMAGE_MAX) {
            sample *= 2
        }

        val bitmap = BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })
            ?: throw IllegalArgumentException("That image cannot be read.")

        try {
            return reader(true).read(bitmap, Rect(0, 0, bitmap.width, bitmap.height), 0)
                .firstNotNullOfOrNull { it.text }
        } finally {
            bitmap.recycle()
        }
    }

    private fun bind(activity: MainActivity, cameras: ProcessCameraProvider, texture: SurfaceTexture) {
        val resolution = ResolutionSelector.Builder()
            .setResolutionStrategy(
                ResolutionStrategy(Size(WIDTH, HEIGHT), ResolutionStrategy.FALLBACK_RULE_CLOSEST_LOWER_THEN_HIGHER)
            )
            .build()

        val preview = Preview.Builder().setResolutionSelector(resolution).build()
        val analysis = ImageAnalysis.Builder()
            .setResolutionSelector(resolution)
            .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
            .build()
        val worker = Executors.newSingleThreadExecutor()
        val reader = reader(false)

        preview.setSurfaceProvider(activity.mainExecutor) { request ->
            val size = request.resolution
            val surface = Surface(texture)

            request.setTransformationInfoListener(activity.mainExecutor) {
                val sideways = it.rotationDegrees % 180 != 0

                width = if (sideways) size.height else size.width
                height = if (sideways) size.width else size.height
            }

            texture.setDefaultBufferSize(size.width, size.height)
            request.provideSurface(surface, activity.mainExecutor) { surface.release() }
        }

        analysis.setAnalyzer(worker) { image ->
            image.use {
                if (found == null) {
                    reader.read(it).firstNotNullOfOrNull { result -> result.text }?.let { value -> found = value }
                }
            }
        }

        cameras.unbindAll()
        cameras.bindToLifecycle(activity, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis)

        provider = cameras
        analyzer = worker
    }

    private fun reader(hard: Boolean): BarcodeReader =
        BarcodeReader(
            BarcodeReader.Options().apply {
                formats = setOf(BarcodeReader.Format.QR_CODE)
                tryHarder = hard
                tryRotate = hard
                tryInvert = hard
            }
        )

    private const val WIDTH = 1920
    private const val HEIGHT = 1080
    private const val IMAGE_MAX = 2048
}
