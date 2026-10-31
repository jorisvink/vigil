package se.sanctorum.vigil

import android.content.Context
import android.os.Build
import android.util.Base64
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import android.security.keystore.UserNotAuthenticatedException
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.PublicKey
import java.security.SecureRandom
import java.security.spec.ECGenParameterSpec
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
        var last: String? = null
    )

    private class History(val chat: String, val epoch: Long) {
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
    }

    private val dir = File(context.noBackupFilesDir, "chats").apply { mkdirs() }
    private val inbox = File(dir, "inbox").apply { mkdirs() }
    private val random = SecureRandom()
    private val open = mutableMapOf<String, Conversation>()
    private var history: History? = null
    private var refs = 0L

    val hardware: Boolean
        get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S

    @Synchronized
    fun prepare() {
        history()
        outbox()
        agreement()
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
        data: ByteArray? = null, media: String = "", name: String = "", quote: Quote? = null
    ) {
        val message = JSONObject()
            .put("chat", chat)
            .put("peer", Calls.hex(peer))
            .put("nick", nick)
            .put("text", text)
            .put("time", time)
            .put("kind", kind)

        if (data != null) {
            message.put("data", Base64.encodeToString(data, Base64.NO_WRAP))
            message.put("media", media)
            message.put("name", name)
        }

        if (quote != null) {
            message.put("qa", quote.author)
            message.put("qh", quote.hash)
        }

        val plain = message.toString().toByteArray()

        try {
            val static = agreement()?.certificate?.publicKey
            val sealed = if (static != null) seal(static, plain) else device(INBOX_AAD, plain)

            write(File(inbox, "%016x-%016x.box".format(time, random.nextLong())), sealed)
        } finally {
            plain.fill(0)
        }
    }

    @Synchronized
    fun merge() {
        val pending = inbox.listFiles { f -> f.name.endsWith(".box") }?.sortedBy { it.name }

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
                val message = JSONObject(String(plain))
                val chat = message.optString("chat", message.getString("peer"))

                val kind = message.optString("kind")
                val ref = ref()
                val record = JSONObject()
                    .put("t", if (kind.isEmpty()) "msg" else "member")
                    .put("kind", kind)
                    .put("dir", "in")
                    .put("ref", ref)
                    .put("peer", message.getString("peer"))
                    .put("text", message.getString("text"))
                    .put("nick", message.optString("nick"))
                    .put("time", message.getLong("time"))

                if (message.has("qh")) {
                    record.put("qa", message.getString("qa"))
                    record.put("qh", message.getString("qh"))
                }

                val encoded = message.optString("data")

                if (encoded.isNotEmpty()) {
                    val data = Base64.decode(encoded, Base64.NO_WRAP)

                    try {
                        record.put("data", dataWrite(chat, ref, data))
                        record.put("media", message.optString("media"))
                        record.put("name", message.optString("name"))
                    } finally {
                        data.fill(0)
                    }
                }

                append(chat, record)
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
            val chat = receipt.getString("peer")

            if (File(dir, "$chat$KEY").exists()) {
                append(
                    chat,
                    JSONObject()
                        .put("t", receipt.getString("t"))
                        .put("ref", receipt.getLong("ref"))
                        .put("acked", receipt.optInt("acked"))
                        .put("pending", receipt.optInt("pending"))
                )
            }
        }

        wipe(receipts)
    }

    @Synchronized
    fun conversations(): JSONArray {
        val out = JSONArray()
        val files = dir.listFiles { f -> f.name.matches(Regex("($CHAT)\\$KEY")) } ?: return out

        for (file in files) {
            val id = file.name.removeSuffix(KEY)
            val conversation = conversation(id, false) ?: continue

            if (conversation.unread < 0) {
                val full = History(id, 0)

                load(id, conversation, full)
                summarize(id, conversation, full)
            }

            out.put(
                JSONObject()
                    .put("peer", id)
                    .put("unread", conversation.unread)
                    .put("last", conversation.last?.let { JSONObject(it) } ?: JSONObject.NULL)
            )
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

        if (fresh && conversation.unread < 0) {
            summarize(chat, conversation, current)
        }

        val list = JSONArray()
        val out = JSONObject()
            .put("unread", conversation.unread)
            .put("epoch", current.epoch)
            .put("stamp", current.stamp)

        if (epoch == current.epoch) {
            for (i in current.messages.indices) {
                if (current.stamps[i] > stamp) {
                    list.put(current.messages[i])
                }
            }

            out.put("delta", true)
        } else {
            for (message in current.messages) {
                list.put(message)
            }
        }

        out.put("messages", list)

        if (current.damaged) {
            out.put("warning", "Part of this chat could not be read, it was damaged or tampered with.")
        } else if (conversation.flags and FLAG_TRUNCATED != 0L) {
            out.put("warning", "Messages were removed from the end of this chat outside of Vigil.")
        }

        return out
    }

    @Synchronized
    fun send(chat: String, text: String, ref: Long = ref(), quote: Quote? = null): Long {
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

        append(chat, record)

        if (group(chat) != null) {
            return ref
        }

        val list = outboxList()
        list.put(entry)
        outboxSave(list)

        return ref
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
    fun sendData(chat: String, media: String, name: String, data: ByteArray, ref: Long = ref()): Long {
        val file = dataWrite(chat, ref, data)

        append(
            chat,
            JSONObject()
                .put("t", "msg")
                .put("dir", "out")
                .put("ref", ref)
                .put("text", "")
                .put("data", file)
                .put("media", media)
                .put("name", name)
                .put("time", System.currentTimeMillis())
        )

        if (group(chat) != null) {
            return ref
        }

        val list = outboxList()
        list.put(
            JSONObject().put("peer", chat).put("ref", ref)
                .put("data", file).put("media", media).put("name", name)
        )
        outboxSave(list)

        return ref
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
    fun receipt(chat: String, ref: Long, kind: String, acked: Int = 0, pending: Int = 0) {
        if (kind == "delivered") {
            val list = outboxList()
            val kept = JSONArray()

            for (i in 0 until list.length()) {
                if (list.getJSONObject(i).getLong("ref") != ref) {
                    kept.put(list.getJSONObject(i))
                }
            }

            if (kept.length() != list.length()) {
                outboxSave(kept)
            }
        }

        val file = File(dir, RECEIPTS)
        val list = if (file.exists()) {
            JSONArray(String(unwrap(outbox(), RECEIPTS_AAD, file.readBytes())))
        } else {
            JSONArray()
        }

        list.put(
            JSONObject().put("peer", chat).put("ref", ref).put("t", kind)
                .put("acked", acked).put("pending", pending)
        )
        write(file, wrap(outbox(), RECEIPTS_AAD, list.toString().toByteArray()))
    }

    @Synchronized
    fun read(chat: String) {
        val conversation = conversation(chat, false) ?: return

        conversation.read = conversation.count
        conversation.unread = 0
        save(chat, conversation)
    }

    @Synchronized
    fun favorite(chat: String, ref: Long, on: Boolean) {
        conversation(chat, false) ?: return

        append(chat, JSONObject().put("t", "fav").put("ref", ref).put("on", on))
    }

    @Synchronized
    fun pin(chat: String, ref: Long, on: Boolean) {
        conversation(chat, false) ?: return

        append(chat, JSONObject().put("t", "pin").put("ref", ref).put("on", on))
    }

    @Synchronized
    fun delete(chat: String) {
        open.remove(chat)?.key?.fill(0)

        if (history?.chat == chat) {
            history = null
        }

        wipe(File(dir, "$chat$KEY"))
        wipe(File(dir, "$chat$LOG"))

        dir.listFiles { f -> f.name.matches(Regex(DATA)) && f.name.startsWith("$chat-") }
            ?.forEach { wipe(it) }

        val list = outboxList()
        val kept = JSONArray()

        for (i in 0 until list.length()) {
            if (list.getJSONObject(i).getString("peer") != chat) {
                kept.put(list.getJSONObject(i))
            }
        }

        if (kept.length() != list.length()) {
            outboxSave(kept)
        }
    }

    private fun cached(chat: String): History {
        val current = history

        if (current != null && current.chat == chat && File(dir, "$chat$LOG").length() >= current.offset) {
            return current
        }

        return History(chat, random.nextLong()).also { history = it }
    }

    private fun summarize(chat: String, conversation: Conversation, full: History) {
        conversation.unread = full.unread
        conversation.last = full.messages.lastOrNull()?.toString()
        save(chat, conversation)
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
                apply(current, JSONObject(String(plain)), conversation.read)
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

                if (message.getString("dir") == "in" && current.seq >= read) {
                    current.unread++
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
            }
            "sent" -> change(current, ref) {
                if (it.getString("state") == "queued") {
                    it.put("state", "sent")
                }
            }
            "delivered" -> change(current, ref) { it.put("state", "delivered") }
            "acks" -> change(current, ref) {
                val pending = record.optInt("pending")

                it.put("acked", record.optInt("acked"))
                it.put("pending", pending)
                it.put("state", if (pending == 0) "delivered" else "sent")
            }
            "fav" -> change(current, ref) { it.put("fav", record.optBoolean("on")) }
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
                .put("text", "")
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

    private fun dataWrite(chat: String, ref: Long, data: ByteArray): String {
        val conversation = conversation(chat, true)!!
        val file = "$chat-%016x.dat".format(ref)
        val nonce = ByteArray(NONCE).also(random::nextBytes)

        val sealed = Cipher.getInstance(GCM).run {
            init(Cipher.ENCRYPT_MODE, SecretKeySpec(conversation.key, "AES"), GCMParameterSpec(TAG, nonce))
            updateAAD(dataAad(chat, file))
            doFinal(data)
        }

        write(File(dir, file), nonce + sealed)

        return file
    }

    private fun dataAad(chat: String, file: String): ByteArray = DATA_AAD + label(chat) + file.toByteArray()

    private fun outboxSave(list: JSONArray) {
        val plain = list.toString().toByteArray()

        try {
            write(File(dir, OUTBOX), wrap(outbox(), OUTBOX_AAD, plain))
        } finally {
            plain.fill(0)
        }
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

        if (type == "msg" || type == "member") {
            conversation.last = message(record).toString()
        }

        if (type == "msg" && record.optString("dir") == "in" && conversation.unread >= 0) {
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

        val plain = try {
            unwrap(history(), keyAad(chat), file.readBytes())
        } catch (e: UserNotAuthenticatedException) {
            throw Locked()
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
            save(chat, conversation)
        }

        open[chat] = conversation

        return conversation
    }

    private fun save(chat: String, conversation: Conversation) {
        val last = conversation.last?.toByteArray() ?: ByteArray(0)
        val plain = ByteBuffer.allocate(68 + last.size)
            .put(conversation.key)
            .putLong(conversation.count)
            .putLong(conversation.read)
            .putLong(conversation.flags)
            .putLong(conversation.unread)
            .putInt(last.size)
            .put(last)
            .array()

        try {
            write(File(dir, "$chat$KEY"), wrap(history(), keyAad(chat), plain))
        } finally {
            plain.fill(0)
            last.fill(0)
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

    private fun seal(static: PublicKey, plain: ByteArray): ByteArray {
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
        val key = hkdf(shared, encoded + static.encoded, INBOX_AAD)
        val nonce = ByteArray(NONCE).also(random::nextBytes)
        shared.fill(0)

        val sealed = try {
            Cipher.getInstance(GCM).run {
                init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG, nonce))
                updateAAD(INBOX_AAD)
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

    private fun unseal(data: ByteArray): ByteArray {
        if (data.isNotEmpty() && data[0] == SEALED_DEVICE) {
            return unwrap(outbox(), INBOX_AAD, data.copyOfRange(1, data.size))
        }

        require(data.size > 3 && data[0] == SEALED_HARDWARE) { "unknown inbox record" }

        val buffer = ByteBuffer.wrap(data, 1, data.size - 1)
        val encoded = ByteArray(buffer.short.toInt()).also { buffer.get(it) }
        val nonce = ByteArray(NONCE).also { buffer.get(it) }
        val sealed = ByteArray(buffer.remaining()).also { buffer.get(it) }

        val entry = agreement() ?: throw IllegalStateException("no inbox key")
        val ephemeral = KeyFactory.getInstance(KeyProperties.KEY_ALGORITHM_EC)
            .generatePublic(X509EncodedKeySpec(encoded))

        val shared = KeyAgreement.getInstance("ECDH", KEYSTORE).run {
            init(entry.privateKey)
            doPhase(ephemeral, true)
            generateSecret()
        }

        val key = hkdf(shared, encoded + entry.certificate.publicKey.encoded, INBOX_AAD)
        shared.fill(0)

        try {
            return Cipher.getInstance(GCM).run {
                init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG, nonce))
                updateAAD(INBOX_AAD)
                doFinal(sealed)
            }
        } finally {
            key.fill(0)
        }
    }

    private fun device(aad: ByteArray, plain: ByteArray): ByteArray =
        byteArrayOf(SEALED_DEVICE) + wrap(outbox(), aad, plain)

    private fun wrap(key: SecretKey, aad: ByteArray, plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(GCM)

        cipher.init(Cipher.ENCRYPT_MODE, key)
        cipher.updateAAD(aad)

        return cipher.iv + cipher.doFinal(plain)
    }

    private fun unwrap(key: SecretKey, aad: ByteArray, sealed: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(GCM)

        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG, sealed, 0, NONCE))
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
        fun group(chat: String): Int? =
            if (chat.length == 5 && chat[0] == 'g') chat.substring(1).toIntOrNull(16) else null

        fun groupChat(group: Int): String = "g%04x".format(group)

        fun quote(author: String, hash: String): Quote? {
            if (hash.isEmpty()) {
                return null
            }

            return Quote(author, hash)
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

        private const val CHAT = "[0-9a-f]{2}|g[0-9a-f]{4}"
        private const val DATA = "(?:[0-9a-f]{2}|g[0-9a-f]{4})-[0-9a-f]{16}\\.dat"
        private const val KEYSTORE = "AndroidKeyStore"
        private const val HISTORY = "se.sanctorum.vigil.chat.history"
        private const val OUTBOX_KEY = "se.sanctorum.vigil.chat.outbox"
        private const val INBOX_KEY = "se.sanctorum.vigil.chat.inbox"
        private const val GCM = "AES/GCM/NoPadding"
        private const val NONCE = 12
        private const val TAG = 128
        private const val KEY = ".key"
        private const val LOG = ".log"
        private const val OUTBOX = "outbox.box"
        private const val RECEIPTS = "receipts.box"
        private const val FLAG_TRUNCATED = 1L
        private const val SEALED_HARDWARE: Byte = 1
        private const val SEALED_DEVICE: Byte = 2

        private val KEY_AAD = "VIGIL.CHAT.KEY".toByteArray()
        private val RECORD_AAD = "VIGIL.CHAT.V1".toByteArray()
        private val INBOX_AAD = "VIGIL.CHAT.INBOX".toByteArray()
        private val DATA_AAD = "VIGIL.CHAT.DATA".toByteArray()
        private val OUTBOX_AAD = "VIGIL.CHAT.OUTBOX".toByteArray()
        private val RECEIPTS_AAD = "VIGIL.CHAT.RECEIPTS".toByteArray()
    }
}
