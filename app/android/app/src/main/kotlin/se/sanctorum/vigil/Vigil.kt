package se.sanctorum.vigil

import android.net.VpnService

object Vigil {
    init {
        System.loadLibrary("vigil")
    }

    @JvmStatic
    external fun run(
        service: VpnService,
        fd: Int,
        config: ByteArray,
        gospels: ByteArray?,
        datadir: String?,
        id: Long
    ): Int

    @JvmStatic
    external fun stop(id: Long)

    @JvmStatic
    external fun runId(): Long

    @JvmStatic
    external fun gospelMax(): Int

    @JvmStatic
    external fun check(config: ByteArray): String

    @JvmStatic
    external fun edit(config: ByteArray, edits: String): ByteArray

    @JvmStatic
    external fun status(): String

    @JvmStatic
    external fun version(): String

    @JvmStatic
    external fun callDial(flock: Long, peer: Int)

    @JvmStatic
    external fun callHangup()

    @JvmStatic
    external fun voiceCall(flock: Long, peer: Int, mode: Int)

    @JvmStatic
    external fun voiceHangup()

    @JvmStatic
    external fun voiceAnswer()

    @JvmStatic
    external fun voiceMute(on: Boolean)

    @JvmStatic
    external fun voiceStatus(): String

    @JvmStatic
    external fun kyrkaVersion(): String

    @JvmStatic
    external fun chatSend(
        flock: Long, peer: Int, ref: Long, text: ByteArray, author: Int, hash: Long, topic: Long
    ): Boolean

    @JvmStatic
    external fun chatTopic(flock: Long, peer: Int, ref: Long, topic: Long, color: Int, name: ByteArray): Boolean

    @JvmStatic
    external fun chatForget(flock: Long, peer: Int): Boolean

    @JvmStatic
    external fun chatStored(token: Long): Boolean

    @JvmStatic
    external fun chatOpen(flock: Long, peer: Int, on: Boolean)

    @JvmStatic
    external fun chatAccept(flock: Long, peer: Int, on: Boolean)

    @JvmStatic
    external fun chatIgnore(flock: Long, peer: Int, on: Boolean)

    @JvmStatic
    external fun chatTyping(flock: Long, peer: Int, topic: Long, on: Boolean)

    @JvmStatic
    external fun chatAvatar(data: ByteArray?): Boolean

    @JvmStatic
    external fun chatDrop(flock: Long, peer: Int, ref: Long): Boolean

    @JvmStatic
    external fun chatGroupTyping(flock: Long, owner: Int, group: Long, topic: Long, members: ByteArray?)

    @JvmStatic
    external fun chatGroupSend(
        flock: Long, peer: Int, ref: Long, owner: Int, group: Long, serial: Int, topic: Long, text: ByteArray,
        author: Int, hash: Long
    ): Boolean

    @JvmStatic
    external fun chatGroupTopic(
        flock: Long, peer: Int, ref: Long, group: Long, serial: Int, topic: Long, color: Int, name: ByteArray
    ): Boolean

    @JvmStatic
    external fun chatGroupRoster(
        flock: Long, peer: Int, ref: Long, group: Long, serial: Int, ended: Boolean, members: ByteArray, name: ByteArray
    ): Boolean

    @JvmStatic
    external fun chatGroupLeave(flock: Long, owner: Int, ref: Long, group: Long): Boolean

    @JvmStatic
    external fun chatRetract(
        flock: Long, peer: Int, ref: Long, owner: Int, group: Long, serial: Int, topic: Long, hash: Long
    ): Boolean

    @JvmStatic
    external fun chatEvents(): ByteArray

    @JvmStatic
    external fun chatDataSend(
        flock: Long, peer: Int, ref: Long, kind: Int, name: ByteArray, data: ByteArray, topic: Long
    ): Boolean

    @JvmStatic
    external fun chatGroupDataSend(
        flock: Long, peer: Int, ref: Long, owner: Int, group: Long, serial: Int, topic: Long, kind: Int,
        name: ByteArray, data: ByteArray
    ): Boolean

    @JvmStatic
    external fun chatDataTake(token: Long, size: Int): ByteArray?

    @JvmStatic
    external fun gospel(set: ByteArray?): Boolean

    @JvmStatic
    external fun additionalInput(flock: Long, peer: Int, ikm: ByteArray?): Boolean

    @JvmStatic
    external fun safetyWords(half: ByteArray): String?

    @JvmStatic
    external fun safetyHalf(words: ByteArray): ByteArray?

    @JvmStatic
    external fun safetyCode(input: ByteArray): String?

    @JvmStatic
    external fun safetyStart(flock: Long, peer: Int, own: ByteArray, theirs: ByteArray): Boolean

    @JvmStatic
    external fun safetyGet(flock: Long, peer: Int): ByteArray?

    @JvmStatic
    external fun safetyStop()

    @JvmStatic
    external fun chatCompanion(config: ByteArray?): Boolean

    @JvmStatic
    external fun chatCompanionSync(json: ByteArray, data: ByteArray?): Boolean
    external fun chatCompanionResync(done: Boolean): Boolean

    @JvmStatic
    external fun remoteWord(idx: Int): String?

    @JvmStatic
    external fun remoteWords(words: ByteArray): String?

    @JvmStatic
    external fun remoteStart(id: String, secret: String): Boolean

    @JvmStatic
    external fun remoteRun(): Int

    @JvmStatic
    external fun remoteConfig(): ByteArray?

    @JvmStatic
    external fun remoteStop()
}
