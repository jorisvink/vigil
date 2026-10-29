package se.sanctorum.vigil

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface
import android.net.Uri
import java.io.ByteArrayOutputStream
import java.io.File

object Images {
    fun jpeg(context: Context, path: String): ByteArray {
        val file = File(path)

        try {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(path, bounds)
            require(bounds.outWidth > 0 && bounds.outHeight > 0) { "That is not an image." }

            var sample = 1
            while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= SIDE) {
                sample *= 2
            }

            val decoded = BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample })
                ?: throw IllegalArgumentException("That is not an image.")
            val scale = minOf(1f, SIDE.toFloat() / maxOf(decoded.width, decoded.height))
            val matrix = Matrix().apply {
                postScale(scale, scale)
                postRotate(rotation(path))
            }

            val bitmap = Bitmap.createBitmap(decoded, 0, 0, decoded.width, decoded.height, matrix, true)
            val out = ByteArrayOutputStream()

            bitmap.compress(Bitmap.CompressFormat.JPEG, QUALITY, out)

            if (bitmap !== decoded) {
                bitmap.recycle()
            }
            decoded.recycle()

            val jpeg = out.toByteArray()
            require(jpeg.size <= MAX) { "That image is too large to send." }

            return jpeg
        } finally {
            if (file.canonicalPath.startsWith(context.cacheDir.canonicalPath)) {
                file.delete()
            }
        }
    }

    fun jpeg(context: Context, uri: Uri): ByteArray {
        val file = File.createTempFile("paste", null, context.cacheDir)

        try {
            val input = context.contentResolver.openInputStream(uri)
                ?: throw IllegalArgumentException("There is no image to paste.")

            input.use { from ->
                file.outputStream().use { to ->
                    require(from.copyTo(to) <= PASTE_MAX) { "That image is too large to send." }
                }
            }
        } catch (e: Exception) {
            file.delete()
            throw e
        }

        return jpeg(context, file.path)
    }

    fun name(): String = "photo-%tY%<tm%<td-%<tH%<tM%<tS.jpg".format(System.currentTimeMillis())

    private fun rotation(path: String): Float =
        when (ExifInterface(path).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)) {
            ExifInterface.ORIENTATION_ROTATE_90 -> 90f
            ExifInterface.ORIENTATION_ROTATE_180 -> 180f
            ExifInterface.ORIENTATION_ROTATE_270 -> 270f
            else -> 0f
        }

    private const val SIDE = 2048
    private const val QUALITY = 85
    private const val MAX = 4 * 1024 * 1024
    private const val PASTE_MAX = 64L * 1024 * 1024
}
