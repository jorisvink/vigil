package se.sanctorum.vigil

import android.content.ContentProvider
import android.content.ContentValues
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock

class HandoffProvider : ContentProvider() {
    override fun onCreate(): Boolean = true

    override fun call(method: String, arg: String?, extras: Bundle?): Bundle? {
        val context = context ?: return null

        if (context.checkCallingPermission(PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            throw SecurityException("not allowed to hand over configurations")
        }

        if (method != METHOD) {
            return null
        }

        val config = extras?.getByteArray(EXTRA) ?: return null
        Handoff.put(config)
        config.fill(0)

        return Bundle().apply { putBoolean("ok", true) }
    }

    override fun getType(uri: Uri): String? = null

    override fun query(
        uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?,
        sortOrder: String?
    ): Cursor? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun update(
        uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?
    ): Int = 0

    private companion object {
        const val PERMISSION = "se.sanctorum.vigil.permission.IMPORT"
        const val METHOD = "import"
        const val EXTRA = "config"
    }
}

object Handoff {
    private const val KEEP_MS = 5 * 60 * 1000L

    private var config: ByteArray? = null
    private var since = 0L

    @Synchronized
    fun put(data: ByteArray) {
        config?.fill(0)
        config = data.copyOf()
        since = SystemClock.elapsedRealtime()
    }

    /** The caller must wipe the returned configuration. */
    @Synchronized
    fun take(): ByteArray? {
        val data = config ?: return null
        config = null

        if (SystemClock.elapsedRealtime() - since > KEEP_MS) {
            data.fill(0)
            return null
        }

        return data
    }
}
