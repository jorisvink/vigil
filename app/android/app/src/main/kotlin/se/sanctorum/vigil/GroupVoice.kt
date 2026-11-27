package se.sanctorum.vigil

import android.content.Context
import android.util.Log
import org.json.JSONObject

object GroupVoice {
    @Volatile
    var talking = false
        private set

    @Volatile
    var active = false
        private set

    @Volatile
    var keys = false

    private var sent = ""

    @Synchronized
    fun on(context: Context, chat: String) {
        val vault = Chats.vault(context)

        require(ChatVault.group(chat) != null) { "Voice is only for groups." }
        require(vault.groupInfo(chat)?.optString("state") == "open") { "This group has ended." }

        if (vault.voice().optString("chat") != chat) {
            save(context, JSONObject().put("chat", chat).put("muted", false))
        }
    }

    @Synchronized
    fun off(context: Context) {
        save(context, JSONObject())
    }

    @Synchronized
    fun mute(context: Context, on: Boolean) {
        val state = Chats.vault(context).voice()

        if (state.optString("chat").isNotEmpty()) {
            save(context, state.put("muted", on))
        }
    }

    fun talk(on: Boolean, ready: () -> Unit = {}) {
        talking = on
        ready()
        Vigil.channelTalk(on)
    }

    @Synchronized
    fun json(context: Context): JSONObject {
        val state = Chats.vault(context).voice()

        return JSONObject()
            .put("chat", state.optString("chat"))
            .put("muted", state.optBoolean("muted"))
            .put("status", JSONObject(Vigil.channelStatus()))
    }

    @Synchronized
    fun poll(context: Context) {
        val vault = Chats.vault(context)
        val state = try {
            vault.voice()
        } catch (e: Exception) {
            Log.w(TAG, "failed to read our group voice", e)
            return
        }

        val chat = state.optString("chat")
        val group = ChatVault.group(chat)
        val info = if (group != null) vault.groupInfo(chat) else null
        val core = JSONObject(Vigil.channelStatus())
        val joined = if (core.optBoolean("joined")) core.getString("chat") else ""

        if (group == null || info == null || info.optString("state") != "open" ||
            group.flock !in Chats.locals
        ) {
            active = false

            if (joined.isNotEmpty()) {
                Vigil.channelLeave()
            }

            if (talking) {
                talk(false)
            }

            if (chat.isNotEmpty() && (info == null || info.optString("state") != "open")) {
                vault.voiceSave(JSONObject())
            }

            sent = ""
            return
        }

        active = true

        val roster = ChatVault.roster(ChatVault.members(info) + "%02x".format(group.owner))
        val wanted = chat + roster.joinToString("") { "%02x".format(it) }

        if (joined != chat || wanted != sent) {
            if (joined != chat && talking) {
                talk(false)
            }

            Vigil.channelJoin(Flock.number(chat), group.owner, group.id, roster)
            sent = wanted
        }

        if (core.optBoolean("muted") != state.optBoolean("muted")) {
            Vigil.channelMute(state.optBoolean("muted"))
        }
    }

    private fun save(context: Context, state: JSONObject) {
        Chats.vault(context).voiceSave(state)
        poll(context)
    }

    private const val TAG = "vigil"
}
