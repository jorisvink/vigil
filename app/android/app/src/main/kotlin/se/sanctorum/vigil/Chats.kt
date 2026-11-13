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
import android.os.SystemClock
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

object Chats {
    private data class Receipt(val peer: String, val ref: Long, val kind: String)

    private var vault: ChatVault? = null
    private var session = 0L
    private var resubmit = 0L
    private var sweep = 0L
    private var screen: BroadcastReceiver? = null
    private data class Shown(val text: String, val time: Long, val name: String, val key: String)

    private val shown = mutableMapOf<String, MutableList<Shown>>()
    private val unseen = mutableMapOf<String, Int>()
    private val receipts = mutableListOf<Receipt>()
    private val waiting = mutableSetOf<Long>()
    private val around = mutableSetOf<String>()

    @Volatile
    var version = 0L
        private set

    @Volatile
    var avatars = 0L
        private set

    @Volatile
    var names = 0L
        private set
    var foreground = false
    var opened = ""
        private set
    var requested = ""
    var requestedTopic = ""

    @Volatile
    var local = ""
        private set

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

        local = chat.optString("id")

        if (chat.optLong("session") != session) {
            resubmit = 0L
            waiting.clear()

            try {
                if (!Vigil.chatNick(nick(service))) {
                    Log.w(TAG, "our nickname was refused")
                }

                val avatar = vault.ownAvatar()

                try {
                    if (!Vigil.chatAvatar(avatar)) {
                        Log.w(TAG, "our avatar was refused")
                    }
                } finally {
                    avatar?.fill(0)
                }

                vault.expire()
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

        val present = around(status)
        val arrived = present - around

        around.clear()
        around.addAll(present)

        try {
            if (SystemClock.elapsedRealtime() >= sweep) {
                sweep = SystemClock.elapsedRealtime() + SWEEP_MS
                sweep(vault)
                rejoin(vault, present)
            } else if (arrived.isNotEmpty()) {
                rejoin(vault, arrived)
            }
        } catch (e: Exception) {
            Log.w(TAG, "failed to sweep the group outbox", e)
        }

        val events = JSONArray(String(Vigil.chatEvents(), Charsets.UTF_8))

        for (i in 0 until events.length()) {
            val event = events.getJSONObject(i)
            val peer = event.getString("peer").toInt(16)
            val id = event.optString("group").ifEmpty { null }
                ?.let { ChatVault.groupChat(event.getString("owner"), it) } ?: Calls.hex(peer)

            try {
                when (event.getString("type")) {
                    "message" -> {
                        val text = event.getString("text")
                        val time = System.currentTimeMillis()

                        val nick = event.optString("nick")
                        val quote = ChatVault.quote(event.optString("author"), event.optString("hash"))

                        when (check(vault, id, peer, event)) {
                            ChatVault.GROUP_DROP -> {
                                stored(event.getLong("token"))
                                continue
                            }
                            ChatVault.GROUP_HOLD -> continue
                        }

                        vault.deliver(id, peer, nick, text, time, quote = quote, topic = event.optString("topic"))
                        stored(event.getLong("token"))

                        if (alerts(service, id, event.optBoolean("mention"))) {
                            unseen[id] = (unseen[id] ?: 0) + 1
                            notify(
                                service, id, peer, nick, event.optString("signed"), text, time,
                                event.optString("topic")
                            )
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

                        when (check(vault, id, peer, event)) {
                            ChatVault.GROUP_DROP -> {
                                data.fill(0)
                                stored(token)
                                continue
                            }
                            ChatVault.GROUP_HOLD -> {
                                data.fill(0)
                                continue
                            }
                        }

                        try {
                            vault.deliver(
                                id, peer, nick, "", time, data = data, media = media, name = name,
                                topic = event.optString("topic")
                            )
                        } finally {
                            data.fill(0)
                        }

                        stored(token)

                        if (alerts(service, id, false)) {
                            unseen[id] = (unseen[id] ?: 0) + 1
                            notify(
                                service, id, peer, nick, event.optString("signed"), summary(media, name), time,
                                event.optString("topic")
                            )
                        }
                    }
                    "topic" -> {
                        when (check(vault, id, peer, event)) {
                            ChatVault.GROUP_DROP -> {
                                stored(event.getLong("token"))
                                continue
                            }
                            ChatVault.GROUP_HOLD -> continue
                        }

                        vault.deliver(
                            id, peer, event.optString("nick"), event.getString("name"),
                            System.currentTimeMillis(), "topic", topic = event.getString("topic"),
                            color = event.getString("color").toInt(16)
                        )
                        stored(event.getLong("token"))
                    }
                    "name" -> {
                        vault.peerName(peer, event.optString("nick"), event.optString("signed"))
                        namesChanged()
                    }
                    "avatar" -> {
                        val token = event.getLong("token")

                        if (token == 0L) {
                            vault.peerAvatar(peer, null)
                        } else {
                            val data = Vigil.chatDataTake(token, event.getInt("size"))
                                ?: throw IllegalStateException("avatar $token is gone")

                            try {
                                vault.peerAvatar(peer, data)
                            } finally {
                                data.fill(0)
                            }

                            stored(token)
                        }

                        avatarChanged()
                    }
                    "retract" -> {
                        when (check(vault, id, peer, event)) {
                            ChatVault.GROUP_DROP -> {
                                stored(event.getLong("token"))
                                continue
                            }
                            ChatVault.GROUP_HOLD -> continue
                        }

                        vault.deliver(
                            id, peer, event.optString("nick"), event.getString("hash"),
                            System.currentTimeMillis(), "retract", topic = event.optString("topic")
                        )
                        stored(event.getLong("token"))
                    }
                    "roster" -> {
                        roster(service, vault, id, peer, event)
                        stored(event.getLong("token"))
                    }
                    "leave" -> {
                        left(service, vault, id, Calls.hex(peer))
                        stored(event.getLong("token"))
                    }
                    "ambry" -> vault.deliver(
                        id, peer, "", event.getString("generation"), System.currentTimeMillis(),
                        "ambry"
                    )
                    "sent", "delivered" ->
                        receipt(vault, Receipt(Calls.hex(peer), event.getLong("ref"), event.getString("type")))
                }
                changed()
            } catch (e: Exception) {
                Log.e(TAG, "failed to store a chat event", e)
            }
        }
    }

    private fun check(vault: ChatVault, chat: String, peer: Int, event: JSONObject): Int {
        if (ChatVault.group(chat) == null) {
            return ChatVault.GROUP_STORE
        }

        return vault.groupCheck(chat, Calls.hex(peer), event.getInt("serial"))
    }

    private fun roster(service: VigilVpnService, vault: ChatVault, chat: String, peer: Int, event: JSONObject) {
        val owner = Calls.hex(peer)
        val info = vault.groupInfo(chat)
        val serial = event.getInt("serial")
        val time = System.currentTimeMillis()

        if (info != null && Integer.compareUnsigned(serial, info.getInt("serial")) <= 0) {
            return
        }

        if (info != null && info.getString("state") in listOf("left", "ended")) {
            return
        }

        val list = event.getJSONArray("members")
        val members = (0 until list.length()).map { list.getString(it) }
        val before = info?.let { ChatVault.members(it) } ?: emptyList()
        val name = event.getString("name")
        val state = when {
            event.getBoolean("ended") -> "ended"
            local !in members -> "removed"
            else -> "open"
        }

        if (info == null && state != "open") {
            return
        }

        vault.groupSave(
            chat,
            JSONObject()
                .put("owner", owner)
                .put("id", chat.substring(3))
                .put("name", name)
                .put("members", JSONArray(members))
                .put("serial", serial)
                .put("state", state)
        )

        if (info == null || info.getString("state") == "removed") {
            if (state == "open") {
                vault.deliver(chat, peer, "", name, time, "added")
                announce(service, chat, peer, "added you to ${name.ifEmpty { "a group" }}")
            }
        } else {
            for (member in members.filter { it !in before && it != local }) {
                vault.deliver(chat, member.toInt(16), "", "", time, "joined")
                announce(service, chat, member.toInt(16), "joined the group")
            }

            for (member in before.filter { it !in members && it != local }) {
                vault.deliver(chat, member.toInt(16), "", "", time, "left")
                announce(service, chat, member.toInt(16), "left the group")
            }

            if (name != info.optString("name")) {
                vault.deliver(chat, peer, "", name, time, "renamed")
            }
        }

        if (state != "open") {
            vault.deliver(chat, peer, "", "", time, state)
            drop(vault.groupDrop(chat, before + members))

            if (state == "removed") {
                announce(service, chat, peer, "removed you from the group")
            }
        } else {
            drop(vault.groupDrop(chat, before.filter { it !in members }))
        }

        namesChanged()
    }

    private fun left(service: VigilVpnService, vault: ChatVault, chat: String, member: String) {
        val info = vault.groupInfo(chat) ?: return

        if (info.getString("owner") != local || info.getString("state") != "open") {
            return
        }

        val members = ChatVault.members(info)

        if (member !in members) {
            return
        }

        info.put("members", JSONArray(members.filter { it != member }))
        info.put("serial", info.getInt("serial") + 1)

        vault.groupSave(chat, info)
        drop(vault.groupDrop(chat, listOf(member)))
        vault.deliver(chat, member.toInt(16), "", "", System.currentTimeMillis(), "left")
        announce(service, chat, member.toInt(16), "left the group")

        submit(vault, vault.rosterQueue(chat, info, local))
        namesChanged()
    }

    private fun sweep(vault: ChatVault) {
        val dropped = vault.expire()

        drop(dropped)

        if (dropped.isNotEmpty()) {
            changed()
        }
    }

    fun drop(dropped: List<Pair<String, Long>>) {
        for ((peer, ref) in dropped) {
            if (!Vigil.chatDrop(peer.toInt(16), ref)) {
                Log.w(TAG, "failed to drop $ref for $peer")
            }
        }
    }

    private fun around(status: JSONObject): Set<String> {
        val around = mutableSetOf<String>()
        val list = status.optJSONObject("chat")?.optJSONArray("peers") ?: JSONArray()
        val calls = status.optJSONObject("call")?.optJSONArray("peers") ?: JSONArray()

        for (i in 0 until list.length()) {
            val peer = list.getJSONObject(i)

            if (peer.optBoolean("present") || peer.optBoolean("online")) {
                around.add(peer.getString("id"))
            }
        }

        for (i in 0 until calls.length()) {
            around.add(calls.getString(i))
        }

        return around
    }

    private fun rejoin(vault: ChatVault, around: Set<String>) {
        val groups = vault.groups()

        for (id in groups.keys()) {
            val info = groups.getJSONObject(id)
            val stale = info.optJSONArray("stale") ?: continue

            if (info.getString("owner") != local) {
                continue
            }

            for (member in (0 until stale.length()).map { stale.getString(it) }.filter { it in around }) {
                vault.groupStale(id, member, false)
                submit(vault, vault.rosterQueue(id, vault.groupInfo(id)!!, local, only = listOf(member)))
                submit(vault, vault.topicQueue(id, local, listOf(member)))
            }
        }
    }

    fun submit(vault: ChatVault, entries: JSONArray) {
        for (i in 0 until entries.length()) {
            val entry = entries.getJSONObject(i)
            val ref = entry.getLong("ref")

            if (submit(vault, entry)) {
                waiting.remove(ref)
            } else {
                refused(ref)
            }
        }
    }

    private fun stored(token: Long) {
        if (!Vigil.chatStored(token)) {
            Log.w(TAG, "failed to ack $token, it is handed over again")
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
            val ref = entry.getLong("ref")

            refs.add(ref)

            if (only != null && ref !in only) {
                continue
            }

            if (submit(vault, entry)) {
                waiting.remove(ref)
            } else {
                refused(ref)
            }
        }

        waiting.retainAll(refs)
    }

    private fun submit(vault: ChatVault, entry: JSONObject): Boolean {
        val peer = entry.getString("peer").toInt(16)
        val chat = entry.optString("chat", entry.getString("peer"))
        val group = ChatVault.group(chat)
        val ref = entry.getLong("ref")
        val topic = ChatVault.topicId(entry.optString("tp"))

        when (entry.optString("op")) {
            "topic" -> {
                val name = entry.getString("name").toByteArray()
                val color = entry.getInt("color")

                if (group != null) {
                    return Vigil.chatGroupTopic(peer, ref, group.id, entry.optInt("gs"), topic, color, name)
                }

                return Vigil.chatTopic(peer, ref, topic, color, name)
            }
            "roster" -> {
                val list = entry.getJSONArray("members")

                return Vigil.chatGroupRoster(
                    peer, ref, group!!.id, entry.getInt("serial"), entry.getBoolean("ended"),
                    ChatVault.roster((0 until list.length()).map { list.getString(it) }),
                    entry.getString("name").toByteArray()
                )
            }
            "leave" -> return Vigil.chatGroupLeave(peer, ref, group!!.id)
            "retract" -> {
                val hash = java.lang.Long.parseUnsignedLong(entry.getString("hash"), 16)

                return Vigil.chatRetract(
                    peer, ref, group?.owner ?: 0, group?.id ?: 0L, entry.optInt("gs"), topic, hash
                )
            }
        }

        val quote = ChatVault.quote(entry.optString("qa"), entry.optString("qh"))

        if (!entry.has("data")) {
            val text = entry.getString("text").toByteArray()

            if (group != null) {
                return Vigil.chatGroupSend(
                    peer, ref, group.owner, group.id, entry.optInt("gs"), topic, text, quote?.peer ?: 0,
                    quote?.digest ?: 0L
                )
            }

            return Vigil.chatSend(peer, ref, text, quote?.peer ?: 0, quote?.digest ?: 0L, topic)
        }

        val data = vault.data(chat, entry.getString("data")) ?: return true
        val kind = ChatVault.kind(entry.getString("media"))
        val name = entry.getString("name").toByteArray()

        try {
            if (group != null) {
                return Vigil.chatGroupDataSend(
                    peer, ref, group.owner, group.id, entry.optInt("gs"), topic, kind, name, data
                )
            }

            return Vigil.chatDataSend(peer, ref, kind, name, data, topic)
        } finally {
            data.fill(0)
        }
    }

    private fun receipt(vault: ChatVault, receipt: Receipt) {
        try {
            vault.receipt(receipt.peer, receipt.ref, receipt.kind)
            changed()
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

    fun typing(context: Context, id: String, on: Boolean, topic: String = "") {
        val group = ChatVault.group(id)
        val where = ChatVault.topicId(topic)

        if (group == null) {
            peer(id)?.let { Vigil.chatTyping(it, where, on) }
            return
        }

        val info = if (on) vault(context).groupInfo(id) else null

        if (info != null && info.getString("state") == "open") {
            Vigil.chatGroupTyping(group.owner, group.id, where, ChatVault.roster(ChatVault.members(info)))
        } else {
            Vigil.chatGroupTyping(0, 0L, 0L, null)
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
            typing(context, opened, false)
            opened = ""
        }

        vault?.lock()
    }

    fun json(context: Context): JSONObject {
        val pending = requested
        val topic = requestedTopic
        requested = ""
        requestedTopic = ""

        return JSONObject()
            .put("version", version)
            .put("avatars", avatars)
            .put("names", names)
            .put("unseen", unseen.values.sum())
            .put("hardware", vault(context).hardware)
            .put("open", pending.ifEmpty { JSONObject.NULL })
            .put("openTopic", topic.ifEmpty { JSONObject.NULL })
    }

    @Synchronized
    fun changed() {
        version++
    }

    @Synchronized
    private fun avatarChanged() {
        avatars++
    }

    @Synchronized
    fun namesChanged() {
        names++
    }

    fun avatar(context: Context, data: ByteArray?) {
        require(Vigil.chatAvatar(data)) { "That picture is too large." }

        vault(context).ownAvatar(data)
        avatarChanged()
    }

    fun nick(context: Context): String = prefs(context).getString("nick", "") ?: ""

    fun preview(context: Context): Boolean = prefs(context).getBoolean("preview", false)

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

    fun mute(context: Context, id: String, until: Long?): Long {
        if (until != null) {
            val mutes = mutes(context)

            if (until == 0L) {
                mutes.remove(id)
            } else {
                mutes.put(id, until)
            }

            prefs(context).edit().putString("mutes", mutes.toString()).apply()
        }

        return muted(context, id)
    }

    private fun muted(context: Context, id: String): Long {
        val until = mutes(context).optLong(id)

        if (until > 0 && until <= System.currentTimeMillis()) {
            return 0L
        }

        return until
    }

    private fun mutes(context: Context): JSONObject =
        JSONObject(prefs(context).getString("mutes", "{}") ?: "{}")

    fun star(context: Context, id: String, on: Boolean) {
        val stars = stars(context)

        if (on) {
            stars.put(id, true)
        } else {
            stars.remove(id)
        }

        prefs(context).edit().putString("stars", stars.toString()).apply()
    }

    fun compact(context: Context, id: String, on: Boolean) {
        val compacts = compacts(context)

        if (on) {
            compacts.put(id, true)
        } else {
            compacts.remove(id)
        }

        prefs(context).edit().putString("compacts", compacts.toString()).apply()
    }

    fun order(context: Context, chats: List<String>) {
        prefs(context).edit().putString("order", JSONArray(chats).toString()).apply()
    }

    fun unlist(context: Context, id: String) {
        val order = order(context)

        star(context, id, false)
        compact(context, id, false)

        if (order.remove(id)) {
            order(context, order)
        }
    }

    fun arrange(context: Context, chats: JSONArray): JSONArray {
        val stars = stars(context)
        val compacts = compacts(context)
        val order = order(context)

        for (idx in 0 until chats.length()) {
            val chat = chats.getJSONObject(idx)
            val id = chat.getString("peer")

            chat.put("star", stars.has(id))
                .put("compact", compacts.has(id))
                .put("order", order.indexOf(id))
        }

        return chats
    }

    private fun stars(context: Context): JSONObject =
        JSONObject(prefs(context).getString("stars", "{}") ?: "{}")

    private fun compacts(context: Context): JSONObject =
        JSONObject(prefs(context).getString("compacts", "{}") ?: "{}")

    private fun order(context: Context): MutableList<String> {
        val saved = JSONArray(prefs(context).getString("order", "[]") ?: "[]")

        return MutableList(saved.length()) { saved.getString(it) }
    }

    private fun announce(service: VigilVpnService, chat: String, peer: Int, text: String) {
        if (!alerts(service, chat, false)) {
            return
        }

        unseen[chat] = (unseen[chat] ?: 0) + 1
        notify(service, chat, peer, "", "", text, System.currentTimeMillis(), "")
    }

    private fun alerts(context: Context, id: String, mention: Boolean): Boolean {
        if (foreground && opened == id) {
            return false
        }

        return mention || muted(context, id) == 0L
    }

    private fun peer(id: String): Int? =
        if (id.length == 2) id.toInt(16).takeIf { it in 1..0xfe } else null

    private fun sender(context: Context, peer: Int, nick: String, signed: String): String {
        if (PeerNames.load(context).has(Calls.hex(peer))) {
            return PeerNames.name(context, peer)
        }

        if (signed.isNotEmpty()) {
            return signed
        }

        if (nick.isNotEmpty()) {
            return "~$nick"
        }

        return PeerNames.name(context, peer)
    }

    private fun summary(media: String, name: String): String =
        when (media) {
            "image" -> "Image"
            "voice" -> "Voice message"
            else -> "File: $name"
        }

    private fun notify(
        service: VigilVpnService, id: String, peer: Int, nick: String, signed: String, text: String,
        time: Long, topic: String
    ) {
        val name = sender(service, peer, nick, signed)
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

        builder.setContentIntent(launch(service, notification(id), id, topic))

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

        builder.setContentIntent(launch(context, SUMMARY, shown.keys.singleOrNull() ?: CHAT_LIST))

        manager.notify(SUMMARY, builder.build())
    }

    private fun launch(context: Context, code: Int, id: String, topic: String = ""): PendingIntent =
        PendingIntent.getActivity(
            context, code,
            VigilVpnService.notified(context, ACTION_CHAT).putExtra(EXTRA_CHAT, id).putExtra(EXTRA_TOPIC, topic),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )

    private fun dismiss(context: Context, id: String) {
        shown.remove(id)
        context.getSystemService(NotificationManager::class.java).cancel(notification(id))
        summary(context)
    }

    private fun notification(id: String): Int {
        if (ChatVault.group(id) != null) {
            return GROUP_NOTIFICATION_BASE + (id.hashCode() and 0xffff)
        }

        return NOTIFICATION_BASE + id.toInt(16)
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences("chats", Context.MODE_PRIVATE)

    const val ACTION_CHAT = "se.sanctorum.vigil.CHAT"
    const val EXTRA_CHAT = "chat"
    const val EXTRA_TOPIC = "topic"
    const val CHAT_LIST = "gospel"

    private const val TAG = "vigil"
    private const val CHANNEL = "messages"
    private const val NOTIFICATION_BASE = 0x1000
    private const val GROUP_NOTIFICATION_BASE = 0x10000
    private const val SHOWN_MAX = 6
    private const val SUMMARY = 0x0fff
    private const val GROUP = "se.sanctorum.vigil.chats"
    private const val RESUBMIT_MS = 10_000L
    private const val SWEEP_MS = 15_000L
}
