package se.sanctorum.vigil

import android.content.Context
import android.net.Uri
import android.webkit.MimeTypeMap
import java.io.File
import java.io.FileOutputStream
import java.security.SecureRandom

object Files {
    const val AUTHORITY = "se.sanctorum.vigil.open"

    fun name(name: String): String {
        val clean = name.substringAfterLast('/').filter { it >= ' ' && it != '\u007f' && it != '\\' }
            .trimStart('.').ifEmpty { "file" }
        if (clean.toByteArray().size <= NAME_MAX) return clean

        val ext = clean.substringAfterLast('.', "").take(16)
        var stem = clean.removeSuffix(".$ext")

        while ("$stem.$ext".toByteArray().size > NAME_MAX) stem = stem.dropLast(1)

        return if (ext.isEmpty()) stem else "$stem.$ext"
    }

    fun type(name: String): String =
        MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase())
            ?: "application/octet-stream"

    fun share(context: Context, name: String, data: ByteArray): Uri {
        wipe(context)

        val id = ByteArray(16).also(SecureRandom()::nextBytes).joinToString("") { "%02x".format(it) }

        FileOutputStream(File(folder(context), id)).use {
            it.write(data)
            it.fd.sync()
        }

        return Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(id).appendPath(name).build()
    }

    fun file(context: Context, uri: Uri): File? {
        val id = uri.pathSegments.firstOrNull() ?: return null
        if (!id.matches(Regex("[0-9a-f]{32}"))) return null

        return File(folder(context), id).takeIf { it.exists() }
    }

    fun wipe(context: Context) {
        for (file in folder(context).listFiles() ?: emptyArray()) {
            FileOutputStream(file).use { it.write(ByteArray(file.length().toInt())) }
            file.delete()
        }
    }

    private fun folder(context: Context): File = File(context.cacheDir, "open").apply { mkdirs() }

    private const val NAME_MAX = 126
}
