package se.sanctorum.vigil

import android.content.Context
import android.os.Build
import android.util.Base64
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import android.security.keystore.UserNotAuthenticatedException
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.security.InvalidKeyException
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.PublicKey
import java.security.SecureRandom
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.InvalidKeySpecException
import java.security.spec.X509EncodedKeySpec
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

class ChatVault(context: Context) {
    class Locked : Exception("Unlock your device to read your chats.")

    class Group(val owner: Int, val id: Long)

    class Lost(val chats: MutableSet<String>, val files: MutableMap<String, String>, var state: Boolean) {
        fun isEmpty(): Boolean = chats.isEmpty() && files.isEmpty() && !state
    }

    class Quote(val author: String, val hash: String) {
        val peer: Int
            get() = author.toInt(16)

        val digest: Long
            get() = java.lang.Long.parseUnsignedLong(hash, 16)
    }

    private class Conversation(
        val key: ByteArray,
        var count: Long,
        var read: Long,
        var flags: Long,
        var unread: Long = -1,
        var last: String? = null,
        var ambry: Int = 0,
        var topics: String? = null
    )

    private class History(val chat: String, val epoch: Long, val raw: MutableList<JSONObject>? = null) {
        var offset = 0L
        var seq = 0L
        var stamp = 0L
        var pinned = 0L
        var unread = 0L
        var damaged = false
        val messages = mutableListOf<JSONObject>()
        val stamps = mutableListOf<Long>()
        val index = mutableMapOf<Long, Int>()
        val digests = mutableMapOf<Int, String>()
        val topics = linkedMapOf<String, JSONObject>()
        val unreads = mutableMapOf<String, Long>()
        var reads = mapOf<String, Long>()
    }

    private val dir = File(context.noBackupFilesDir, "chats").apply { mkdirs() }
    private val inbox = File(dir, "inbox").apply { mkdirs() }
    private val avatars = File(dir, "avatars").apply { mkdirs() }
    private val names = File(dir, "names").apply { mkdirs() }
    private val safety = File(dir, "safety").apply { mkdirs() }
    private val random = SecureRandom()
    private val open = mutableMapOf<String, Conversation>()
    private val deferred = mutableListOf<Pair<JSONObject, ByteArray?>>()
    private val dumped = mutableListOf<JSONObject>()
    private var history: History? = null
    private var dumping: String? = null
    private var lost: Lost? = null
    private var inboxNext = 0L
    private var refs = 0L

    var mirror: ((JSONObject, ByteArray?) -> Boolean)? = null
    var linked: (() -> Boolean)? = null

    @Volatile
    var accepts = 0L
        private set

    @Volatile
    var reachable = false
        private set

    val hardware: Boolean
        get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S

    @Synchronized
    fun prepare() {
        history()
        outbox()
        agreement()

        dir.listFiles { f -> f.name.matches(Regex(OLD_GROUP)) }?.forEach { wipe(it) }
    }

    @Synchronized
    fun lock() {
        for (conversation in open.values) {
            conversation.key.fill(0)
        }
        open.clear()
        history = null
    }

    @Synchronized
    fun ref(): Long {
        refs = maxOf(refs + 1, System.currentTimeMillis() * 1000)
        return refs
    }

    @Synchronized
    fun deliver(
        chat: String, peer: Int, nick: String, text: String, time: Long, kind: String = "",
        data: ByteArray? = null, media: String = "", name: String = "", quote: Quote? = null,
        topic: String = "", color: Int = 0
    ) {
        if (kind == "ambry" && !rotated(chat, text)) {
            return
        }

        val message = JSONObject()
            .put("chat", chat)
            .put("peer", Calls.hex(peer))
            .put("nick", nick)
            .put("text", text)
            .put("time", time)
            .put("kind", kind)

        if (data != null) {
            message.put("media", media)
            message.put("name", name)
        }

        if (quote != null) {
            message.put("qa", quote.author)
            message.put("qh", quote.hash)
        }

        if (topic.isNotEmpty()) {
            message.put("tp", topic)
            message.put("color", color)
        }

        commit(chat, record(message, data), data)
    }

    @Synchronized
    fun echo(
        chat: String, ref: Long, text: String, time: Long, quote: Quote?, topic: String, to: List<String>,
        serial: Int, data: ByteArray? = null, media: String = "", name: String = ""
    ) {
        val message = JSONObject()
            .put("chat", chat)
            .put("peer", SELF)
            .put("dir", "out")
            .put("ref", ref)
            .put("text", text)
            .put("time", time)
        val entry = JSONObject().put("peer", chat).put("ref", ref).put("text", text)

        if (data != null) {
            message.put("media", media)
            message.put("name", name)
            entry.put("echo", true)
        }

        if (quote != null) {
            message.put("qa", quote.author).put("qh", quote.hash)
            entry.put("qa", quote.author).put("qh", quote.hash)
        }

        if (topic.isNotEmpty()) {
            message.put("tp", topic)
            entry.put("tp", topic)
        }

        val list = outboxList()

        if (group(chat) == null) {
            list.put(entry)
            accept(chat, true)
        } else {
            message.put("to", JSONArray(to))

            for (member in to) {
                list.put(
                    JSONObject(entry.toString()).put("peer", member).put("chat", chat)
                        .put("gs", serial).put("at", time)
                )
            }
        }

        commit(chat, record(message, data), data)
        outboxSave(list)
    }

    private fun record(message: JSONObject, data: ByteArray?): JSONObject {
        val chat = message.optString("chat", message.getString("peer"))
        val kind = message.optString("kind")
        val out = message.optString("dir") == "out"
        val ref = if (out) message.getLong("ref") else ref()

        if (kind == "retract") {
            val record = JSONObject()
                .put("t", "retract")
                .put("ref", ref)
                .put("peer", message.getString("peer"))
                .put("hash", message.getString("text"))

            if (message.has("tp")) {
                record.put("tp", message.getString("tp"))
            }

            return record
        }

        if (kind == "topic") {
            return JSONObject()
                .put("t", "topic")
                .put("ref", ref)
                .put("dir", if (out) "out" else "in")
                .put("tp", message.getString("tp"))
                .put("name", message.getString("text"))
                .put("color", message.optInt("color"))
                .put("time", message.getLong("time"))
        }

        val record = JSONObject()
            .put("t", if (kind.isEmpty()) "msg" else "member")
            .put("kind", kind)
            .put("dir", if (out) "out" else "in")
            .put("ref", ref)
            .put("peer", message.getString("peer"))
            .put("text", message.getString("text"))
            .put("nick", message.optString("nick"))
            .put("time", message.getLong("time"))

        if (message.has("qh")) {
            record.put("qa", message.getString("qa"))
            record.put("qh", message.getString("qh"))
        }

        if (message.has("tp")) {
            record.put("tp", message.getString("tp"))
        }

        if (message.has("to")) {
            record.put("to", message.getJSONArray("to"))
        }

        if (data != null) {
            described(record, chat, message.optString("media"), message.optString("name"), data)
        }

        return record
    }

    private fun described(record: JSONObject, chat: String, media: String, name: String, data: ByteArray) {
        record.put("data", "$chat-%016x.dat".format(record.getLong("ref")))
            .put("media", media)
            .put("name", name)
            .put("size", data.size)

        if (media == "image") {
            record.put("ch", "%016x".format(contentHash(data)))
        }
    }

    private fun commit(chat: String, record: JSONObject, data: ByteArray? = null) {
        val op = JSONObject().put("op", "rec").put("chat", chat).put("rec", record)

        enqueue(op, data, data?.takeIf { record.getLong("ref") and COMPANION_REF == 0L })
    }

    private fun enqueue(op: JSONObject, data: ByteArray? = null, mirrored: ByteArray? = null) {
        val entry = JSONObject(op.toString())

        if (data != null) {
            entry.put("data", Base64.encodeToString(data, Base64.NO_WRAP))
        }

        inboxWrite(entry)
        emit(op, mirrored)

        try {
            merge()
        } catch (e: Locked) {
            return
        }
    }

    @Synchronized
    fun emit(op: JSONObject, data: ByteArray? = null) {
        val chat = op.optString("chat")

        if (dumping != null && chat == dumping) {
            deferred.add(op to data?.copyOf())
            return
        }

        if (chat in lost().chats) {
            lose(op)
            return
        }

        if (mirror?.invoke(op, data) != true && linked?.invoke() == true) {
            lose(op)
        }
    }

    private fun lose(op: JSONObject) {
        val all = lost()
        val chat = op.optString("chat")

        when (op.getString("op")) {
            "rec", "read", "delete" -> {
                all.chats.add(chat)

                val file = op.optJSONObject("rec")?.optString("data") ?: ""

                if (file.isNotEmpty()) {
                    all.files[file] = chat
                }
            }
            "group", "name", "avatar", "prefs" -> all.state = true
            else -> return
        }

        lostSave()
    }

    private fun lost(): Lost {
        lost?.let { return it }

        val file = File(dir, LOST)
        val all = Lost(mutableSetOf(), mutableMapOf(), false)

        if (file.exists()) {
            val plain = unwrap(outbox(), LOST_AAD, file.readBytes())

            try {
                val json = JSONObject(String(plain))
                val chats = json.getJSONArray("chats")
                val files = json.getJSONObject("files")

                for (i in 0 until chats.length()) {
                    all.chats.add(chats.getString(i))
                }

                for (name in files.keys()) {
                    all.files[name] = files.getString(name)
                }

                all.state = json.getBoolean("state")
            } finally {
                plain.fill(0)
            }
        }

        lost = all

        return all
    }

    private fun lostSave() {
        val all = lost()
        val json = JSONObject()
            .put("chats", JSONArray(all.chats.toList()))
            .put("files", JSONObject(all.files.toMap()))
            .put("state", all.state)
        val plain = json.toString().toByteArray()

        try {
            write(File(dir, LOST), wrap(outbox(), LOST_AAD, plain))
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun lostSet(): Lost {
        val all = lost()

        return Lost(all.chats.toMutableSet(), all.files.toMutableMap(), all.state)
    }

    private fun inboxWrite(entry: JSONObject) {
        val plain = entry.toString().toByteArray()

        if (inboxNext == 0L) {
            inboxNext = (inbox.list()?.mapNotNull { it.removeSuffix(".box").toLongOrNull(16) }?.maxOrNull() ?: 0L) + 1
        }

        try {
            val static = agreement()?.certificate?.publicKey
            val sealed = if (static != null) seal(static, plain) else device(INBOX_AAD, plain)

            write(File(inbox, "%016x.box".format(inboxNext++)), sealed)
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun merge() {
        val pending = inbox.listFiles { f -> f.name.endsWith(".box") }
            ?.sortedWith(compareBy({ !it.name.contains('-') }, { it.name }))

        for (file in pending ?: emptyList()) {
            val plain = try {
                unseal(file.readBytes())
            } catch (e: AEADBadTagException) {
                wipe(file)
                continue
            } catch (e: IllegalArgumentException) {
                wipe(file)
                continue
            } catch (e: UserNotAuthenticatedException) {
                throw Locked()
            }

            try {
                val entry = JSONObject(String(plain))

                if (entry.has("op")) {
                    applyOp(entry)
                } else {
                    legacy(entry)
                }
            } catch (e: JSONException) {
                wipe(file)
                continue
            } catch (e: IllegalArgumentException) {
                wipe(file)
                continue
            } finally {
                plain.fill(0)
            }

            wipe(file)
        }

        val receipts = File(dir, RECEIPTS)
        if (!receipts.exists()) {
            return
        }

        val list = JSONArray(String(unwrap(outbox(), RECEIPTS_AAD, receipts.readBytes())))

        for (i in 0 until list.length()) {
            val receipt = list.getJSONObject(i)
            val record = JSONObject().put("t", receipt.getString("t")).put("ref", receipt.getLong("ref"))

            if (receipt.has("member")) {
                record.put("member", receipt.getString("member"))
            }

            applyOp(JSONObject().put("op", "rec").put("chat", receipt.getString("peer")).put("rec", record))
        }

        wipe(receipts)
    }

    private fun legacy(message: JSONObject) {
        val chat = message.optString("chat", message.getString("peer"))
        val encoded = message.optString("data")
        val data = if (encoded.isEmpty()) null else Base64.decode(encoded, Base64.NO_WRAP)

        if (message.optString("kind") == "ambry" && !rotated(chat, message.getString("text"))) {
            return
        }

        try {
            val op = JSONObject().put("op", "rec").put("chat", chat).put("rec", record(message, data))

            if (data != null) {
                op.put("data", encoded)
            }

            applyOp(op)
        } finally {
            data?.fill(0)
        }
    }

    private fun applyOp(op: JSONObject) {
        val chat = op.getString("chat")

        label(chat)

        when (op.getString("op")) {
            "rec" -> {
                val record = op.getJSONObject("rec")
                val type = record.getString("t")

                if (type !in CREATING && !File(dir, "$chat$KEY").exists()) {
                    return
                }

                val encoded = op.optString("data")

                if (encoded.isNotEmpty()) {
                    val data = Base64.decode(encoded, Base64.NO_WRAP)

                    try {
                        dataWrite(chat, record.getString("data"), data)
                    } finally {
                        data.fill(0)
                    }
                }

                append(chat, record)

                if (type == "retract") {
                    retracted(chat, record)
                } else if (type == "topic" && record.optString("name").isEmpty()) {
                    topicWipe(chat, record.getString("tp"))
                }
            }
            "delete" -> wipeChat(chat)
            "read" -> readMeta(chat, op.optString("topic"))
            else -> throw IllegalArgumentException("unknown inbox entry")
        }
    }

    class Head(val ops: List<Pair<JSONObject, ByteArray?>>, val chats: List<String>) {
        fun wipe() {
            for ((_, data) in ops) {
                data?.fill(0)
            }
        }
    }

    @Synchronized
    fun dumpHead(): Head {
        merge()

        val groups = groups()
        val names = peerNames()
        val faces = avatars.list()?.filter { it.matches(Regex("[0-9a-f]{2}\\.box")) }?.map { it.substring(0, 2) } ?: emptyList()
        val ops = mutableListOf<Pair<JSONObject, ByteArray?>>()

        for (chat in groups.keys()) {
            ops.add(JSONObject().put("op", "group").put("chat", chat).put("info", groups.getJSONObject(chat)) to null)
        }

        for (peer in names.keys()) {
            ops.add(JSONObject(names.getJSONObject(peer).toString()).put("op", "name").put("peer", peer) to null)
        }

        ops.add(JSONObject().put("op", "avatars").put("peers", JSONArray(faces)) to null)

        for (peer in faces) {
            val data = peerAvatar(peer.toInt(16)) ?: continue

            ops.add(JSONObject().put("op", "avatar").put("peer", peer) to data)
        }

        val chats = dir.list()?.filter { it.matches(Regex("($CHAT)\\$KEY")) }?.map { it.removeSuffix(KEY) } ?: emptyList()

        return Head(ops, chats)
    }

    @Synchronized
    fun dumpHead(head: Head): Boolean {
        for ((op, data) in head.ops) {
            if (!dumpSend(op, data)) {
                return false
            }
        }

        val all = lost()

        if (all.state) {
            all.state = false
            lostSave()
        }

        return true
    }

    class Dump(var budget: Int) {
        var at = 0
    }

    @Synchronized
    fun dumpChat(chat: String, dump: Dump, files: MutableList<String>): Boolean {
        if (dump.at == 0) {
            dumpEnd()
            merge()

            val conversation = conversation(chat, false)

            if (conversation == null) {
                if (!dumpSend(JSONObject().put("op", "delete").put("chat", chat))) {
                    return false
                }

                lostDone(chat)

                return true
            }

            val full = History(chat, 0, mutableListOf())

            load(chat, conversation, full)
            dumping = chat
            dumped.clear()
            dumped.add(JSONObject().put("op", "chat").put("chat", chat))

            val reads = topicMeta(conversation).mapValues { it.value.optLong("read", conversation.read) }

            for ((seq, record) in full.raw!!.withIndex()) {
                dumpReads(chat, seq.toLong(), conversation.read, reads)
                dumped.add(JSONObject().put("op", "rec").put("chat", chat).put("rec", dumpRecord(chat, record, files)))
            }

            dumpReads(chat, full.raw.size.toLong(), conversation.read, reads)
        }

        while (dump.at < dumped.size && dump.budget > 0) {
            val batch = JSONArray()
            var size = 0

            while (dump.at + batch.length() < dumped.size) {
                val op = dumped[dump.at + batch.length()]
                val len = op.toString().toByteArray().size

                if (batch.length() > 0 && size + len > BATCH_MAX) {
                    break
                }

                batch.put(op)
                size += len
            }

            if (!dumpSend(JSONObject().put("op", "batch").put("ops", batch))) {
                return false
            }

            dump.at += batch.length()
            dump.budget -= size
        }

        if (dump.at < dumped.size) {
            return false
        }

        lostDone(chat)
        dumpEnd()

        return true
    }

    @Synchronized
    fun dumpData(chat: String, file: String): Int {
        val data = try {
            data(chat, file)
        } catch (e: IllegalArgumentException) {
            null
        }

        if (data == null) {
            lostDone(file)
            return 0
        }

        try {
            if (!dumpSend(JSONObject().put("op", "data").put("chat", chat).put("file", file), data)) {
                return -1
            }

            lostDone(file)

            return data.size
        } finally {
            data.fill(0)
        }
    }

    private fun lostDone(name: String) {
        val all = lost()

        if (all.chats.remove(name) || all.files.remove(name) != null) {
            lostSave()
        }
    }

    @Synchronized
    fun dataSize(file: String): Long = File(dir, file).length()

    @Synchronized
    fun dumpEnd() {
        dumping = null
        dumped.clear()

        val pending = deferred.toList()

        deferred.clear()

        for ((op, data) in pending) {
            emit(op, data)
            data?.fill(0)
        }
    }

    private fun dumpReads(chat: String, seq: Long, read: Long, reads: Map<String, Long>) {
        if (seq == read) {
            dumped.add(JSONObject().put("op", "read").put("chat", chat))
        }

        for ((topic, at) in reads) {
            if (at == seq) {
                dumped.add(JSONObject().put("op", "read").put("chat", chat).put("topic", topic))
            }
        }
    }

    private fun dumpRecord(chat: String, record: JSONObject, files: MutableList<String>): JSONObject {
        val file = record.optString("data")

        if (file.isEmpty() || !File(dir, file).exists()) {
            return record
        }

        files.add(file)

        if (record.optString("media") == "image" && !record.has("ch")) {
            contentHash(chat, file)?.let { record.put("ch", "%016x".format(it)) }
        }

        return record
    }

    private fun dumpSend(op: JSONObject, data: ByteArray? = null): Boolean = mirror?.invoke(op, data) == true

    @Synchronized
    fun conversations(): JSONArray {
        val out = JSONArray()
        val files = dir.listFiles { f -> f.name.matches(Regex("($CHAT)\\$KEY")) } ?: return out

        for (file in files) {
            val id = file.name.removeSuffix(KEY)
            val conversation = conversation(id, false) ?: continue

            if (conversation.unread < 0 || conversation.topics == null) {
                val full = History(id, 0)

                load(id, conversation, full)
                summarize(id, conversation, full)
            }

            val entry = JSONObject()
                .put("peer", id)
                .put("unread", conversation.unread)
                .put("last", conversation.last?.let { JSONObject(it) } ?: JSONObject.NULL)
                .put("topics", JSONArray(conversation.topics ?: "[]"))

            if (group(id) != null) {
                entry.put("group", groupInfo(id) ?: JSONObject.NULL)
            }

            out.put(entry)
        }

        return out
    }

    @Synchronized
    fun messages(chat: String, epoch: Long = 0, stamp: Long = 0): JSONObject {
        val conversation = conversation(chat, false)
            ?: return JSONObject().put("messages", JSONArray()).put("unread", 0)

        val current = cached(chat)
        val fresh = current.offset == 0L

        load(chat, conversation, current)

        if (fresh && (conversation.unread < 0 || conversation.topics == null)) {
            summarize(chat, conversation, current)
        }

        val list = JSONArray()
        val topics = JSONArray()
        val meta = topicMeta(conversation)
        val out = JSONObject()
            .put("unread", conversation.unread)
            .put("epoch", current.epoch)
            .put("stamp", current.stamp)

        if (epoch == current.epoch) {
            for (i in current.messages.indices) {
                if (current.stamps[i] > stamp) {
                    list.put(gone(current.messages[i]))
                }
            }

            out.put("delta", true)
        } else {
            for (message in current.messages) {
                list.put(gone(message))
            }
        }

        for (topic in current.topics.values) {
            val unread = meta[topic.getString("id")]?.optLong("unread") ?: 0L

            topics.put(JSONObject(topic.toString()).put("unread", unread))
        }

        out.put("messages", list)
        out.put("topics", topics)

        if (group(chat) != null) {
            out.put("group", groupInfo(chat) ?: JSONObject.NULL)
        }

        if (current.damaged) {
            out.put("warning", "Part of this chat could not be read, it was damaged or tampered with.")
        } else if (conversation.flags and FLAG_TRUNCATED != 0L) {
            out.put("warning", "Messages were removed from the end of this chat outside of Vigil.")
        }

        return out
    }

    @Synchronized
    fun send(
        chat: String, text: String, ref: Long = ref(), quote: Quote? = null, topic: String = "", self: String = ""
    ): JSONArray {
        val time = System.currentTimeMillis()
        val record = JSONObject()
            .put("t", "msg")
            .put("dir", "out")
            .put("ref", ref)
            .put("text", text)
            .put("time", time)
        val entry = JSONObject().put("peer", chat).put("ref", ref).put("text", text)

        if (quote != null) {
            record.put("qa", quote.author).put("qh", quote.hash)
            entry.put("qa", quote.author).put("qh", quote.hash)
        }

        if (topic.isNotEmpty()) {
            record.put("tp", topic)
            entry.put("tp", topic)
        }

        return queue(chat, record, entry, self)
    }

    @Synchronized
    fun quoted(chat: String, ref: Long): Quote {
        val conversation = conversation(chat, false) ?: throw IllegalArgumentException("That message is gone.")
        val current = cached(chat)

        load(chat, conversation, current)

        val at = current.index[ref] ?: throw IllegalArgumentException("That message is gone.")
        val message = current.messages[at]

        if (message.getString("dir") == "out") {
            return Quote(SELF, digest(message))
        }

        return Quote(message.getString("peer"), digest(message))
    }

    @Synchronized
    fun sendData(
        chat: String, media: String, name: String, data: ByteArray, ref: Long = ref(), topic: String = "",
        self: String = ""
    ): JSONArray {
        val record = JSONObject()
            .put("t", "msg")
            .put("dir", "out")
            .put("ref", ref)
            .put("text", "")
            .put("time", System.currentTimeMillis())

        described(record, chat, media, name, data)

        val entry = JSONObject().put("peer", chat).put("ref", ref)
            .put("data", record.getString("data")).put("media", media).put("name", name)

        if (topic.isNotEmpty()) {
            record.put("tp", topic)
            entry.put("tp", topic)
        }

        return queue(chat, record, entry, self, data)
    }

    private fun queue(
        chat: String, record: JSONObject, entry: JSONObject, self: String, data: ByteArray? = null
    ): JSONArray {
        val list = outboxList()
        val queued = JSONArray()

        if (group(chat) == null) {
            commit(chat, record, data)
            list.put(entry)
            queued.put(entry)
            outboxSave(list)
            accept(chat, true)
            return queued
        }

        val info = groupInfo(chat) ?: throw IllegalArgumentException("There is no group $chat.")

        require(info.getString("state") == "open") { "This group is closed." }

        val to = JSONArray(members(info).filter { it != self })

        address(list, queued, entry, chat, info, self)
        commit(chat, record.put("to", to), data)
        outboxSave(list)

        return queued
    }

    @Synchronized
    fun topic(
        chat: String, topic: String, name: String, color: Int, ref: Long = ref(), self: String = ""
    ): JSONArray {
        val entry = JSONObject().put("peer", chat).put("ref", ref).put("op", "topic")
            .put("tp", topic).put("name", name).put("color", color)
        val list = outboxList()
        val queued = JSONArray()

        if (group(chat) == null) {
            topicOut(chat, ref, topic, name, color)
            list.put(entry)
            queued.put(entry)
            outboxSave(list)
            accept(chat, true)
            return queued
        }

        val info = groupInfo(chat) ?: throw IllegalArgumentException("There is no group $chat.")

        require(info.getString("owner") == self) { "Only the owner can change the topics of a group." }
        require(info.getString("state") == "open") { "This group is closed." }

        groupSave(chat, info.put("topics", topicsWith(topicsOf(chat, info), topic, name, color)))
        topicOut(chat, ref, topic, name, color)
        address(list, queued, entry, chat, info, self)

        outboxSave(list)

        return queued
    }

    private fun topicOut(chat: String, ref: Long, topic: String, name: String, color: Int) {
        val message = JSONObject()
            .put("chat", chat)
            .put("peer", SELF)
            .put("dir", "out")
            .put("ref", ref)
            .put("kind", "topic")
            .put("tp", topic)
            .put("text", name)
            .put("color", color)
            .put("time", System.currentTimeMillis())

        commit(chat, record(message, null))
    }

    @Synchronized
    fun topicQueue(chat: String, self: String, targets: Collection<String>): JSONArray {
        val info = groupInfo(chat) ?: return JSONArray()
        val topics = info.optJSONArray("topics") ?: return JSONArray()
        val list = outboxList()
        val queued = JSONArray()

        for (peer in targets.filter { it != self }) {
            for (i in 0 until topics.length()) {
                val topic = topics.getJSONObject(i)
                val entry = JSONObject()
                    .put("peer", peer)
                    .put("chat", chat)
                    .put("ref", ref())
                    .put("op", "topic")
                    .put("tp", topic.getString("id"))
                    .put("name", topic.getString("name"))
                    .put("color", topic.optInt("color"))
                    .put("gs", info.getInt("serial"))
                    .put("at", System.currentTimeMillis())

                list.put(entry)
                queued.put(entry)
            }
        }

        outboxSave(list)

        return queued
    }

    @Synchronized
    fun topicsOf(chat: String, info: JSONObject): JSONArray {
        info.optJSONArray("topics")?.let { return it }

        val out = JSONArray()
        val topics = messages(chat).optJSONArray("topics") ?: return out

        for (i in 0 until topics.length()) {
            val topic = topics.getJSONObject(i)

            out.put(
                JSONObject().put("id", topic.getString("id")).put("name", topic.getString("name"))
                    .put("color", topic.optInt("color"))
            )
        }

        return out
    }

    @Synchronized
    fun topicsFill(self: String) {
        val all = groups()

        for (chat in all.keys()) {
            val info = all.getJSONObject(chat)

            if (info.getString("owner") != self || info.has("topics")) {
                continue
            }

            groupSave(chat, info.put("topics", topicsOf(chat, info)))
        }
    }

    private fun topicsWith(topics: JSONArray, topic: String, name: String, color: Int): JSONArray {
        val out = JSONArray()

        for (i in 0 until topics.length()) {
            if (topics.getJSONObject(i).getString("id") != topic) {
                out.put(topics.getJSONObject(i))
            }
        }

        if (name.isNotEmpty()) {
            out.put(JSONObject().put("id", topic).put("name", name).put("color", color))
        }

        return out
    }

    @Synchronized
    fun groups(): JSONObject {
        val file = File(dir, GROUPS)
        if (!file.exists()) {
            return JSONObject()
        }

        val plain = unwrap(outbox(), GROUPS_AAD, file.readBytes())

        try {
            return JSONObject(String(plain))
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun groupInfo(chat: String): JSONObject? = groups().optJSONObject(chat)

    @Synchronized
    fun accepted(): Set<String> {
        val file = File(dir, ACCEPTED)
        if (!file.exists()) {
            return acceptedSave(acceptedFirst())
        }

        val plain = unwrap(outbox(), ACCEPTED_AAD, file.readBytes())

        try {
            val list = JSONArray(String(plain))
            return (0 until list.length()).map { list.getString(it) }.toSet()
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun accept(peer: String, on: Boolean) {
        val all = accepted().toMutableSet()
        val changed = if (on) all.add(peer) else all.remove(peer)

        if (changed) {
            acceptedSave(all)
        }
    }

    @Synchronized
    fun reach(on: Boolean) {
        if (!on && reachable) {
            acceptedSave(accepted() + acceptedFirst())
        }

        reachable = on
        accepts++
    }

    @Synchronized
    fun admits(): Set<String> {
        if (reachable) {
            return (1 until 0xff).map { "%02x".format(it) }.toSet() - ignored()
        }

        val out = accepted().toMutableSet()
        val all = groups()

        for (chat in all.keys()) {
            val info = all.getJSONObject(chat)

            out.add(info.getString("owner"))
            out.addAll(members(info))
        }

        val list = outboxList()

        for (i in 0 until list.length()) {
            out.add(list.getJSONObject(i).getString("peer"))
        }

        return out - ignored()
    }

    @Synchronized
    fun ignored(): Set<String> {
        val file = File(dir, IGNORED)
        if (!file.exists()) {
            return emptySet()
        }

        val plain = unwrap(outbox(), IGNORED_AAD, file.readBytes())

        try {
            val list = JSONArray(String(plain))
            return (0 until list.length()).map { list.getString(it) }.toSet()
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun ignore(peer: String, on: Boolean) {
        val all = ignored().toMutableSet()

        if (on) {
            accept(peer, false)
            all.add(peer)
        } else {
            all.remove(peer)
        }

        val plain = JSONArray(all.sorted()).toString().toByteArray()

        try {
            write(File(dir, IGNORED), wrap(outbox(), IGNORED_AAD, plain))
        } finally {
            plain.fill(0)
        }

        accepts++
    }

    private fun acceptedFirst(): Set<String> {
        val out = mutableSetOf<String>()
        val files = dir.listFiles { f -> f.name.matches(Regex("[0-9a-f]{2}\\$KEY")) } ?: emptyArray()
        val list = outboxList()

        for (file in files) {
            out.add(file.name.removeSuffix(KEY))
        }

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)

            if (!entry.has("chat")) {
                out.add(entry.getString("peer"))
            }
        }

        return out
    }

    private fun acceptedSave(peers: Set<String>): Set<String> {
        val plain = JSONArray(peers.sorted()).toString().toByteArray()

        try {
            write(File(dir, ACCEPTED), wrap(outbox(), ACCEPTED_AAD, plain))
        } finally {
            plain.fill(0)
        }

        accepts++
        return peers
    }

    @Synchronized
    fun groupGone(chat: String): Boolean {
        if (group(chat) == null) {
            return false
        }

        return groupInfo(chat)?.optString("state") != "open"
    }

    @Synchronized
    fun groupSave(chat: String, info: JSONObject?) {
        val all = groups()

        if (info == null) {
            all.remove(chat)
        } else {
            all.put(chat, info)
        }

        val plain = all.toString().toByteArray()

        try {
            write(File(dir, GROUPS), wrap(outbox(), GROUPS_AAD, plain))
        } finally {
            plain.fill(0)
        }

        accepts++

        emit(JSONObject().put("op", "group").put("chat", chat).put("info", info ?: JSONObject.NULL))
    }

    @Synchronized
    fun groupCheck(chat: String, peer: String, serial: Int): Int {
        val info = groupInfo(chat) ?: return GROUP_HOLD

        if (info.getString("state") != "open") {
            return GROUP_DROP
        }

        if (Integer.compareUnsigned(serial, info.getInt("serial")) > 0) {
            return GROUP_HOLD
        }

        if (peer == info.getString("owner") || members(info).contains(peer)) {
            return GROUP_STORE
        }

        return GROUP_DROP
    }

    @Synchronized
    fun groupDrop(chat: String, peers: Collection<String>, keep: Long = 0L): List<Pair<String, Long>> {
        val list = outboxList()
        val kept = JSONArray()
        val dropped = mutableListOf<Pair<String, Long>>()

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)

            if (entry.optString("chat") == chat && entry.getString("peer") in peers && entry.getLong("ref") != keep) {
                dropped.add(entry.getString("peer") to entry.getLong("ref"))
                continue
            }

            kept.put(entry)
        }

        if (dropped.isNotEmpty()) {
            outboxSave(kept)
        }

        return dropped
    }

    @Synchronized
    fun rosterQueue(
        chat: String, info: JSONObject, self: String, also: Collection<String> = emptyList(),
        only: Collection<String>? = null
    ): JSONArray {
        val list = outboxList()
        val kept = JSONArray()
        val queued = JSONArray()
        val targets = (only ?: (members(info) + also)).filter { it != self }.toSet()

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)

            if (entry.optString("chat") == chat && entry.optString("op") == "roster" &&
                entry.getString("peer") in targets
            ) {
                continue
            }

            kept.put(entry)
        }

        for (peer in targets) {
            val entry = JSONObject()
                .put("peer", peer)
                .put("chat", chat)
                .put("ref", ref())
                .put("op", "roster")
                .put("serial", info.getInt("serial"))
                .put("members", info.getJSONArray("members"))
                .put("name", info.getString("name"))
                .put("ended", info.getString("state") == "ended")
                .put("at", System.currentTimeMillis())

            kept.put(entry)
            queued.put(entry)
        }

        outboxSave(kept)

        return queued
    }

    @Synchronized
    fun leaveQueue(chat: String, owner: String): JSONObject {
        val entry = JSONObject().put("peer", owner).put("chat", chat).put("ref", ref()).put("op", "leave")
            .put("at", System.currentTimeMillis())
        val list = outboxList()

        list.put(entry)
        outboxSave(list)

        return entry
    }

    @Synchronized
    fun data(chat: String, file: String): ByteArray? {
        require(file.matches(Regex(DATA)) && file.startsWith("$chat-")) { "There is no data $file." }

        val conversation = conversation(chat, false) ?: return null
        val path = File(dir, file)
        if (!path.exists()) {
            return null
        }

        val sealed = path.readBytes()

        return Cipher.getInstance(GCM).run {
            init(Cipher.DECRYPT_MODE, SecretKeySpec(conversation.key, "AES"), GCMParameterSpec(TAG, sealed, 0, NONCE))
            updateAAD(dataAad(chat, file))
            doFinal(sealed, NONCE, sealed.size - NONCE)
        }
    }

    @Synchronized
    fun ownAvatar(): ByteArray? {
        val file = File(dir, AVATAR)
        if (!file.exists()) {
            return null
        }

        return unwrap(outbox(), AVATAR_AAD, file.readBytes())
    }

    @Synchronized
    fun ownAvatar(data: ByteArray?) {
        if (data == null) {
            wipe(File(dir, AVATAR))
            return
        }

        write(File(dir, AVATAR), wrap(outbox(), AVATAR_AAD, data))
    }

    @Synchronized
    fun peerAvatar(peer: Int): ByteArray? {
        val file = File(avatars, "%02x.box".format(peer))
        if (!file.exists()) {
            return null
        }

        return try {
            unseal(file.readBytes(), avatarAad(peer))
        } catch (e: AEADBadTagException) {
            wipe(file)
            null
        } catch (e: IllegalArgumentException) {
            wipe(file)
            null
        } catch (e: UserNotAuthenticatedException) {
            throw Locked()
        }
    }

    @Synchronized
    fun peerAvatar(peer: Int, data: ByteArray?) {
        val file = File(avatars, "%02x.box".format(peer))

        if (data == null) {
            wipe(file)
        } else {
            val aad = avatarAad(peer)
            val static = agreement()?.certificate?.publicKey

            write(file, if (static != null) seal(static, data, aad) else device(aad, data))
        }

        emit(JSONObject().put("op", "avatar").put("peer", Calls.hex(peer)), data)
    }

    @Synchronized
    fun safetyTime(peer: Int): Long = File(safety, "%02x.box".format(peer)).lastModified()

    @Synchronized
    fun safetyCode(peer: Int): ByteArray? {
        val file = File(safety, "%02x.box".format(peer))
        if (!file.exists()) {
            return null
        }

        val aad = safetyAad(peer)
        val data = file.readBytes()
        val key = try {
            unwrap(outbox(), aad, data.copyOfRange(0, WRAPPED))
        } catch (e: AEADBadTagException) {
            wipe(file)
            return null
        }

        return try {
            Cipher.getInstance(GCM).run {
                init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG, data, WRAPPED, NONCE))
                updateAAD(aad)
                doFinal(data, WRAPPED + NONCE, data.size - WRAPPED - NONCE)
            }
        } catch (e: AEADBadTagException) {
            wipe(file)
            null
        } finally {
            key.fill(0)
        }
    }

    @Synchronized
    fun safetyCode(peer: Int, plain: ByteArray?) {
        val file = File(safety, "%02x.box".format(peer))

        if (plain == null) {
            wipe(file)
            return
        }

        val aad = safetyAad(peer)
        val key = ByteArray(32).also(random::nextBytes)
        val nonce = ByteArray(NONCE).also(random::nextBytes)

        try {
            val sealed = Cipher.getInstance(GCM).run {
                init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG, nonce))
                updateAAD(aad)
                doFinal(plain)
            }

            write(file, wrap(outbox(), aad, key) + nonce + sealed)
        } finally {
            key.fill(0)
        }
    }

    @Synchronized
    fun safetyInputs(): Map<Int, ByteArray> {
        val out = mutableMapOf<Int, ByteArray>()
        val files = safety.listFiles { f -> f.name.matches(Regex("[0-9a-f]{2}\\.box")) } ?: emptyArray()

        for (file in files) {
            val peer = file.name.substring(0, 2).toInt(16)
            val plain = safetyCode(peer) ?: continue

            if (plain.size == INPUT) {
                out[peer] = plain
            } else {
                plain.fill(0)
            }
        }

        return out
    }

    @Synchronized
    fun peerName(peer: Int, nick: String, signed: String) {
        val aad = nameAad(peer)
        val name = JSONObject().put("nick", nick).put("signed", signed)
        val plain = name.toString().toByteArray()
        val static = agreement()?.certificate?.publicKey

        try {
            write(
                File(names, "%02x.box".format(peer)),
                if (static != null) seal(static, plain, aad) else device(aad, plain)
            )
        } finally {
            plain.fill(0)
        }

        emit(JSONObject(name.toString()).put("op", "name").put("peer", Calls.hex(peer)))
    }

    @Synchronized
    fun peerNames(): JSONObject {
        val out = JSONObject()
        val files = names.listFiles { f -> f.name.matches(Regex("[0-9a-f]{2}\\.box")) } ?: emptyArray()

        for (file in files) {
            val peer = file.name.substring(0, 2)
            val plain = try {
                unseal(file.readBytes(), nameAad(peer.toInt(16)))
            } catch (e: AEADBadTagException) {
                wipe(file)
                continue
            } catch (e: IllegalArgumentException) {
                wipe(file)
                continue
            } catch (e: UserNotAuthenticatedException) {
                throw Locked()
            }

            try {
                out.put(peer, JSONObject(String(plain)))
            } catch (e: JSONException) {
                wipe(file)
            } finally {
                plain.fill(0)
            }
        }

        return out
    }

    @Synchronized
    fun outboxList(): JSONArray {
        val file = File(dir, OUTBOX)
        if (!file.exists()) {
            return JSONArray()
        }

        val plain = unwrap(outbox(), OUTBOX_AAD, file.readBytes())

        try {
            return JSONArray(String(plain))
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun receipt(peer: String, ref: Long, kind: String) {
        val outbox = outboxList()
        val kept = JSONArray()
        var chat = peer

        for (i in 0 until outbox.length()) {
            val entry = outbox.getJSONObject(i)

            if (entry.getString("peer") != peer || entry.getLong("ref") != ref) {
                kept.put(entry)
                continue
            }

            chat = entry.optString("chat", peer)

            if (kind != "delivered") {
                kept.put(entry)
            }
        }

        if (kept.length() != outbox.length()) {
            outboxSave(kept)
        }

        receiptWrite(chat, ref, kind, if (chat != peer) peer else null)
    }

    @Synchronized
    fun expire(): List<Pair<String, Long>> {
        val now = System.currentTimeMillis()
        val list = outboxList()
        val kept = JSONArray()
        val dropped = mutableListOf<Pair<String, Long>>()
        var changed = false

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)
            val chat = entry.optString("chat")

            if (chat.isEmpty()) {
                kept.put(entry)
                continue
            }

            if (!entry.has("at")) {
                entry.put("at", now)
                changed = true
            }

            if (now - entry.getLong("at") < GROUP_EXPIRE) {
                kept.put(entry)
                continue
            }

            val peer = entry.getString("peer")
            val ref = entry.getLong("ref")

            dropped.add(peer to ref)
            changed = true

            when (entry.optString("op")) {
                "roster", "topic" -> groupStale(chat, peer, true)
                "" -> receiptWrite(chat, ref, "expired", peer)
            }
        }

        if (changed) {
            outboxSave(kept)
        }

        return dropped
    }

    @Synchronized
    fun groupStale(chat: String, peer: String, on: Boolean) {
        val info = groupInfo(chat) ?: return
        val stale = info.optJSONArray("stale") ?: JSONArray()
        val kept = JSONArray()

        for (i in 0 until stale.length()) {
            if (stale.getString(i) != peer) {
                kept.put(stale.getString(i))
            }
        }

        if (on) {
            kept.put(peer)
        }

        groupSave(chat, info.put("stale", kept))
    }

    private fun receiptWrite(chat: String, ref: Long, kind: String, member: String?) {
        val record = JSONObject().put("t", kind).put("ref", ref)

        if (member != null) {
            record.put("member", member)
        }

        commit(chat, record)
    }

    @Synchronized
    fun read(chat: String, topic: String = "") {
        val op = JSONObject().put("op", "read").put("chat", chat)

        if (topic.isNotEmpty()) {
            op.put("topic", topic)
        }

        enqueue(op)
    }

    private fun readMeta(chat: String, topic: String) {
        val conversation = conversation(chat, false) ?: return

        if (topic.isEmpty()) {
            conversation.read = conversation.count
            conversation.unread = 0
            save(chat, conversation)
            return
        }

        if (conversation.topics == null) {
            val full = History(chat, 0)

            load(chat, conversation, full)
            summarize(chat, conversation, full)
        }

        val list = JSONArray(conversation.topics ?: return)

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)

            if (entry.getString("id") == topic) {
                entry.put("read", conversation.count).put("unread", 0)
            }
        }

        conversation.topics = list.toString()
        save(chat, conversation)
    }

    @Synchronized
    fun favorite(chat: String, ref: Long, on: Boolean) {
        commit(chat, JSONObject().put("t", "fav").put("ref", ref).put("on", on))
    }

    @Synchronized
    fun pin(chat: String, ref: Long, on: Boolean) {
        commit(chat, JSONObject().put("t", "pin").put("ref", ref).put("on", on))
    }

    @Synchronized
    fun delete(chat: String) {
        enqueue(JSONObject().put("op", "delete").put("chat", chat))

        val list = outboxList()
        val kept = JSONArray()

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)
            val op = entry.optString("op")

            // The members still have to hear that we ended or left the group.
            if (op == "roster" || op == "leave") {
                kept.put(entry)
                continue
            }

            if (entry.getString("peer") != chat && entry.optString("chat") != chat) {
                kept.put(entry)
            }
        }

        if (kept.length() != list.length()) {
            outboxSave(kept)
        }

        if (group(chat) != null) {
            groupSave(chat, null)
        } else {
            accept(chat, false)
        }
    }

    private fun wipeChat(chat: String) {
        open.remove(chat)?.key?.fill(0)

        if (history?.chat == chat) {
            history = null
        }

        wipe(File(dir, "$chat$META"))
        wipe(File(dir, "$chat$KEY"))
        wipe(File(dir, "$chat$LOG"))

        dir.listFiles { f -> f.name.matches(Regex(DATA)) && f.name.startsWith("$chat-") }
            ?.forEach { wipe(it) }
    }

    private fun topicWipe(chat: String, topic: String) {
        val conversation = conversation(chat, false) ?: return
        val full = History(chat, 0)

        load(chat, conversation, full)

        for (message in full.messages) {
            if (message.optString("tp") != topic) {
                continue
            }

            val data = message.optString("data")

            if (data.isNotEmpty()) {
                wipe(File(dir, data))
            }
        }
    }

    private fun rotated(chat: String, generation: String): Boolean {
        val file = File(dir, AMBRY)
        val all = if (file.exists()) JSONObject(String(unwrap(outbox(), AMBRY_AAD, file.readBytes()))) else JSONObject()
        val before = all.optString(chat)

        if (before == generation) {
            return false
        }

        val plain = all.put(chat, generation).toString().toByteArray()

        try {
            write(file, wrap(outbox(), AMBRY_AAD, plain))
        } finally {
            plain.fill(0)
        }

        return before.isNotEmpty()
    }

    private fun cached(chat: String): History {
        val current = history

        if (current != null && current.chat == chat && File(dir, "$chat$LOG").length() >= current.offset) {
            return current
        }

        return History(chat, random.nextLong()).also { history = it }
    }

    private fun summarize(chat: String, conversation: Conversation, full: History) {
        val topics = JSONArray()

        for (topic in full.topics.values) {
            val id = topic.getString("id")
            val entry = JSONObject(topic.toString())
                .put("read", full.reads[id] ?: conversation.read)
                .put("unread", full.unreads[id] ?: 0L)

            full.messages.lastOrNull { it.optString("tp") == id }?.let { entry.put("last", it) }
            topics.put(entry)
        }

        conversation.unread = full.unread
        conversation.last = full.messages.lastOrNull { !it.has("tp") }?.toString()
        conversation.topics = topics.toString()
        save(chat, conversation)
    }

    private fun topicMeta(conversation: Conversation): Map<String, JSONObject> {
        val list = JSONArray(conversation.topics ?: "[]")
        val out = mutableMapOf<String, JSONObject>()

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)
            out[entry.getString("id")] = entry
        }

        return out
    }

    private fun topicsUpdate(topics: String, record: JSONObject, count: Long): String {
        val list = JSONArray(topics)
        val kept = JSONArray()
        val topic = record.getString("tp")
        var found: JSONObject? = null

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)

            if (entry.getString("id") == topic) {
                found = entry
            } else {
                kept.put(entry)
            }
        }

        if (record.optString("t") != "topic") {
            found?.put("last", message(record))

            if (record.optString("dir") == "in") {
                found?.let { it.put("unread", it.optLong("unread") + 1) }
            }

            return list.toString()
        }

        if (record.optString("name").isEmpty()) {
            return kept.toString()
        }

        val entry = JSONObject()
            .put("id", topic)
            .put("name", record.getString("name"))
            .put("color", record.optInt("color"))
            .put("read", found?.optLong("read") ?: count)
            .put("unread", found?.optLong("unread") ?: 0L)

        found?.optJSONObject("last")?.let { entry.put("last", it) }
        kept.put(entry)

        return kept.toString()
    }

    private fun load(chat: String, conversation: Conversation, current: History) {
        current.stamp++

        val file = File(dir, "$chat$LOG")
        val size = file.length()
        if (size <= current.offset) {
            return
        }

        val data = RandomAccessFile(file, "r").use {
            val buffer = ByteArray((size - current.offset).toInt())

            it.seek(current.offset)
            it.readFully(buffer)
            buffer
        }

        val key = SecretKeySpec(conversation.key, "AES")
        var offset = 0

        current.reads = topicMeta(conversation).mapValues { it.value.optLong("read", conversation.read) }

        while (offset + 4 <= data.size) {
            val len = ByteBuffer.wrap(data, offset, 4).int
            if (len < NONCE + TAG / 8 || offset + 4 + len > data.size) {
                break
            }

            val plain = try {
                Cipher.getInstance(GCM).run {
                    init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG, data, offset + 4, NONCE))
                    updateAAD(recordAad(chat, current.seq))
                    doFinal(data, offset + 4 + NONCE, len - NONCE)
                }
            } catch (e: AEADBadTagException) {
                current.damaged = true
                break
            }

            try {
                val record = JSONObject(String(plain))

                current.raw?.add(record)
                apply(current, record, conversation.read)
            } finally {
                plain.fill(0)
            }

            offset += 4 + len
            current.offset += 4 + len
            current.seq++
        }

        data.fill(0)
    }

    private fun apply(current: History, record: JSONObject, read: Long) {
        val ref = record.optLong("ref")

        when (record.optString("t")) {
            "msg" -> {
                val message = message(record)
                val topic = message.optString("tp")

                if (message.getString("dir") == "in") {
                    if (topic.isEmpty()) {
                        if (current.seq >= read) {
                            current.unread++
                        }
                    } else if (current.seq >= (current.reads[topic] ?: read)) {
                        current.unreads[topic] = (current.unreads[topic] ?: 0L) + 1
                    }
                }

                current.index[ref] = current.messages.size
                current.messages.add(message)
                current.stamps.add(current.stamp)

                if (message.has("qh")) {
                    quote(current, current.messages.size - 1)
                }
            }
            "member" -> {
                current.messages.add(message(record))
                current.stamps.add(current.stamp)

                if (record.optString("kind") == "left") {
                    gone(current, record.optString("peer"))
                }
            }
            "sent" -> change(current, ref) {
                if (it.getString("state") == "queued") {
                    it.put("state", "sent")
                }
            }
            "delivered" -> change(current, ref) {
                if (!record.has("member")) {
                    it.put("state", "delivered")
                    return@change
                }

                val got = it.optJSONArray("got") ?: JSONArray().also { list -> it.put("got", list) }
                val member = record.getString("member")

                if ((0 until got.length()).none { i -> got.getString(i) == member }) {
                    got.put(member)
                }

                tally(it)
            }

            "expired" -> change(current, ref) {
                unaddress(it, record.optString("member"))
            }
            "fav" -> change(current, ref) { it.put("fav", record.optBoolean("on")) }
            "topic" -> {
                val topic = record.getString("tp")
                val name = record.optString("name")

                if (name.isEmpty()) {
                    current.topics.remove(topic)
                } else {
                    current.topics[topic] = JSONObject()
                        .put("id", topic)
                        .put("name", name)
                        .put("color", record.optInt("color"))
                }
            }
            "pin" -> {
                val previous = current.pinned

                if (record.optBoolean("on")) {
                    current.pinned = ref
                } else if (current.pinned == ref) {
                    current.pinned = 0
                }

                if (previous != current.pinned) {
                    change(current, previous) { it.remove("pinned") }
                    change(current, current.pinned) { it.put("pinned", true) }
                }
            }
        }
    }

    private fun gone(current: History, member: String) {
        for (at in current.messages.indices) {
            if (unaddress(current.messages[at], member)) {
                current.stamps[at] = current.stamp
            }
        }
    }

    private fun unaddress(message: JSONObject, member: String): Boolean {
        val to = message.optJSONArray("to") ?: return false
        val got = message.optJSONArray("got") ?: JSONArray()

        if ((0 until got.length()).any { got.getString(it) == member }) {
            return false
        }

        val kept = JSONArray()

        for (i in 0 until to.length()) {
            if (to.getString(i) != member) {
                kept.put(to.getString(i))
            }
        }

        if (kept.length() == to.length()) {
            return false
        }

        message.put("to", kept)
        tally(message)

        return true
    }

    private fun tally(message: JSONObject) {
        val to = message.optJSONArray("to")?.length() ?: 0
        val acked = message.optJSONArray("got")?.length() ?: 0

        message.put("acked", acked)
        message.put("pending", maxOf(to - acked, 0))

        if (acked > 0 && to - acked <= 0) {
            message.put("state", "delivered")
        } else if (acked > 0) {
            message.put("state", "sent")
        } else if (to == 0) {
            message.put("state", "failed")
        }
    }

    private fun change(current: History, ref: Long, edit: (JSONObject) -> Unit) {
        val at = current.index[ref] ?: return

        edit(current.messages[at])
        current.stamps[at] = current.stamp
    }

    private fun message(record: JSONObject): JSONObject {
        if (record.optString("t") == "member") {
            return JSONObject()
                .put("ref", record.getLong("ref"))
                .put("dir", "in")
                .put("kind", record.getString("kind"))
                .put("text", record.optString("text"))
                .put("time", record.getLong("time"))
                .put("nick", record.optString("nick"))
                .put("peer", record.optString("peer"))
                .put("state", "received")
        }

        val incoming = record.optString("dir") == "in"
        val message = JSONObject()
            .put("ref", record.getLong("ref"))
            .put("dir", record.getString("dir"))
            .put("text", record.getString("text"))
            .put("time", record.getLong("time"))
            .put("nick", record.optString("nick"))
            .put("peer", record.optString("peer"))
            .put("data", record.optString("data"))
            .put("media", record.optString("media"))
            .put("name", record.optString("name"))
            .put("state", if (incoming) "received" else "queued")

        if (record.has("qh")) {
            message.put("qa", record.getString("qa"))
            message.put("qh", record.getString("qh"))
        }

        if (record.has("tp")) {
            message.put("tp", record.getString("tp"))
        }

        if (record.has("ch")) {
            message.put("ch", record.getString("ch"))
        }

        if (record.has("to")) {
            message.put("to", JSONArray(record.getJSONArray("to").toString()))
            tally(message)
        }

        return message
    }

    private fun quote(current: History, at: Int) {
        val message = current.messages[at]
        val hash = message.getString("qh")
        val author = message.getString("qa")

        message.put("quoted", true)

        for (i in at - 1 downTo 0) {
            val other = current.messages[i]

            if (other.has("kind")) {
                continue
            }

            if (other.optString("tp") != message.optString("tp")) {
                continue
            }

            if (!wrote(other, author)) {
                continue
            }

            if (current.digests.getOrPut(i) { digest(other) } == hash) {
                message.put("reply", other.getLong("ref"))
                break
            }
        }
    }

    private fun wrote(message: JSONObject, author: String): Boolean {
        if (author == SELF) {
            return message.getString("dir") == "out"
        }

        return message.getString("dir") == "in" && message.optString("peer") == author
    }

    private fun digest(message: JSONObject): String {
        val content = if (message.optString("data").isEmpty()) {
            message.getString("text")
        } else {
            message.optString("name")
        }

        val sum = MessageDigest.getInstance("SHA-256")
            .digest((message.optString("media") + "\n" + content).toByteArray())

        return sum.copyOf(8).joinToString("") { "%02x".format(it) }
    }

    class Retracted(val queued: JSONArray, val dropped: List<Pair<String, Long>>)

    @Synchronized
    fun retract(chat: String, ref: Long, self: String = ""): Retracted {
        val conversation = conversation(chat, false) ?: throw IllegalArgumentException("That image is gone.")
        val current = cached(chat)

        load(chat, conversation, current)

        val at = current.index[ref] ?: throw IllegalArgumentException("That image is gone.")
        val message = current.messages[at]
        val file = message.optString("data")

        require(message.getString("dir") == "out" && message.optString("media") == "image" && file.isNotEmpty()) {
            "Only your own images can be deleted."
        }

        val hash = message.optString("ch").ifEmpty { null }?.let { java.lang.Long.parseUnsignedLong(it, 16) }
            ?: contentHash(chat, file) ?: throw IllegalArgumentException("That image was already removed.")

        return retractHash(chat, ref, hash, message.optString("tp"), self)
    }

    @Synchronized
    fun retractHash(chat: String, ref: Long, hash: Long, topic: String, self: String = ""): Retracted {
        val record = JSONObject()
            .put("t", "retract")
            .put("ref", ref())
            .put("peer", SELF)
            .put("hash", "%016x".format(hash))
            .put("target", ref)

        if (topic.isNotEmpty()) {
            record.put("tp", topic)
        }

        commit(chat, record)

        return retractQueue(chat, ref, hash, topic, self)
    }

    private fun retractQueue(chat: String, ref: Long, hash: Long, topic: String, self: String): Retracted {
        val list = outboxList()
        val kept = JSONArray()
        val dropped = mutableListOf<Pair<String, Long>>()

        for (i in 0 until list.length()) {
            val entry = list.getJSONObject(i)

            if (entry.getLong("ref") == ref && entry.optString("chat", entry.getString("peer")) == chat) {
                dropped.add(entry.getString("peer") to ref)
                continue
            }

            kept.put(entry)
        }

        val entry = JSONObject().put("peer", chat).put("ref", ref()).put("op", "retract")
            .put("hash", "%016x".format(hash))
        val queued = JSONArray()

        if (topic.isNotEmpty()) {
            entry.put("tp", topic)
        }

        if (group(chat) == null) {
            kept.put(entry)
            queued.put(entry)
            outboxSave(kept)
            return Retracted(queued, dropped)
        }

        val info = groupInfo(chat)

        if (info != null && info.getString("state") == "open") {
            address(kept, queued, entry, chat, info, self)
        }

        outboxSave(kept)

        return Retracted(queued, dropped)
    }

    private fun address(
        list: JSONArray, queued: JSONArray, entry: JSONObject, chat: String, info: JSONObject, self: String
    ) {
        for (member in members(info)) {
            if (member == self) {
                continue
            }

            val copy = JSONObject(entry.toString()).put("peer", member).put("chat", chat)
                .put("gs", info.getInt("serial")).put("at", System.currentTimeMillis())

            list.put(copy)
            queued.put(copy)
        }
    }

    private fun gone(message: JSONObject): JSONObject {
        val file = message.optString("data")

        if (file.isNotEmpty() && !message.has("gone") && !File(dir, file).exists()) {
            message.put("gone", true)
        }

        return message
    }

    private fun retracted(chat: String, record: JSONObject) {
        val conversation = conversation(chat, false) ?: return
        val full = History(chat, 0)

        load(chat, conversation, full)

        val message = full.messages.getOrNull(retractTarget(chat, full.messages, record)) ?: return

        wipe(File(dir, message.getString("data")))
        history = null
    }

    private fun retractTarget(chat: String, messages: List<JSONObject>, record: JSONObject): Int {
        if (record.has("target")) {
            val ref = record.getLong("target")

            return messages.indexOfLast { it.getLong("ref") == ref && it.optString("dir") == "out" }
        }

        val want = java.lang.Long.parseUnsignedLong(record.getString("hash"), 16)

        for (at in messages.indices.reversed()) {
            val message = messages[at]
            val data = message.optString("data")

            if (data.isEmpty() || message.optString("media") != "image") {
                continue
            }

            if (message.optString("tp") != record.optString("tp")) {
                continue
            }

            if (!wrote(message, record.getString("peer"))) {
                continue
            }

            val have = message.optString("ch").ifEmpty { null }?.let { java.lang.Long.parseUnsignedLong(it, 16) }
                ?: contentHash(chat, data)

            if (have == want) {
                return at
            }
        }

        return -1
    }

    private fun contentHash(chat: String, file: String): Long? {
        val data = data(chat, file) ?: return null

        try {
            return contentHash(data)
        } finally {
            data.fill(0)
        }
    }

    private fun contentHash(data: ByteArray): Long {
        val sum = MessageDigest.getInstance("SHA-256").digest(data)
        val hash = ByteBuffer.wrap(sum, 0, 8).long

        if (hash == 0L) {
            return 1L
        }

        return hash
    }

    private fun dataWrite(chat: String, file: String, data: ByteArray) {
        require(file.matches(Regex(DATA)) && file.startsWith("$chat-")) { "There is no data $file." }

        val conversation = conversation(chat, true)!!
        val nonce = ByteArray(NONCE).also(random::nextBytes)

        val sealed = Cipher.getInstance(GCM).run {
            init(Cipher.ENCRYPT_MODE, SecretKeySpec(conversation.key, "AES"), GCMParameterSpec(TAG, nonce))
            updateAAD(dataAad(chat, file))
            doFinal(data)
        }

        write(File(dir, file), nonce + sealed)
    }

    private fun dataAad(chat: String, file: String): ByteArray = DATA_AAD + label(chat) + file.toByteArray()

    private fun outboxSave(list: JSONArray) {
        val plain = list.toString().toByteArray()

        try {
            write(File(dir, OUTBOX), wrap(outbox(), OUTBOX_AAD, plain))
        } finally {
            plain.fill(0)
        }

        accepts++
    }

    private fun append(chat: String, record: JSONObject) {
        val conversation = conversation(chat, true)!!
        val nonce = ByteArray(NONCE).also(random::nextBytes)
        val plain = record.toString().toByteArray()

        val sealed = try {
            Cipher.getInstance(GCM).run {
                init(Cipher.ENCRYPT_MODE, SecretKeySpec(conversation.key, "AES"), GCMParameterSpec(TAG, nonce))
                updateAAD(recordAad(chat, conversation.count))
                doFinal(plain)
            }
        } finally {
            plain.fill(0)
        }

        FileOutputStream(File(dir, "$chat$LOG"), true).use {
            it.write(ByteBuffer.allocate(4).putInt(NONCE + sealed.size).array())
            it.write(nonce)
            it.write(sealed)
            it.fd.sync()
        }

        val type = record.optString("t")
        val topics = conversation.topics

        if (record.has("tp") && type in CREATING) {
            if (topics != null) {
                conversation.topics = topicsUpdate(topics, record, conversation.count)
            }
        } else if (type == "msg" || type == "member") {
            conversation.last = message(record).toString()
        }

        if (type == "msg" && !record.has("tp") && record.optString("dir") == "in" && conversation.unread >= 0) {
            conversation.unread++
        }

        conversation.count++
        save(chat, conversation)
    }

    private fun conversation(chat: String, create: Boolean): Conversation? {
        open[chat]?.let { return it }

        val file = File(dir, "$chat$KEY")

        if (!file.exists()) {
            if (!create) {
                return null
            }

            val conversation = Conversation(ByteArray(32).also(random::nextBytes), 0, 0, 0, 0)
            try {
                save(chat, conversation)
            } catch (e: UserNotAuthenticatedException) {
                throw Locked()
            }

            val log = File(dir, "$chat$LOG")

            if (log.exists()) {
                check(log.delete()) { "the old history of $chat could not be removed" }
            }

            open[chat] = conversation

            return conversation
        }

        var plain = try {
            unwrap(history(), keyAad(chat), file.readBytes())
        } catch (e: UserNotAuthenticatedException) {
            throw Locked()
        }

        val meta = File(dir, "$chat$META")
        val merged = plain.size == 32 && meta.exists()

        if (merged) {
            val tail = try {
                unwrap(history(), metaAad(chat), meta.readBytes())
            } catch (e: UserNotAuthenticatedException) {
                plain.fill(0)
                throw Locked()
            }

            val joined = plain + tail

            plain.fill(0)
            tail.fill(0)
            plain = joined
        }

        val buffer = ByteBuffer.wrap(plain)
        val conversation = Conversation(
            ByteArray(32).also { buffer.get(it) }, buffer.long, buffer.long, buffer.long
        )

        if (buffer.remaining() >= 12) {
            conversation.unread = buffer.long

            val last = ByteArray(buffer.int).also { buffer.get(it) }

            if (last.isNotEmpty()) {
                conversation.last = String(last)
            }

            last.fill(0)

            if (buffer.remaining() >= 4) {
                conversation.ambry = buffer.int
            }

            if (buffer.remaining() >= 4) {
                val topics = ByteArray(buffer.int).also { buffer.get(it) }

                if (topics.isNotEmpty()) {
                    conversation.topics = String(topics)
                }

                topics.fill(0)
            }
        }

        plain.fill(0)

        val framed = framed(chat)

        if (framed != conversation.count) {
            if (framed < conversation.count) {
                conversation.flags = conversation.flags or FLAG_TRUNCATED
            }
            conversation.count = framed
            conversation.unread = -1
            conversation.last = null
            conversation.topics = null
            save(chat, conversation)
        }

        if (merged) {
            save(chat, conversation)
            wipe(meta)
        }

        open[chat] = conversation

        return conversation
    }

    private fun save(chat: String, conversation: Conversation) {
        val last = conversation.last?.toByteArray() ?: ByteArray(0)
        val topics = conversation.topics?.toByteArray() ?: ByteArray(0)
        val plain = ByteBuffer.allocate(76 + last.size + topics.size)
            .put(conversation.key)
            .putLong(conversation.count)
            .putLong(conversation.read)
            .putLong(conversation.flags)
            .putLong(conversation.unread)
            .putInt(last.size)
            .put(last)
            .putInt(conversation.ambry)
            .putInt(topics.size)
            .put(topics)
            .array()

        try {
            write(File(dir, "$chat$KEY"), wrap(history(), keyAad(chat), plain))
        } finally {
            plain.fill(0)
            last.fill(0)
            topics.fill(0)
        }
    }

    private fun framed(chat: String): Long {
        val file = File(dir, "$chat$LOG")
        if (!file.exists()) {
            return 0
        }

        val data = file.readBytes()
        var offset = 0
        var count = 0L

        while (offset + 4 <= data.size) {
            val len = ByteBuffer.wrap(data, offset, 4).int
            if (len < NONCE + TAG / 8 || offset + 4 + len > data.size) {
                break
            }

            offset += 4 + len
            count++
        }

        return count
    }

    private fun seal(static: PublicKey, plain: ByteArray, aad: ByteArray = INBOX_AAD): ByteArray {
        val ephemeral = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC).run {
            initialize(ECGenParameterSpec("secp256r1"))
            generateKeyPair()
        }

        val shared = KeyAgreement.getInstance("ECDH").run {
            init(ephemeral.private)
            doPhase(static, true)
            generateSecret()
        }

        val encoded = ephemeral.public.encoded
        val key = hkdf(shared, encoded + static.encoded, aad)
        val nonce = ByteArray(NONCE).also(random::nextBytes)
        shared.fill(0)

        val sealed = try {
            Cipher.getInstance(GCM).run {
                init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG, nonce))
                updateAAD(aad)
                doFinal(plain)
            }
        } finally {
            key.fill(0)
        }

        return ByteBuffer.allocate(3 + encoded.size + NONCE + sealed.size)
            .put(SEALED_HARDWARE)
            .putShort(encoded.size.toShort())
            .put(encoded)
            .put(nonce)
            .put(sealed)
            .array()
    }

    private fun unseal(data: ByteArray, aad: ByteArray = INBOX_AAD): ByteArray {
        if (data.isNotEmpty() && data[0] == SEALED_DEVICE) {
            return unwrap(outbox(), aad, data.copyOfRange(1, data.size))
        }

        require(data.size > 3 && data[0] == SEALED_HARDWARE) { "unknown inbox record" }

        val buffer = ByteBuffer.wrap(data, 1, data.size - 1)
        val length = buffer.short.toInt()
        require(length > 0 && length + NONCE <= buffer.remaining()) { "short inbox record" }

        val encoded = ByteArray(length).also { buffer.get(it) }
        val nonce = ByteArray(NONCE).also { buffer.get(it) }
        val sealed = ByteArray(buffer.remaining()).also { buffer.get(it) }

        val entry = agreement() ?: throw IllegalStateException("no inbox key")
        val ephemeral = try {
            KeyFactory.getInstance(KeyProperties.KEY_ALGORITHM_EC)
                .generatePublic(X509EncodedKeySpec(encoded)) as ECPublicKey
        } catch (e: InvalidKeySpecException) {
            throw IllegalArgumentException("bad inbox key")
        }

        val own = entry.certificate.publicKey as ECPublicKey
        require(ephemeral.params.order == own.params.order) { "bad inbox key" }

        val shared = KeyAgreement.getInstance("ECDH", KEYSTORE).run {
            keystore { init(entry.privateKey) }
            doPhase(ephemeral, true)
            generateSecret()
        }

        val key = hkdf(shared, encoded + entry.certificate.publicKey.encoded, aad)
        shared.fill(0)

        try {
            return Cipher.getInstance(GCM).run {
                init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG, nonce))
                updateAAD(aad)
                doFinal(sealed)
            }
        } finally {
            key.fill(0)
        }
    }

    private fun device(aad: ByteArray, plain: ByteArray): ByteArray =
        byteArrayOf(SEALED_DEVICE) + wrap(outbox(), aad, plain)

    private fun keystore(init: () -> Unit) {
        try {
            init()
        } catch (e: KeyPermanentlyInvalidatedException) {
            throw e
        } catch (e: InvalidKeyException) {
            throw Locked()
        }
    }

    private fun wrap(key: SecretKey, aad: ByteArray, plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(GCM)

        keystore { cipher.init(Cipher.ENCRYPT_MODE, key) }
        cipher.updateAAD(aad)

        return cipher.iv + cipher.doFinal(plain)
    }

    private fun unwrap(key: SecretKey, aad: ByteArray, sealed: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(GCM)

        keystore { cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG, sealed, 0, NONCE)) }
        cipher.updateAAD(aad)

        return cipher.doFinal(sealed, NONCE, sealed.size - NONCE)
    }

    private fun hkdf(secret: ByteArray, salt: ByteArray, info: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")

        mac.init(SecretKeySpec(salt, "HmacSHA256"))
        val prk = mac.doFinal(secret)

        mac.init(SecretKeySpec(prk, "HmacSHA256"))
        mac.update(info)
        mac.update(1.toByte())
        prk.fill(0)

        return mac.doFinal()
    }

    private fun history(): SecretKey = secret(HISTORY, true)

    private fun outbox(): SecretKey = secret(OUTBOX_KEY, false)

    private fun secret(alias: String, unlocked: Boolean): SecretKey {
        val store = KeyStore.getInstance(KEYSTORE).apply { load(null) }

        (store.getEntry(alias, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }

        fun generate(strongbox: Boolean): SecretKey =
            KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE).run {
                init(
                    KeyGenParameterSpec.Builder(
                        alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
                    )
                        .setKeySize(256)
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setUnlockedDeviceRequired(unlocked)
                        .setIsStrongBoxBacked(strongbox)
                        .build()
                )
                generateKey()
            }

        return try {
            generate(true)
        } catch (e: StrongBoxUnavailableException) {
            generate(false)
        }
    }

    private fun agreement(): KeyStore.PrivateKeyEntry? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return null
        }

        val store = KeyStore.getInstance(KEYSTORE).apply { load(null) }

        (store.getEntry(INBOX_KEY, null) as? KeyStore.PrivateKeyEntry)?.let { return it }

        fun generate(strongbox: Boolean) {
            KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, KEYSTORE).run {
                initialize(
                    KeyGenParameterSpec.Builder(INBOX_KEY, KeyProperties.PURPOSE_AGREE_KEY)
                        .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
                        .setUnlockedDeviceRequired(true)
                        .setIsStrongBoxBacked(strongbox)
                        .build()
                )
                generateKeyPair()
            }
        }

        try {
            generate(true)
        } catch (e: StrongBoxUnavailableException) {
            generate(false)
        }

        return store.getEntry(INBOX_KEY, null) as KeyStore.PrivateKeyEntry
    }

    private fun write(file: File, data: ByteArray) {
        val temp = File(file.parentFile, file.name + ".tmp")

        FileOutputStream(temp).use {
            it.write(data)
            it.fd.sync()
        }

        if (!temp.renameTo(file)) {
            throw IllegalStateException("failed to write ${file.name}")
        }
    }

    private fun wipe(file: File) {
        if (!file.exists()) {
            return
        }

        FileOutputStream(file).use {
            it.write(ByteArray(file.length().toInt()))
            it.fd.sync()
        }

        check(file.delete()) { "${file.name} could not be removed" }
    }

    private fun keyAad(chat: String): ByteArray = KEY_AAD + label(chat)

    private fun metaAad(chat: String): ByteArray = META_AAD + label(chat)

    private fun avatarAad(peer: Int): ByteArray = AVATAR_AAD + byteArrayOf(peer.toByte())

    private fun nameAad(peer: Int): ByteArray = NAME_AAD + byteArrayOf(peer.toByte())

    private fun safetyAad(peer: Int): ByteArray = SAFETY_AAD + byteArrayOf(peer.toByte())

    private fun recordAad(chat: String, seq: Long): ByteArray {
        val label = label(chat)

        return ByteBuffer.allocate(RECORD_AAD.size + label.size + 8)
            .put(RECORD_AAD)
            .put(label)
            .putLong(seq)
            .array()
    }

    private fun label(chat: String): ByteArray {
        require(chat.matches(Regex(CHAT))) { "invalid chat $chat" }

        return if (group(chat) != null) chat.toByteArray() else byteArrayOf(chat.toInt(16).toByte())
    }

    companion object {
        fun group(chat: String): Group? {
            if (!chat.matches(Regex(GROUP_CHAT))) {
                return null
            }

            return Group(chat.substring(1, 3).toInt(16), java.lang.Long.parseUnsignedLong(chat.substring(3), 16))
        }

        fun groupChat(owner: String, group: String): String = "g$owner$group"

        fun members(info: JSONObject): List<String> {
            val list = info.optJSONArray("members") ?: return emptyList()

            return (0 until list.length()).map { list.getString(it) }
        }

        fun roster(members: Collection<String>): ByteArray {
            val bits = ByteArray(ROSTER)

            for (member in members) {
                val peer = member.toInt(16)
                bits[peer / 8] = (bits[peer / 8].toInt() or (1 shl (peer % 8))).toByte()
            }

            return bits
        }

        fun quote(author: String, hash: String): Quote? {
            if (hash.isEmpty()) {
                return null
            }

            return Quote(author, hash)
        }

        fun topicId(topic: String): Long {
            if (topic.isEmpty()) {
                return 0L
            }

            return java.lang.Long.parseUnsignedLong(topic, 16)
        }

        fun kind(media: String): Int =
            when (media) {
                "image" -> DATA_IMAGE
                "voice" -> DATA_VOICE
                else -> DATA_FILE
            }

        const val DATA_IMAGE = 1
        const val DATA_FILE = 2
        const val DATA_VOICE = 3

        const val SELF = "00"
        const val COMPANION_REF = 1L shl 62

        const val GROUP_STORE = 0
        const val GROUP_DROP = 1
        const val GROUP_HOLD = 2

        private const val GROUP_CHAT = "g[0-9a-f]{18}"
        private const val CHAT = "[0-9a-f]{2}|$GROUP_CHAT"
        private const val DATA = "(?:[0-9a-f]{2}|$GROUP_CHAT)-[0-9a-f]{16}\\.dat"
        private const val OLD_GROUP = "g[0-9a-f]{4}(?:\\.key|\\.meta|\\.log|-[0-9a-f]{16}\\.dat)"
        private const val KEYSTORE = "AndroidKeyStore"
        private const val HISTORY = "se.sanctorum.vigil.chat.history"
        private const val OUTBOX_KEY = "se.sanctorum.vigil.chat.outbox"
        private const val INBOX_KEY = "se.sanctorum.vigil.chat.inbox"
        private const val GCM = "AES/GCM/NoPadding"
        private const val NONCE = 12
        private const val TAG = 128
        private const val KEY = ".key"
        private const val META = ".meta"
        private const val LOG = ".log"
        private const val OUTBOX = "outbox.box"
        private const val RECEIPTS = "receipts.box"
        private const val GROUPS = "groups.box"
        private const val ACCEPTED = "accepted.box"
        private const val IGNORED = "ignored.box"
        private const val ROSTER = 32
        private const val BATCH_MAX = 12 * 1024
        private const val GROUP_EXPIRE = 3 * 24 * 60 * 60 * 1000L
        private const val AVATAR = "avatar.box"
        private const val AMBRY = "ambry.box"
        private const val LOST = "lost.box"
        private const val WRAPPED = NONCE + 32 + 16
        const val INPUT = 32
        private val CREATING = setOf("msg", "member", "topic")
        private const val FLAG_TRUNCATED = 1L
        private const val SEALED_HARDWARE: Byte = 1
        private const val SEALED_DEVICE: Byte = 2

        private val KEY_AAD = "VIGIL.CHAT.KEY".toByteArray()
        private val META_AAD = "VIGIL.CHAT.META".toByteArray()
        private val RECORD_AAD = "VIGIL.CHAT.V1".toByteArray()
        private val INBOX_AAD = "VIGIL.CHAT.INBOX".toByteArray()
        private val DATA_AAD = "VIGIL.CHAT.DATA".toByteArray()
        private val OUTBOX_AAD = "VIGIL.CHAT.OUTBOX".toByteArray()
        private val RECEIPTS_AAD = "VIGIL.CHAT.RECEIPTS".toByteArray()
        private val GROUPS_AAD = "VIGIL.CHAT.GROUPS".toByteArray()
        private val ACCEPTED_AAD = "VIGIL.CHAT.ACCEPTED".toByteArray()
        private val IGNORED_AAD = "VIGIL.CHAT.IGNORED".toByteArray()
        private val AVATAR_AAD = "VIGIL.CHAT.AVATAR".toByteArray()
        private val NAME_AAD = "VIGIL.CHAT.NAME".toByteArray()
        private val AMBRY_AAD = "VIGIL.CHAT.AMBRY".toByteArray()
        private val LOST_AAD = "VIGIL.CHAT.LOST".toByteArray()
        private val SAFETY_AAD = "VIGIL.CHAT.SAFETY".toByteArray()
    }
}
