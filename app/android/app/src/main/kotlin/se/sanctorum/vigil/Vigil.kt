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
        voice: ByteArray?,
        datadir: String?,
        id: Long
    ): Int

    @JvmStatic
    external fun stop(id: Long)

    @JvmStatic
    external fun runId(): Long

    @JvmStatic
    external fun check(config: ByteArray): String

    @JvmStatic
    external fun edit(config: ByteArray, edits: String): ByteArray

    @JvmStatic
    external fun status(): String

    @JvmStatic
    external fun version(): String

    @JvmStatic
    external fun callDial(peer: Int)

    @JvmStatic
    external fun callHangup()

    @JvmStatic
    external fun voiceCall(peer: Int, mode: Int)

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
    external fun chatSend(peer: Int, ref: Long, text: ByteArray, author: Int, hash: Long, topic: Long): Boolean

    @JvmStatic
    external fun chatTopic(peer: Int, ref: Long, topic: Long, color: Int, name: ByteArray): Boolean

    @JvmStatic
    external fun chatForget(peer: Int): Boolean

    @JvmStatic
    external fun chatStored(token: Long): Boolean

    @JvmStatic
    external fun chatOpen(peer: Int, on: Boolean)

    @JvmStatic
    external fun chatTyping(peer: Int, topic: Long, on: Boolean)

    @JvmStatic
    external fun chatNick(name: String): Boolean

    @JvmStatic
    external fun chatAvatar(data: ByteArray?): Boolean

    @JvmStatic
    external fun chatDrop(peer: Int, ref: Long): Boolean

    @JvmStatic
    external fun chatGroupTyping(owner: Int, group: Long, topic: Long, members: ByteArray?)

    @JvmStatic
    external fun chatGroupSend(
        peer: Int, ref: Long, owner: Int, group: Long, serial: Int, topic: Long, text: ByteArray, author: Int,
        hash: Long
    ): Boolean

    @JvmStatic
    external fun chatGroupTopic(
        peer: Int, ref: Long, group: Long, serial: Int, topic: Long, color: Int, name: ByteArray
    ): Boolean

    @JvmStatic
    external fun chatGroupRoster(
        peer: Int, ref: Long, group: Long, serial: Int, ended: Boolean, members: ByteArray, name: ByteArray
    ): Boolean

    @JvmStatic
    external fun chatGroupLeave(owner: Int, ref: Long, group: Long): Boolean

    @JvmStatic
    external fun chatEvents(): ByteArray

    @JvmStatic
    external fun chatDataSend(peer: Int, ref: Long, kind: Int, name: ByteArray, data: ByteArray, topic: Long): Boolean

    @JvmStatic
    external fun chatGroupDataSend(
        peer: Int, ref: Long, owner: Int, group: Long, serial: Int, topic: Long, kind: Int, name: ByteArray,
        data: ByteArray
    ): Boolean

    @JvmStatic
    external fun chatDataTake(token: Long, size: Int): ByteArray?

    @JvmStatic
    external fun gospel(config: ByteArray?): Boolean

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
