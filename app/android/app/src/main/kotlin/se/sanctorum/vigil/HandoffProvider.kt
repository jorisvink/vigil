package se.sanctorum.vigil

import android.content.ContentProvider
import android.content.ContentValues
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import java.security.SecureRandom

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
    private const val GONE = "The handoff expired, hand the device over again"
    private val PREFIX = "handoff ".toByteArray()

    private var config: ByteArray? = null
    private var issued: ByteArray? = null
    private var since = 0L

    @Synchronized
    fun put(data: ByteArray) {
        clear()
        config = data.copyOf()
        since = SystemClock.elapsedRealtime()
    }

    @Synchronized
    fun token(): ByteArray? {
        if (config == null) {
            return null
        }

        if (issued != null) {
            return null
        }

        if (expired()) {
            clear()
            return null
        }

        val random = ByteArray(16)
        SecureRandom().nextBytes(random)

        val token = PREFIX + random.joinToString("") { "%02x".format(it) }.toByteArray()
        issued = token

        return token.copyOf()
    }

    @Synchronized
    fun <T> held(token: ByteArray, work: (ByteArray) -> T): T? {
        if (!token.copyOfRange(0, minOf(token.size, PREFIX.size)).contentEquals(PREFIX)) {
            return null
        }

        if (expired()) {
            clear()
        }

        val data = config ?: throw TunnelStore.Invalid(GONE)
        val ours = issued ?: throw TunnelStore.Invalid(GONE)

        if (!ours.contentEquals(token)) {
            throw TunnelStore.Invalid(GONE)
        }

        return work(data)
    }

    @Synchronized
    fun clear() {
        config?.fill(0)
        config = null
        issued = null
    }

    private fun expired(): Boolean = SystemClock.elapsedRealtime() - since > KEEP_MS
}
