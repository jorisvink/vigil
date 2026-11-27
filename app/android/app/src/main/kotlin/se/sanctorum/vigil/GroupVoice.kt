package se.sanctorum.vigil

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
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

    private var routed = false

    private var earpiece = false

    private var headset = false

    @Synchronized
    fun on(context: Context, chat: String) {
        val vault = Chats.vault(context)

        require(ChatVault.group(chat) != null) { "Voice is only for groups." }
        require(vault.groupInfo(chat)?.optString("state") == "open") { "This group has ended." }

        val state = vault.voice()

        if (state.optString("chat") != chat) {
            save(context, kept(state).put("chat", chat).put("muted", false))
        }
    }

    @Synchronized
    fun off(context: Context) {
        save(context, kept(Chats.vault(context).voice()))
    }

    @Synchronized
    fun earpiece(context: Context, on: Boolean) {
        save(context, Chats.vault(context).voice().put("earpiece", on))
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
            .put("earpiece", state.optBoolean("earpiece"))
            .put("output", if (headset) "headset" else if (state.optBoolean("earpiece")) "earpiece" else "speaker")
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

        earpiece = state.optBoolean("earpiece")

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
                vault.voiceSave(kept(state))
            }

            sent = ""
            route(context)
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

        route(context)
    }

    @Suppress("DEPRECATION")
    private fun route(context: Context) {
        val audio = context.getSystemService(AudioManager::class.java) ?: return

        headset = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any { it.type in HEADSETS }

        if (active && !headset && !Calls.busy && !Voice.earpiece) {
            val want = if (earpiece) AudioDeviceInfo.TYPE_BUILTIN_EARPIECE else AudioDeviceInfo.TYPE_BUILTIN_SPEAKER

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (audio.communicationDevice?.type != want) {
                    audio.availableCommunicationDevices
                        .firstOrNull { it.type == want }
                        ?.let { audio.setCommunicationDevice(it) }
                }
            } else if (!routed || audio.isSpeakerphoneOn == earpiece) {
                audio.mode = AudioManager.MODE_IN_COMMUNICATION
                audio.isSpeakerphoneOn = !earpiece
            }

            routed = true
            return
        }

        if (!routed) {
            return
        }

        routed = false

        if (Calls.busy || Voice.earpiece) {
            return
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            audio.clearCommunicationDevice()
        } else {
            audio.isSpeakerphoneOn = false
            audio.mode = AudioManager.MODE_NORMAL
        }
    }

    private fun kept(state: JSONObject): JSONObject =
        JSONObject().put("earpiece", state.optBoolean("earpiece"))

    private fun save(context: Context, state: JSONObject) {
        Chats.vault(context).voiceSave(state)
        poll(context)
    }

    private val HEADSETS = setOf(
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_HEADSET,
    )

    private const val TAG = "vigil"
}
