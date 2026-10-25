package se.sanctorum.vigil

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyStore
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * The configurations are encrypted with AES-256-GCM under a key that
 * never leaves the Android Keystore. Next to each we keep its summary,
 * which has no keys in it, so we can list tunnels without decrypting.
 */
class TunnelStore(context: Context) {
    private val dir = File(context.noBackupFilesDir, "tunnels").apply { mkdirs() }
    private val prefs = context.getSharedPreferences("vigil", Context.MODE_PRIVATE)

    class Invalid(message: String) : Exception(message)

    fun list(): JSONArray {
        val out = JSONArray()

        dir.listFiles { f -> f.name.endsWith(SUMMARY) }
            ?.sortedByDescending { it.lastModified() }
            ?.forEach { f ->
                out.put(
                    JSONObject()
                        .put("id", f.name.removeSuffix(SUMMARY))
                        .put("summary", parse(f.readText()))
                )
            }

        return out
    }

    fun summary(id: String): JSONObject = parse(file(id, SUMMARY).readText())

    private fun parse(text: String): JSONObject = JSONObject(text).apply {
        if (optString("mode") == "voice") put("mode", "gospel")
    }

    fun voice(): String? {
        val list = list()

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)
            if (entry.getJSONObject("summary").optString("mode") == "gospel") {
                return entry.getString("id")
            }
        }

        return null
    }

    fun check(config: String): String {
        val plain = config.toByteArray(Charsets.UTF_8)

        try {
            return Vigil.check(plain)
        } finally {
            plain.fill(0)
        }
    }

    fun import(config: String): JSONObject {
        val plain = config.toByteArray(Charsets.UTF_8)

        try {
            val summary = JSONObject(Vigil.check(plain))

            if (summary.has("error")) throw Invalid(summary.getString("error"))

            if (summary.optString("mode") == "gospel" && voice() != null) {
                throw Invalid("There is already a Gospel configuration, remove it first.")
            }

            val id = UUID.randomUUID().toString()
            seal(id, plain, summary)

            return JSONObject().put("id", id).put("summary", summary)
        } finally {
            plain.fill(0)
        }
    }

    fun edit(id: String, edits: String): JSONObject {
        val plain = load(id)

        val config = try {
            Vigil.edit(plain, edits)
        } catch (e: IllegalArgumentException) {
            throw Invalid(e.message ?: "invalid configuration")
        } finally {
            plain.fill(0)
        }

        try {
            val summary = JSONObject(Vigil.check(config))

            if (summary.has("error")) throw Invalid(summary.getString("error"))

            val imported = file(id, SUMMARY).lastModified()
            seal(id, config, summary)
            file(id, SUMMARY).setLastModified(imported)

            return JSONObject().put("id", id).put("summary", summary)
        } finally {
            config.fill(0)
        }
    }

    /** The caller must wipe the returned configuration. */
    fun load(id: String): ByteArray {
        val sealed = file(id, CONFIG).readBytes()
        val cipher = Cipher.getInstance(TRANSFORMATION)

        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, sealed, 0, IV_LENGTH))

        return cipher.doFinal(sealed, IV_LENGTH, sealed.size - IV_LENGTH)
    }

    private fun seal(id: String, config: ByteArray, summary: JSONObject) {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, key())

        val sealed = cipher.iv + cipher.doFinal(config)

        file(id, CONFIG).writeBytes(sealed)
        file(id, SUMMARY).writeText(summary.toString())
    }

    fun remove(id: String) {
        val config = file(id, CONFIG)

        if (config.exists()) {
            config.writeBytes(ByteArray(config.length().toInt()))
            config.delete()
        }

        file(id, SUMMARY).delete()

        if (last == id) last = null
    }

    var last: String?
        get() = prefs.getString("last", null)
        set(value) = prefs.edit().putString("last", value).apply()

    fun used(id: String) {
        file(id, SUMMARY).setLastModified(System.currentTimeMillis())
    }

    var gospel: Boolean
        get() = prefs.getBoolean("gospel", true)
        set(value) = prefs.edit().putBoolean("gospel", value).apply()

    private fun file(id: String, suffix: String): File {
        require(id.matches(Regex("[0-9a-f-]{36}"))) { "invalid tunnel id" }
        return File(dir, id + suffix)
    }

    private fun key(): SecretKey {
        val store = KeyStore.getInstance(KEYSTORE).apply { load(null) }

        (store.getEntry(ALIAS, null) as? KeyStore.SecretKeyEntry)?.let {
            return it.secretKey
        }

        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        generator.init(
            KeyGenParameterSpec.Builder(
                ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setKeySize(256)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build()
        )

        return generator.generateKey()
    }

    private companion object {
        const val KEYSTORE = "AndroidKeyStore"
        const val ALIAS = "se.sanctorum.vigil.tunnels"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_LENGTH = 12
        const val CONFIG = ".cfg"
        const val SUMMARY = ".json"
    }
}
