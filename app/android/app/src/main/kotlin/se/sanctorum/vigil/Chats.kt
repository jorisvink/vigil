package se.sanctorum.vigil

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

object Chats {
    private data class Receipt(
        val chat: String,
        val ref: Long,
        val kind: String,
        val acked: Int = 0,
        val pending: Int = 0
    )

    private var vault: ChatVault? = null
    private var session = 0L
    private var resubmit = 0L
    private var screen: BroadcastReceiver? = null
    private data class Shown(val text: String, val time: Long, val name: String, val key: String)

    private val shown = mutableMapOf<String, MutableList<Shown>>()
    private val unseen = mutableMapOf<String, Int>()
    private val receipts = mutableListOf<Receipt>()
    private val waiting = mutableSetOf<Long>()

    var version = 0L
        private set
    var foreground = false
    var opened = ""
        private set
    var requested = ""

    fun vault(context: Context): ChatVault =
        vault ?: ChatVault(context.applicationContext).also { vault = it }

    fun attach(service: VigilVpnService) {
        service.getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Messages", NotificationManager.IMPORTANCE_HIGH)
        )

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                vault?.lock()
            }
        }

        service.registerReceiver(receiver, IntentFilter(Intent.ACTION_SCREEN_OFF))
        screen = receiver
        session = 0
        resubmit = 0
        waiting.clear()
    }

    fun detach(service: VigilVpnService) {
        screen?.let { service.unregisterReceiver(it) }
        screen = null
    }

    fun poll(service: VigilVpnService, status: JSONObject?) {
        val chat = status?.optJSONObject("chat") ?: return
        val vault = vault(service)

        if (chat.optLong("session") != session) {
            resubmit = 0L
            waiting.clear()

            try {
                Vigil.chatNick(nick(service))
                Vigil.chatGroup(group(service))
                handover(vault, null)
                session = chat.optLong("session")
            } catch (e: Exception) {
                Log.w(TAG, "failed to hand over the outbox", e)
            }
        } else if (resubmit != 0L && System.currentTimeMillis() >= resubmit) {
            resubmit = 0L

            try {
                handover(vault, waiting.toSet())
            } catch (e: Exception) {
                Log.w(TAG, "failed to hand over the refused data", e)
                refused(null)
            }
        }

        val pending = receipts.toList()
        receipts.clear()

        for (receipt in pending) {
            receipt(vault, receipt)
        }

        val events = JSONArray(String(Vigil.chatEvents(), Charsets.UTF_8))

        for (i in 0 until events.length()) {
            val event = events.getJSONObject(i)
            val peer = event.getString("peer").toInt(16)
            val id = event.optString("group").ifEmpty { null }
                ?.let { ChatVault.groupChat(it.toInt(16)) } ?: Calls.hex(peer)

            try {
                when (event.getString("type")) {
                    "message" -> {
                        val text = event.getString("text")
                        val time = System.currentTimeMillis()

                        val nick = event.optString("nick")

                        vault.deliver(id, peer, nick, text, time)
                        Vigil.chatStored(event.getLong("token"))

                        if (!foreground || opened != id) {
                            unseen[id] = (unseen[id] ?: 0) + 1
                            notify(service, id, peer, nick, text, time)
                        }
                    }
                    "data" -> {
                        val time = System.currentTimeMillis()
                        val nick = event.optString("nick")
                        val name = event.getString("name")
                        val media = event.getString("kind")
                        val token = event.getLong("token")
                        val data = Vigil.chatDataTake(token, event.getInt("size"))
                            ?: throw IllegalStateException("data $token is gone")

                        try {
                            vault.deliver(id, peer, nick, "", time, data = data, media = media, name = name)
                        } finally {
                            data.fill(0)
                        }

                        Vigil.chatStored(token)

                        if (!foreground || opened != id) {
                            unseen[id] = (unseen[id] ?: 0) + 1
                            notify(service, id, peer, nick, summary(media, name), time)
                        }
                    }
                    "joined", "left" -> vault.deliver(
                        id, peer, event.optString("nick"), "", System.currentTimeMillis(),
                        event.getString("type")
                    )
                    "sent", "delivered" ->
                        receipt(vault, Receipt(id, event.getLong("ref"), event.getString("type")))
                    "acks" -> receipt(
                        vault,
                        Receipt(
                            id, event.getLong("ref"), "acks",
                            event.getInt("acked"), event.getInt("pending")
                        )
                    )
                }
                version++
            } catch (e: Exception) {
                Log.e(TAG, "failed to store a chat event", e)
            }
        }
    }

    fun refused(ref: Long?) {
        ref?.let { waiting.add(it) }
        if (resubmit == 0L) {
            resubmit = System.currentTimeMillis() + RESUBMIT_MS
        }
    }

    private fun handover(vault: ChatVault, only: Set<Long>?) {
        val outbox = vault.outboxList()
        val refs = mutableSetOf<Long>()

        for (i in 0 until outbox.length()) {
            val entry = outbox.getJSONObject(i)
            val chat = entry.getString("peer")
            val ref = entry.getLong("ref")

            refs.add(ref)

            if (only != null && ref !in only) {
                continue
            }

            if (!entry.has("data")) {
                Vigil.chatSend(chat.toInt(16), ref, entry.getString("text").toByteArray())
                continue
            }

            val data = vault.data(chat, entry.getString("data")) ?: continue

            try {
                val sent = Vigil.chatDataSend(
                    chat.toInt(16), ref, ChatVault.kind(entry.getString("media")),
                    entry.getString("name").toByteArray(), data
                )

                if (sent) {
                    waiting.remove(ref)
                } else {
                    refused(ref)
                }
            } finally {
                data.fill(0)
            }
        }

        waiting.retainAll(refs)
    }

    private fun receipt(vault: ChatVault, receipt: Receipt) {
        try {
            vault.receipt(receipt.chat, receipt.ref, receipt.kind, receipt.acked, receipt.pending)
            version++
        } catch (e: Exception) {
            Log.w(TAG, "failed to store a receipt, trying again", e)
            receipts.add(receipt)
        }
    }

    fun open(context: Context, id: String, on: Boolean) {
        peer(opened)?.let { if (opened != id) Vigil.chatOpen(it, false) }

        opened = if (on) id else ""

        peer(id)?.let { Vigil.chatOpen(it, on) }
        if (on) {
            seen(context, id)
        }
    }

    fun typing(id: String, on: Boolean) {
        if (ChatVault.group(id) != null) {
            Vigil.chatGroupTyping(on)
        } else {
            peer(id)?.let { Vigil.chatTyping(it, on) }
        }
    }

    fun seen(context: Context, id: String) {
        unseen.remove(id)
        dismiss(context, id)
    }

    fun background(context: Context) {
        foreground = false

        if (opened != "") {
            peer(opened)?.let { Vigil.chatOpen(it, false) }
            typing(opened, false)
            opened = ""
        }

        vault?.lock()
    }

    fun json(context: Context): JSONObject {
        val pending = requested
        requested = ""

        val group = group(context)

        return JSONObject()
            .put("version", version)
            .put("unseen", unseen.values.sum())
            .put("hardware", vault(context).hardware)
            .put("open", pending.ifEmpty { JSONObject.NULL })
            .put("group", if (group != 0) ChatVault.groupChat(group) else JSONObject.NULL)
    }

    fun changed() {
        version++
    }

    fun nick(context: Context): String = prefs(context).getString("nick", "") ?: ""

    fun preview(context: Context): Boolean = prefs(context).getBoolean("preview", false)

    fun group(context: Context): Int = prefs(context).getInt("group", 0)

    fun join(context: Context, group: Int) {
        require(group in 0..0xffff) { "A group is 1 to 4 hex digits." }

        prefs(context).edit().putInt("group", group).apply()
        Vigil.chatGroup(group)
        changed()
    }

    fun settings(context: Context, nick: String?, preview: Boolean?): JSONObject {
        if (nick != null) {
            if (!Vigil.chatNick(nick)) {
                throw IllegalArgumentException("A name is 2 to 16 letters or digits.")
            }
            prefs(context).edit().putString("nick", nick).apply()
        }

        if (preview != null) {
            prefs(context).edit().putBoolean("preview", preview).apply()
        }

        return JSONObject().put("nick", nick(context)).put("preview", preview(context))
    }

    private fun peer(id: String): Int? =
        if (id.length == 2) id.toInt(16).takeIf { it in 1..0xfe } else null

    private fun summary(media: String, name: String): String =
        when (media) {
            "image" -> "Image"
            "voice" -> "Voice message"
            else -> "File: $name"
        }

    private fun notify(
        service: VigilVpnService, id: String, peer: Int, nick: String, text: String, time: Long
    ) {
        val named = PeerNames.load(service).has(Calls.hex(peer))
        val name = if (named || nick.isEmpty()) Calls.name(peer) else nick
        val messages = shown.getOrPut(id) { mutableListOf() }

        messages.add(Shown(if (preview(service)) text else "New message", time, name, Calls.hex(peer)))
        while (messages.size > SHOWN_MAX) {
            messages.removeAt(0)
        }

        val group = ChatVault.group(id) != null
        val me = Person.Builder().setName("You").build()
        val style = Notification.MessagingStyle(me)

        if (group) {
            style.setConversationTitle(PeerNames.chat(service, id)).setGroupConversation(true)
        }

        for (message in messages) {
            val sender = Person.Builder().setName(message.name).setKey(message.key).build()
            style.addMessage(message.text, message.time, sender)
        }

        val builder = Notification.Builder(service, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setStyle(style)
            .setNumber(unseen[id] ?: 1)
            .setGroup(GROUP)
            .setAutoCancel(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)
            .setPublicVersion(
                Notification.Builder(service, CHANNEL)
                    .setSmallIcon(R.drawable.ic_notification)
                    .setContentTitle("Vigil")
                    .setContentText("New message")
                    .build()
            )

        launch(service, notification(id), id)?.let { builder.setContentIntent(it) }

        service.getSystemService(NotificationManager::class.java)
            .notify(notification(id), builder.build())

        summary(service)
    }

    private fun summary(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java)

        if (shown.isEmpty()) {
            manager.cancel(SUMMARY)
            return
        }

        val style = Notification.InboxStyle()
        for ((id, messages) in shown) {
            style.addLine(if (ChatVault.group(id) != null) PeerNames.chat(context, id) else messages.last().name)
        }

        val builder = Notification.Builder(context, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setStyle(style)
            .setContentTitle("Vigil")
            .setContentText("New messages")
            .setGroup(GROUP)
            .setGroupSummary(true)
            .setGroupAlertBehavior(Notification.GROUP_ALERT_CHILDREN)
            .setAutoCancel(true)
            .setVisibility(Notification.VISIBILITY_PRIVATE)

        launch(context, SUMMARY, shown.keys.singleOrNull() ?: CHAT_LIST)?.let { builder.setContentIntent(it) }

        manager.notify(SUMMARY, builder.build())
    }

    private fun launch(context: Context, code: Int, id: String): PendingIntent? {
        val intent = context.packageManager.getLaunchIntentForPackage(context.packageName) ?: return null

        return PendingIntent.getActivity(
            context, code,
            intent.setAction(ACTION_CHAT)
                .putExtra(EXTRA_CHAT, id)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
    }

    private fun dismiss(context: Context, id: String) {
        shown.remove(id)
        context.getSystemService(NotificationManager::class.java).cancel(notification(id))
        summary(context)
    }

    private fun notification(id: String): Int =
        ChatVault.group(id)?.let { GROUP_NOTIFICATION_BASE + it } ?: (NOTIFICATION_BASE + id.toInt(16))

    private fun prefs(context: Context) =
        context.getSharedPreferences("chats", Context.MODE_PRIVATE)

    const val ACTION_CHAT = "se.sanctorum.vigil.CHAT"
    const val EXTRA_CHAT = "chat"
    const val CHAT_LIST = "gospel"

    private const val TAG = "vigil"
    private const val CHANNEL = "messages"
    private const val NOTIFICATION_BASE = 0x1000
    private const val GROUP_NOTIFICATION_BASE = 0x10000
    private const val SHOWN_MAX = 6
    private const val SUMMARY = 0x0fff
    private const val GROUP = "se.sanctorum.vigil.chats"
    private const val RESUBMIT_MS = 10_000L
}
