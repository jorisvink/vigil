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
    external fun chatSend(peer: Int, ref: Long, text: ByteArray, author: Int, hash: Long): Boolean

    @JvmStatic
    external fun chatForget(peer: Int)

    @JvmStatic
    external fun chatStored(token: Long)

    @JvmStatic
    external fun chatOpen(peer: Int, on: Boolean)

    @JvmStatic
    external fun chatTyping(peer: Int, on: Boolean)

    @JvmStatic
    external fun chatNick(name: String): Boolean

    @JvmStatic
    external fun chatGroup(group: Int)

    @JvmStatic
    external fun chatGroupTyping(on: Boolean)

    @JvmStatic
    external fun chatGroupSend(ref: Long, text: ByteArray, author: Int, hash: Long): Boolean

    @JvmStatic
    external fun chatEvents(): ByteArray

    @JvmStatic
    external fun chatDataSend(peer: Int, ref: Long, kind: Int, name: ByteArray, data: ByteArray): Boolean

    @JvmStatic
    external fun chatGroupDataSend(ref: Long, kind: Int, name: ByteArray, data: ByteArray): Boolean

    @JvmStatic
    external fun chatDataTake(token: Long, size: Int): ByteArray?

    @JvmStatic
    external fun gospel(config: ByteArray?): Boolean
}
