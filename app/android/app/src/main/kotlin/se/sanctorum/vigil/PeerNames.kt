package se.sanctorum.vigil

import android.content.Context
import org.json.JSONObject

object PeerNames {
    fun load(context: Context): JSONObject {
        val all = try {
            JSONObject(prefs(context).getString(KEY, null) ?: "{}")
        } catch (e: Exception) {
            JSONObject()
        }

        val names = JSONObject()

        for (key in all.keys()) {
            if (Flock.valid(key)) {
                names.put(key, all.getString(key))
            }
        }

        return names
    }

    fun save(context: Context, names: JSONObject): JSONObject {
        val clean = JSONObject()

        for (key in names.keys()) {
            val name = names.optString(key).trim().take(MAX)
            if (Flock.valid(key) && name.isNotEmpty()) {
                clean.put(key, name)
            }
        }

        prefs(context).edit().putString(KEY, clean.toString()).apply()
        return clean
    }

    fun name(context: Context, peer: String): String = load(context).optString(peer).ifEmpty { peer }

    fun chat(context: Context, chat: String): String {
        if (ChatVault.group(chat) == null) {
            return name(context, chat)
        }

        val info = try {
            Chats.vault(context).groupInfo(chat)
        } catch (e: Exception) {
            null
        }

        return info?.optString("name")?.ifEmpty { null } ?: "Group"
    }

    fun migrate(context: Context, flock: String) {
        val all = try {
            JSONObject(prefs(context).getString(KEY, null) ?: "{}")
        } catch (e: Exception) {
            JSONObject()
        }
        val moved = JSONObject()

        for (key in all.keys()) {
            moved.put(if (key.matches(Regex("[0-9a-f]{2}"))) "$flock-$key" else key, all.get(key))
        }

        prefs(context).edit().putString(KEY, moved.toString()).apply()
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences("vigil", Context.MODE_PRIVATE)

    private const val KEY = "names"
    const val MAX = 32
}
