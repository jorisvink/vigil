package se.sanctorum.vigil

import android.content.Context
import org.json.JSONObject

object PeerNames {
    fun load(context: Context): JSONObject =
        try {
            JSONObject(prefs(context).getString(KEY, null) ?: "{}")
        } catch (e: Exception) {
            JSONObject()
        }

    fun save(context: Context, names: JSONObject): JSONObject {
        val clean = JSONObject()

        for (key in names.keys()) {
            val name = names.optString(key).trim().take(MAX)
            if (key.matches(Regex("[0-9a-f]{2}|g[0-9a-f]{4}")) && name.isNotEmpty()) {
                clean.put(key, name)
            }
        }

        prefs(context).edit().putString(KEY, clean.toString()).apply()
        return clean
    }

    fun name(context: Context, peer: Int): String =
        load(context).optString(Calls.hex(peer)).ifEmpty { "Peer ${Calls.hex(peer)}" }

    fun chat(context: Context, chat: String): String {
        val group = ChatVault.group(chat) ?: return name(context, chat.toInt(16))
        return load(context).optString(chat).ifEmpty { "Group %04x".format(group) }
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences("vigil", Context.MODE_PRIVATE)

    private const val KEY = "names"
    const val MAX = 32
}
