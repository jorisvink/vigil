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
import java.security.SecureRandom

object Chats {
    private data class Receipt(val peer: String, val ref: Long, val kind: String)

    private var vault: ChatVault? = null
    private var session = 0L
    private var resubmit = 0L
    private var sweep = 0L
    private var screen: BroadcastReceiver? = null
    private var safetyHalf: ByteArray? = null
    private var safetyFor = ""
    private data class Shown(val text: String, val time: Long, val name: String, val key: String)

    private val shown = mutableMapOf<String, MutableList<Shown>>()
    private val unseen = mutableMapOf<String, Int>()
    private val receipts = mutableListOf<Receipt>()
    private val waiting = mutableSetOf<Long>()
    private val around = mutableSetOf<String>()
    private val asking = mutableSetOf<String>()
    private val sessions = mutableMapOf<String, Long>()
    private var accepts = -1L

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
    var locals = mapOf<String, String>()
        private set

    fun vault(context: Context): ChatVault =
        vault ?: ChatVault(context.applicationContext).also {
            val store = TunnelStore(context.applicationContext)

            it.mirror = ::mirror
            it.linked = store::linked
            it.flock = { store.firstFlock()?.also { flock -> migrate(context.applicationContext, flock) } }
            it.reach(prefs(context.applicationContext).getBoolean("reachable", true))
            vault = it
        }

    private fun migrate(context: Context, flock: String) {
        val prefs = prefs(context)

        if (prefs.getBoolean("flocked", false)) {
            return
        }

        val edit = prefs.edit()

        for (key in listOf("stars", "compacts", "mutes", "codes")) {
            val old = JSONObject(prefs.getString(key, "{}") ?: "{}")
            val moved = JSONObject()

            for (id in old.keys()) {
                moved.put("$flock-$id", old.get(id))
            }

            edit.putString(key, moved.toString())
        }

        val order = JSONArray(prefs.getString("order", "[]") ?: "[]")

        edit.putString("order", JSONArray((0 until order.length()).map { "$flock-${order.getString(it)}" }).toString())
        edit.putBoolean("flocked", true).apply()

        PeerNames.migrate(context, flock)
    }

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
        val gospels = chat.optJSONArray("gospels") ?: JSONArray()
        val running = mutableMapOf<String, Long>()
        val own = mutableMapOf<String, String>()

        for (i in 0 until gospels.length()) {
            val gospel = gospels.getJSONObject(i)

            running[gospel.getString("flock")] = gospel.optLong("session")
            own[gospel.getString("flock")] = Flock.local(gospel.getString("id"))
        }

        locals = own

        val fresh = running.filter { sessions[it.key] != it.value }.keys

        if (chat.optLong("session") != session || fresh.isNotEmpty()) {
            resubmit = 0L
            accepts = -1L
            waiting.clear()

            try {
                if (chat.optLong("session") != session) {
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
                }

                handover(vault, null, fresh)
                session = chat.optLong("session")
                sessions.clear()
                sessions.putAll(running)
            } catch (e: Exception) {
                Log.w(TAG, "failed to hand over the outbox", e)
            }
        } else if (resubmit != 0L && System.currentTimeMillis() >= resubmit) {
            resubmit = 0L

            try {
                handover(vault, waiting.toSet(), running.keys)
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

        try {
            admit(vault)
        } catch (e: Exception) {
            Log.w(TAG, "failed to hand over the peers we accepted", e)
        }

        val events = JSONArray(String(Vigil.chatEvents(), Charsets.UTF_8))

        for (i in 0 until events.length()) {
            val event = events.getJSONObject(i)
            val flock = Flock.hex(event.getString("flock"))
            val peer = event.getString("peer").toInt(16)
            val from = Flock.id(flock, event.getString("peer"))
            val id = event.optString("group").ifEmpty { null }
                ?.let { ChatVault.groupChat(flock, event.getString("owner"), it) } ?: from

            try {
                when (event.getString("type")) {
                    "message" -> {
                        val text = event.getString("text")
                        val time = System.currentTimeMillis()

                        val nick = event.optString("nick")
                        val quote = ChatVault.quote(event.optString("author"), event.optString("hash"))

                        if (!admitted(vault, id, peer, event)) {
                            continue
                        }

                        vault.deliver(id, peer, nick, text, time, quote = quote, topic = event.optString("topic"))
                        stored(event.getLong("token"))

                        if (alerts(service, id, event.optBoolean("mention"))) {
                            unseen[id] = (unseen[id] ?: 0) + 1
                            notify(
                                service, id, from, nick, event.optString("signed"), text, time,
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

                        if (!admitted(vault, id, peer, event)) {
                            data.fill(0)
                            continue
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
                                service, id, from, nick, event.optString("signed"), summary(media, name), time,
                                event.optString("topic")
                            )
                        }
                    }
                    "topic" -> {
                        if (!admitted(vault, id, peer, event)) {
                            continue
                        }

                        vault.deliver(
                            id, peer, event.optString("nick"), event.getString("name"),
                            System.currentTimeMillis(), "topic", topic = event.getString("topic"),
                            color = event.getString("color").toInt(16)
                        )
                        stored(event.getLong("token"))
                    }
                    "name" -> {
                        vault.peerName(from, event.optString("nick"), event.optString("signed"))
                        namesChanged()
                    }
                    "ask" -> {
                        if (asked(vault, from)) {
                            changed()
                            announce(service, from, from, "Is trying to reach you, accept or decline under Peers")
                        }
                    }
                    "avatar" -> {
                        val token = event.getLong("token")

                        if (token == 0L) {
                            vault.peerAvatar(from, null)
                        } else {
                            val data = Vigil.chatDataTake(token, event.getInt("size"))
                                ?: throw IllegalStateException("avatar $token is gone")

                            try {
                                vault.peerAvatar(from, data)
                            } finally {
                                data.fill(0)
                            }

                            stored(token)
                        }

                        avatarChanged()
                    }
                    "retract" -> {
                        if (!admitted(vault, id, peer, event)) {
                            continue
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
                    "safety" -> safetyStore(service, from)
                    "ambry" -> vault.deliver(
                        id, peer, "", event.getString("generation"), System.currentTimeMillis(),
                        "ambry"
                    )
                    "echo" -> {
                        echo(vault, id, event)
                        stored(event.optLong("token"))
                    }
                    "desktop" -> desktop(service, event)
                    "sent", "delivered" ->
                        receipt(vault, Receipt(from, event.getLong("ref"), event.getString("type")))
                }
                changed()
            } catch (e: Exception) {
                Log.e(TAG, "failed to store a chat event", e)
            }
        }

        resyncStep(service, vault, status)
    }

    private fun echo(vault: ChatVault, chat: String, event: JSONObject) {
        val what = event.getString("what")
        val list = event.optJSONArray("members")
        val to = (0 until (list?.length() ?: 0)).map { list!!.getString(it) }
        val quote = ChatVault.quote(event.optString("author"), event.optString("hash"))
        val time = System.currentTimeMillis()

        if (vault.groupGone(chat)) {
            unsent(chat, event, to)
            return
        }

        if (what == "text") {
            vault.echo(
                chat, event.getLong("ref"), event.getString("text"), time, quote, event.optString("topic"), to,
                event.optInt("serial")
            )
            return
        }

        if (what != "data") {
            return
        }

        val data = Vigil.chatDataTake(event.getLong("token"), event.getInt("size"))
            ?: throw IllegalStateException("data ${event.getLong("token")} is gone")

        try {
            vault.echo(
                chat, event.getLong("ref"), "", time, null, event.optString("topic"), to, event.optInt("serial"),
                data = data, media = event.getString("kind"), name = event.getString("name")
            )
        } finally {
            data.fill(0)
        }
    }

    private fun unsent(chat: String, event: JSONObject, to: List<String>) {
        val ref = event.getLong("ref")
        val token = event.optLong("token")

        Log.i(TAG, "dropped a message from our desktop to $chat, we are no longer in that group")

        if (event.optString("what") == "data") {
            Vigil.chatDataTake(token, event.getInt("size"))?.fill(0)
        }

        for (member in to) {
            Vigil.chatDrop(Flock.number(chat), member.toInt(16), ref)
        }

        val answer = JSONObject()
            .put("op", "answer")
            .put("what", "send")
            .put("ok", false)
            .put("reason", "You are no longer in that group.")
            .put("chat", chat)
            .put("ref", ref)

        if (!mirror(answer, null)) {
            Log.w(TAG, "could not tell our desktop its message to $chat was not sent")
        }
    }

    private fun desktop(service: VigilVpnService, event: JSONObject) {
        if (event.getString("what") == "resync") {
            resync(service)
            return
        }

        if (event.getString("what") != "ask") {
            return
        }

        val ask = JSONObject(event.getString("ask"))
        val token = event.optLong("token")
        val data = if (event.optInt("size") > 0) Vigil.chatDataTake(token, event.getInt("size")) else null
        val answer = JSONObject().put("op", "answer").put("id", ask.optLong("id")).put("what", ask.optString("what"))

        try {
            asked(service, ask, data)
            answer.put("ok", true)
        } catch (e: ChatVault.Locked) {
            Log.w(TAG, "could not do what our desktop asked while locked")
            answer.put("ok", false).put("reason", "Unlock your phone to do that.")
        } catch (e: Exception) {
            Log.w(TAG, "could not do what our desktop asked", e)
            answer.put("ok", false).put("reason", e.message ?: "")
        } finally {
            data?.fill(0)
        }

        if (!mirror(answer, null)) {
            Log.w(TAG, "could not answer our desktop")
        }

        stored(token)
    }

    private fun asked(service: VigilVpnService, ask: JSONObject, data: ByteArray?) {
        val chat = ask.optString("chat")

        when (ask.getString("what")) {
            "topic" -> topic(service, checked(chat), ask.optString("topic"), ask.getString("name"),
                ask.getInt("color"))
            "retract" -> retract(
                service, checked(chat), ask.getLong("ref"), ask.optString("hash"), ask.optString("topic")
            )
            "group" -> groupCreate(
                service, ask.getString("flock"), ask.getString("name"), strings(ask.getJSONArray("members"))
            )
            "edit" -> groupEdit(
                service, checked(chat), if (ask.has("name")) ask.getString("name") else null,
                ask.optJSONArray("members")?.let { strings(it) }
            )
            "leave" -> groupLeave(service, checked(chat))
            "delete" -> delete(service, checked(chat))
            "read" -> {
                vault(service).read(checked(chat), ask.optString("topic"))
                seen(service, chat)
            }
            "favorite" -> vault(service).favorite(checked(chat), ask.getLong("ref"), ask.getBoolean("on"))
            "pin" -> vault(service).pin(checked(chat), ask.getLong("ref"), ask.getBoolean("on"))
            "star" -> star(service, checked(chat), ask.getBoolean("on"))
            "compact" -> compact(service, checked(chat), ask.getBoolean("on"))
            "mute" -> mute(service, checked(chat), ask.getLong("until"))
            "nick" -> settings(service, ask.getString("nick"), null)
            "avatar" -> {
                val image = data?.let { Images.avatar(it) }

                try {
                    avatar(service, image)
                } finally {
                    image?.fill(0)
                }
            }
            else -> throw IllegalArgumentException("Vigil does not know how to do that.")
        }

        changed()
    }

    fun checked(chat: String): String {
        require(ChatVault.group(chat) != null || Flock.valid(chat)) { "There is no chat $chat." }

        return chat
    }

    private class Resync(val full: Boolean) {
        val what: String
            get() {
                if (full) {
                    return "full resync of our desktop"
                }

                return "repair of what our desktop missed"
            }

        var chats: List<String>? = null
        var chat = 0
        var at = 0
        var file = 0
        val files = mutableListOf<Pair<String, String>>()
    }

    private var resync: Resync? = null

    private fun mirror(op: JSONObject, data: ByteArray?): Boolean =
        Vigil.chatCompanionSync(op.toString().toByteArray(), data)

    private fun resync(context: Context) {
        prefs(context).edit().putBoolean("resync", true).apply()
        vault(context).dumpEnd()
        resync = Resync(true)
    }

    private fun resyncStep(service: VigilVpnService, vault: ChatVault, status: JSONObject) {
        if (resync == null && prefs(service).getBoolean("resync", false)) {
            resync = Resync(true)
        }

        val companion = status.optJSONObject("chat")?.optJSONObject("companion") ?: return

        if (companion.optString("role") != "phone") {
            return
        }

        if (resync == null && !vault.lostSet().isEmpty()) {
            resync = Resync(false)
        }

        val job = resync ?: return

        var room = companion.optLong("room") - RESYNC_MARGIN

        if (room < RESYNC_MARGIN) {
            return
        }

        val dump = ChatVault.Dump(minOf(room / 2, RESYNC_STEP).toInt())

        try {
            if (job.chats == null) {
                if (!synchronized(vault) { resyncHead(service, vault, job) }) {
                    return
                }

                Log.i(TAG, "${job.what} started, ${job.chats?.size} chats")
            }

            val chats = job.chats ?: return

            while (job.chat < chats.size && dump.budget > 0) {
                val chat = chats[job.chat]
                val names = mutableListOf<String>()

                dump.at = job.at

                val done = vault.dumpChat(chat, dump, names)

                if (job.full) {
                    job.files.addAll(names.map { chat to it })
                }

                job.at = dump.at

                if (!done) {
                    return
                }

                job.chat++
                job.at = 0
                Log.i(TAG, "${job.what}: sent chat ${job.chat} of ${chats.size}")
            }

            room -= minOf(room / 2, RESYNC_STEP) - dump.budget

            while (job.chat == chats.size && job.file < job.files.size && dump.budget > 0) {
                val (chat, file) = job.files[job.file]

                if (vault.dataSize(file) > room) {
                    return
                }

                val sent = vault.dumpData(chat, file)

                if (sent < 0) {
                    return
                }

                room -= sent
                dump.budget -= sent
                job.file++
                Log.i(TAG, "${job.what}: sent file ${job.file} of ${job.files.size}")
            }

            if (job.chat == chats.size && job.file == job.files.size && Vigil.chatCompanionResync(true)) {
                resync = null

                if (job.full) {
                    prefs(service).edit().putBoolean("resync", false).apply()
                }

                Log.i(TAG, "${job.what} finished")
            }
        } catch (e: ChatVault.Locked) {
            return
        } catch (e: Exception) {
            Log.w(TAG, "resync of our desktop failed, trying again", e)
        }
    }

    private fun resyncHead(context: Context, vault: ChatVault, job: Resync): Boolean {
        val head = vault.dumpHead()

        try {
            if (job.full && !Vigil.chatCompanionResync(false)) {
                return false
            }

            val lost = vault.lostSet()

            if (job.full || lost.state) {
                if (!mirror(prefsOf(context), null)) {
                    return false
                }

                if (!vault.dumpHead(head)) {
                    return false
                }

                if (job.full) {
                    job.chats = head.chats
                    return true
                }
            }

            job.chats = lost.chats.toList()
            job.files.addAll(lost.files.map { it.value to it.key })

            return true
        } finally {
            head.wipe()
        }
    }

    fun prefsChanged(context: Context) {
        vault(context).emit(prefsOf(context))
    }

    private fun prefsOf(context: Context): JSONObject =
        JSONObject()
            .put("op", "prefs")
            .put("stars", stars(context))
            .put("compacts", compacts(context))
            .put("mutes", mutes(context))
            .put("order", JSONArray(order(context)))
            .put("names", PeerNames.load(context))

    private fun strings(list: JSONArray): List<String> = (0 until list.length()).map { list.getString(it) }

    fun self(chat: String): String = local(Flock.of(chat))

    private fun local(flock: String): String =
        locals[flock] ?: throw IllegalArgumentException("Gospel is not running for flock $flock.")

    fun safetyCode(context: Context, id: String, clear: Boolean, on: Boolean?): String {
        val peer = safetyPeer(id)
        val vault = vault(context)

        safetyStore(context, peer)

        val codes = codes(context)

        if (on != null) {
            if (on) {
                codes.put(id, true)
            } else {
                codes.remove(id)
            }

            prefs(context).edit().putString("codes", codes.toString()).apply()
        }

        if (clear) {
            vault.safetyCode(peer, null)
        }

        val stored = vault.safetyCode(peer)?.takeIf { it.size == ChatVault.INPUT }
        val code = stored?.let { Vigil.safetyCode(it) } ?: ""

        stored?.fill(0)

        if (on != null || clear) {
            safetyApply(context, peer)
        }

        return JSONObject().put("set", code.isNotEmpty()).put("code", code).put("since", vault.safetyTime(peer))
            .put("on", codes.optBoolean(id)).toString()
    }

    @Synchronized
    fun safetyBegin(id: String): String {
        val peer = safetyPeer(id)
        val half = ByteArray(8).also { SecureRandom().nextBytes(it) }

        Vigil.safetyStop()
        safetyHalf?.fill(0)
        safetyHalf = half
        safetyFor = peer

        return Vigil.safetyWords(half) ?: throw IllegalStateException("No words for our half.")
    }

    @Synchronized
    fun safetyWords(id: String, words: ByteArray) {
        val peer = safetyPeer(id)
        val own = safetyHalf?.takeIf { safetyFor == peer } ?: throw IllegalArgumentException("Open the safety code again.")
        val theirs = Vigil.safetyHalf(words) ?: throw IllegalArgumentException("Those are not $SAFETY_WORDS words from the list.")

        try {
            require(!own.contentEquals(theirs)) { "Those are your own words." }
            require(Vigil.safetyStart(Flock.number(peer), Flock.peer(peer), own, theirs)) {
                "Could not start the safety code exchange."
            }
        } finally {
            theirs.fill(0)
        }
    }

    @Synchronized
    fun safetyStop(context: Context) {
        val peer = safetyFor

        Vigil.safetyStop()

        if (peer.isNotEmpty()) {
            safetyStore(context, peer)
        }

        safetyHalf?.fill(0)
        safetyHalf = null
        safetyFor = ""
    }

    fun safetyStore(context: Context, peer: String) {
        val input = Vigil.safetyGet(Flock.number(peer), Flock.peer(peer)) ?: return
        val codes = codes(context)

        try {
            vault(context).safetyCode(peer, input)
        } finally {
            input.fill(0)
        }

        codes.put(peer, true)
        prefs(context).edit().putString("codes", codes.toString()).apply()
        safetyApply(context, peer)
    }

    private fun safetyPeer(id: String): String {
        require(Flock.valid(id)) { "There is no peer $id." }

        return id
    }

    fun safetyCodes(context: Context): String {
        val set = JSONArray()

        for ((peer, input) in vault(context).safetyInputs()) {
            set.put(peer)
            input.fill(0)
        }

        return JSONObject().put("on", JSONArray(codes(context).keys().asSequence().toList())).put("set", set).toString()
    }

    private fun safetyApply(context: Context, peer: String) {
        val input = vault(context).safetyInputs()[peer]

        if (input != null && codes(context).optBoolean(peer)) {
            Vigil.additionalInput(Flock.number(peer), Flock.peer(peer), input)
        } else {
            Vigil.additionalInput(Flock.number(peer), Flock.peer(peer), null)
        }

        input?.fill(0)
    }

    fun safetyApplyAll(context: Context) {
        try {
            val codes = codes(context)

            for ((peer, input) in vault(context).safetyInputs()) {
                if (codes.optBoolean(peer)) {
                    Vigil.additionalInput(Flock.number(peer), Flock.peer(peer), input)
                }

                input.fill(0)
            }
        } catch (e: Exception) {
            Log.w(TAG, "failed to load the safety codes for our peers", e)
        }
    }

    private fun codes(context: Context): JSONObject =
        JSONObject(prefs(context).getString("codes", "{}") ?: "{}")

    fun topic(context: Context, id: String, topic: String, name: String, color: Int): String {
        val trimmed = name.trim()
        val chosen = topic.ifEmpty {
            "%016x".format(SecureRandom().nextLong().takeIf { it != 0L } ?: 1L)
        }

        require(trimmed.toByteArray().size <= TOPIC_NAME_MAX) { "A topic name is at most $TOPIC_NAME_MAX bytes." }
        require(chosen.matches(Regex("[0-9a-f]{16}")) && chosen != "0".repeat(16)) { "There is no topic $topic." }

        val vault = vault(context)
        val self = if (ChatVault.group(id) != null) self(id) else ""

        submit(vault, vault.topic(id, chosen, trimmed, color and 0xffffff, vault.ref(), self))
        merge(vault)
        changed()

        return chosen
    }

    fun retract(context: Context, id: String, ref: Long, hash: String = "", topic: String = "") {
        val vault = vault(context)
        val self = if (ChatVault.group(id) != null) self(id) else ""
        val retracted = try {
            vault.retract(id, ref, self)
        } catch (e: ChatVault.Locked) {
            if (!hash.matches(Regex("[0-9a-f]{16}"))) {
                throw e
            }

            vault.retractHash(id, ref, java.lang.Long.parseUnsignedLong(hash, 16), topic, self)
        }

        drop(retracted.dropped)
        submit(vault, retracted.queued)
        changed()
    }

    fun groupCreate(context: Context, picked: String, name: String, chosen: List<String>): String {
        val flock = Flock.hex(picked)
        val local = local(flock)
        val trimmed = name.trim()
        val members = chosen.filter { Flock.valid(it) && Flock.of(it) == flock }.map { Flock.local(it) }
            .filter { it != local }.distinct()

        require(trimmed.toByteArray().size <= GROUP_NAME_MAX) { "A group name is at most $GROUP_NAME_MAX bytes." }
        require(members.isNotEmpty()) { "Pick at least one member of that flock." }

        val id = "%016x".format(SecureRandom().nextLong().takeIf { it != 0L } ?: 1L)
        val chat = ChatVault.groupChat(flock, local, id)
        val vault = vault(context)
        val info = JSONObject()
            .put("owner", local)
            .put("id", id)
            .put("name", trimmed)
            .put("members", JSONArray(members + local))
            .put("serial", 1)
            .put("state", "open")
            .put("topics", JSONArray())

        vault.groupSave(chat, info)
        vault.deliver(chat, local.toInt(16), "", trimmed, System.currentTimeMillis(), "created")

        submit(vault, vault.rosterQueue(chat, info, local))
        merge(vault)
        namesChanged()
        changed()

        return chat
    }

    fun groupEdit(context: Context, chat: String, name: String?, picked: List<String>?) {
        val local = self(chat)
        val flock = Flock.of(chat)
        val vault = vault(context)
        val info = vault.groupInfo(chat) ?: throw IllegalArgumentException("There is no group $chat.")
        val before = ChatVault.members(info)
        val time = System.currentTimeMillis()

        require(info.getString("owner") == local) { "Only the owner can change a group." }
        require(info.getString("state") == "open") { "This group is closed." }

        val members = picked?.filter { Flock.valid(it) && Flock.of(it) == flock }?.map { Flock.local(it) }
            ?.filter { it != local }?.distinct()?.plus(local) ?: before

        require(members.size > 1) { "A group needs at least one other member." }

        if (name != null) {
            val trimmed = name.trim()

            require(trimmed.toByteArray().size <= GROUP_NAME_MAX) { "A group name is at most $GROUP_NAME_MAX bytes." }

            if (trimmed != info.getString("name")) {
                vault.deliver(chat, local.toInt(16), "", trimmed, time, "renamed")
            }

            info.put("name", trimmed)
        }

        val gone = before.filter { it !in members }

        for (member in members.filter { it !in before }) {
            vault.deliver(chat, member.toInt(16), "", "", time, "joined")
        }

        for (member in gone) {
            vault.deliver(chat, member.toInt(16), "", "", time, "left")
        }

        info.put("members", JSONArray(members))
        info.put("serial", info.getInt("serial") + 1)
        info.put("topics", vault.topicsOf(chat, info))

        vault.groupSave(chat, info)
        drop(vault.groupDrop(chat, gone))

        submit(vault, vault.rosterQueue(chat, info, local, gone))
        submit(vault, vault.topicQueue(chat, local, members.filter { it !in before }))
        merge(vault)
        namesChanged()
        changed()
    }

    fun accept(context: Context, chat: String, on: Boolean) {
        val vault = vault(context)

        synchronized(this) {
            asking.remove(chat)
        }

        if (on) {
            vault.accept(chat, true)
            admit(vault)
        }

        seen(context, chat)
        changed()
    }

    fun ignore(context: Context, chat: String, on: Boolean) {
        val vault = vault(context)

        synchronized(this) {
            asking.remove(chat)
        }

        vault.ignore(chat, on)
        admit(vault)
        seen(context, chat)
        changed()
    }

    fun delete(context: Context, chat: String) {
        val vault = vault(context)

        if (synchronized(this) { asking.remove(chat) }) {
            seen(context, chat)
            changed()
            return
        }

        if (ChatVault.group(chat) == null) {
            check(Vigil.chatForget(Flock.number(chat), Flock.peer(chat))) {
                "Vigil is busy, try deleting the chat again."
            }
        } else if (vault.groupInfo(chat)?.optString("state") == "open") {
            groupLeave(context, chat)
        }

        vault.delete(chat)
        mute(context, chat, 0L)
        unlist(context, chat)
        seen(context, chat)
        changed()
    }

    fun groupLeave(context: Context, chat: String) {
        val local = self(chat)
        val vault = vault(context)
        val info = vault.groupInfo(chat) ?: return
        val owner = info.getString("owner")
        val time = System.currentTimeMillis()

        if (info.getString("state") != "open") {
            return
        }

        drop(vault.groupDrop(chat, ChatVault.members(info) + owner))

        if (owner == local) {
            info.put("state", "ended")
            info.put("serial", info.getInt("serial") + 1)
            vault.groupSave(chat, info)
            vault.deliver(chat, local.toInt(16), "", "", time, "ended")
            submit(vault, vault.rosterQueue(chat, info, local))
        } else {
            info.put("state", "left")
            vault.groupSave(chat, info)
            vault.deliver(chat, local.toInt(16), "", "", time, "parted")
            submit(vault, JSONArray().put(vault.leaveQueue(chat, owner)))
        }

        merge(vault)
        namesChanged()
        changed()
    }

    private fun admitted(vault: ChatVault, chat: String, peer: Int, event: JSONObject): Boolean {
        if (ChatVault.group(chat) == null) {
            return true
        }

        when (vault.groupCheck(chat, Calls.hex(peer), event.getInt("serial"))) {
            ChatVault.GROUP_DROP -> {
                stored(event.getLong("token"))
                return false
            }
            ChatVault.GROUP_HOLD -> return false
        }

        return true
    }

    private fun roster(service: VigilVpnService, vault: ChatVault, chat: String, peer: Int, event: JSONObject) {
        val owner = Calls.hex(peer)
        val local = self(chat)
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
                .put("id", "%016x".format(ChatVault.group(chat)!!.id))
                .put("name", name)
                .put("members", JSONArray(members))
                .put("serial", serial)
                .put("state", state)
        )

        val flock = Flock.of(chat)

        if (info == null || info.getString("state") == "removed") {
            if (state == "open") {
                vault.deliver(chat, peer, "", name, time, "added")
                announce(service, chat, Flock.id(flock, owner), "added you to ${name.ifEmpty { "a group" }}")
            }
        } else {
            for (member in members.filter { it !in before && it != local }) {
                vault.deliver(chat, member.toInt(16), "", "", time, "joined")
                announce(service, chat, Flock.id(flock, member), "joined the group")
            }

            for (member in before.filter { it !in members && it != local }) {
                vault.deliver(chat, member.toInt(16), "", "", time, "left")
                announce(service, chat, Flock.id(flock, member), "left the group")
            }

            if (name != info.optString("name")) {
                vault.deliver(chat, peer, "", name, time, "renamed")
            }
        }

        if (state != "open") {
            vault.deliver(chat, peer, "", "", time, state)
            drop(vault.groupDrop(chat, before + members))

            if (state == "removed") {
                announce(service, chat, Flock.id(flock, owner), "removed you from the group")
            }
        } else {
            drop(vault.groupDrop(chat, before.filter { it !in members }))
        }

        namesChanged()
    }

    private fun left(service: VigilVpnService, vault: ChatVault, chat: String, member: String) {
        val info = vault.groupInfo(chat) ?: return
        val local = self(chat)

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
        announce(service, chat, Flock.id(Flock.of(chat), member), "left the group")

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
            if (!Vigil.chatDrop(Flock.number(peer), Flock.peer(peer), ref)) {
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
            val flock = Flock.of(id)
            val local = locals[flock] ?: continue

            if (info.getString("owner") != local) {
                continue
            }

            val members = (0 until stale.length()).map { stale.getString(it) }

            for (member in members.filter { Flock.id(flock, it) in around }) {
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

    private fun handover(vault: ChatVault, only: Set<Long>?, flocks: Set<String>) {
        val outbox = vault.outboxList()
        val refs = mutableSetOf<Long>()

        for (i in 0 until outbox.length()) {
            val entry = outbox.getJSONObject(i)
            val ref = entry.getLong("ref")

            refs.add(ref)

            if (only != null && ref !in only) {
                continue
            }

            if (Flock.of(entry.getString("peer")) !in flocks) {
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
        if (entry.optBoolean("echo")) {
            return true
        }

        val flock = Flock.number(entry.getString("peer"))
        val peer = Flock.peer(entry.getString("peer"))
        val chat = entry.optString("chat", entry.getString("peer"))
        val group = ChatVault.group(chat)
        val ref = entry.getLong("ref")
        val topic = ChatVault.topicId(entry.optString("tp"))

        when (entry.optString("op")) {
            "topic" -> {
                val name = entry.getString("name").toByteArray()
                val color = entry.getInt("color")

                if (group != null) {
                    return Vigil.chatGroupTopic(flock, peer, ref, group.id, entry.optInt("gs"), topic, color, name)
                }

                return Vigil.chatTopic(flock, peer, ref, topic, color, name)
            }
            "roster" -> {
                val list = entry.getJSONArray("members")

                return Vigil.chatGroupRoster(
                    flock, peer, ref, group!!.id, entry.getInt("serial"), entry.getBoolean("ended"),
                    ChatVault.roster((0 until list.length()).map { list.getString(it) }),
                    entry.getString("name").toByteArray()
                )
            }
            "leave" -> return Vigil.chatGroupLeave(flock, peer, ref, group!!.id)
            "retract" -> {
                val hash = java.lang.Long.parseUnsignedLong(entry.getString("hash"), 16)

                return Vigil.chatRetract(
                    flock, peer, ref, group?.owner ?: 0, group?.id ?: 0L, entry.optInt("gs"), topic, hash
                )
            }
        }

        val quote = ChatVault.quote(entry.optString("qa"), entry.optString("qh"))

        if (!entry.has("data")) {
            val text = entry.getString("text").toByteArray()

            if (group != null) {
                return Vigil.chatGroupSend(
                    flock, peer, ref, group.owner, group.id, entry.optInt("gs"), topic, text, quote?.peer ?: 0,
                    quote?.digest ?: 0L
                )
            }

            return Vigil.chatSend(flock, peer, ref, text, quote?.peer ?: 0, quote?.digest ?: 0L, topic)
        }

        val data = try {
            vault.data(chat, entry.getString("data")) ?: return true
        } catch (e: ChatVault.Locked) {
            return false
        }
        val kind = ChatVault.kind(entry.getString("media"))
        val name = entry.getString("name").toByteArray()

        try {
            if (group != null) {
                return Vigil.chatGroupDataSend(
                    flock, peer, ref, group.owner, group.id, entry.optInt("gs"), topic, kind, name, data
                )
            }

            return Vigil.chatDataSend(flock, peer, ref, kind, name, data, topic)
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
        if (Flock.valid(opened) && opened != id) {
            Vigil.chatOpen(Flock.number(opened), Flock.peer(opened), false)
        }

        opened = if (on) id else ""

        if (Flock.valid(id)) {
            Vigil.chatOpen(Flock.number(id), Flock.peer(id), on)
        }

        if (on) {
            seen(context, id)
        }
    }

    fun typing(context: Context, id: String, on: Boolean, topic: String = "") {
        val group = ChatVault.group(id)
        val where = ChatVault.topicId(topic)

        if (group == null) {
            if (Flock.valid(id)) {
                Vigil.chatTyping(Flock.number(id), Flock.peer(id), where, on)
            }
            return
        }

        val info = if (on) vault(context).groupInfo(id) else null

        if (info != null && info.getString("state") == "open") {
            Vigil.chatGroupTyping(
                Flock.number(id), group.owner, group.id, where, ChatVault.roster(ChatVault.members(info))
            )
        } else {
            Vigil.chatGroupTyping(0L, 0, 0L, 0L, null)
        }
    }

    fun seen(context: Context, id: String) {
        unseen.remove(id)
        dismiss(context, id)
    }

    fun background(context: Context) {
        foreground = false

        if (opened != "") {
            if (Flock.valid(opened)) {
                Vigil.chatOpen(Flock.number(opened), Flock.peer(opened), false)
            }

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

    fun topicsFill(vault: ChatVault) {
        for ((flock, local) in locals) {
            vault.topicsFill(flock, local)
        }
    }

    @Synchronized
    private fun merge(vault: ChatVault) {
        try {
            vault.merge()
        } catch (e: ChatVault.Locked) {
            Log.i(TAG, "inbox stays until the device is unlocked")
        }
    }

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

    fun reachable(context: Context, on: Boolean?): Boolean {
        val vault = vault(context)

        if (on != null) {
            prefs(context).edit().putBoolean("reachable", on).apply()
            vault.reach(on)
            admit(vault)
            changed()
        }

        return vault.reachable
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

    fun mute(context: Context, id: String, until: Long?): Long {
        if (until != null) {
            val mutes = mutes(context)

            if (until == 0L) {
                mutes.remove(id)
            } else {
                mutes.put(id, until)
            }

            prefs(context).edit().putString("mutes", mutes.toString()).apply()
            prefsChanged(context)
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
        prefsChanged(context)
    }

    fun compact(context: Context, id: String, on: Boolean) {
        val compacts = compacts(context)

        if (on) {
            compacts.put(id, true)
        } else {
            compacts.remove(id)
        }

        prefs(context).edit().putString("compacts", compacts.toString()).apply()
        prefsChanged(context)
    }

    fun order(context: Context, chats: List<String>) {
        prefs(context).edit().putString("order", JSONArray(chats).toString()).apply()
        prefsChanged(context)
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
        val listed = (0 until chats.length()).map { chats.getJSONObject(it).getString("peer") }.toSet()

        synchronized(this) {
            for (id in asking - listed) {
                chats.put(
                    JSONObject().put("peer", id).put("unread", 0).put("last", JSONObject.NULL)
                        .put("topics", JSONArray()).put("request", true)
                )
            }
        }

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

    @Synchronized
    private fun admit(vault: ChatVault) {
        if (vault.accepts == accepts) {
            return
        }

        accepts = vault.accepts

        val admits = vault.admits()
        val ignored = vault.ignored()

        for (flock in locals.keys) {
            val number = java.lang.Long.parseUnsignedLong(flock, 16)

            for (peer in 1 until 0xff) {
                val id = Flock.id(flock, Calls.hex(peer))

                Vigil.chatIgnore(number, peer, id in ignored)
                Vigil.chatAccept(number, peer, vault.reachable || id in admits)
            }
        }

        synchronized(this) {
            val before = asking.size

            asking.removeAll(admits + ignored)

            if (vault.reachable) {
                asking.removeAll { it !in ignored }
            }

            if (asking.size != before) {
                changed()
            }
        }
    }

    private fun asked(vault: ChatVault, id: String): Boolean {
        if (vault.reachable) {
            return false
        }

        if (id in vault.admits()) {
            return false
        }

        if (id in vault.ignored()) {
            return false
        }

        synchronized(this) {
            return asking.add(id)
        }
    }

    private fun announce(service: VigilVpnService, chat: String, peer: String, text: String) {
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

    private fun sender(context: Context, peer: String, nick: String, signed: String): String {
        if (PeerNames.load(context).has(peer)) {
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
        service: VigilVpnService, id: String, peer: String, nick: String, signed: String, text: String,
        time: Long, topic: String
    ) {
        val name = sender(service, peer, nick, signed)
        val messages = shown.getOrPut(id) { mutableListOf() }

        messages.add(Shown(if (preview(service)) text else "New message", time, name, peer))
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

        return NOTIFICATION_BASE + (id.hashCode() and 0xffff)
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences("chats", Context.MODE_PRIVATE)

    const val ACTION_CHAT = "se.sanctorum.vigil.CHAT"
    const val EXTRA_CHAT = "chat"
    const val EXTRA_TOPIC = "topic"
    const val CHAT_LIST = "gospel"

    private const val TOPIC_NAME_MAX = 32
    private const val GROUP_NAME_MAX = 32
    private const val RESYNC_STEP = 2L * 1024 * 1024
    private const val RESYNC_MARGIN = 256L * 1024
    private const val TAG = "vigil"
    private const val CHANNEL = "messages"
    private const val NOTIFICATION_BASE = 0x20000
    private const val GROUP_NOTIFICATION_BASE = 0x10000
    private const val SHOWN_MAX = 6
    private const val SUMMARY = 0x0fff
    private const val GROUP = "se.sanctorum.vigil.chats"
    private const val RESUBMIT_MS = 10_000L
    private const val SWEEP_MS = 15_000L
    private const val SAFETY_WORDS = 5
}
