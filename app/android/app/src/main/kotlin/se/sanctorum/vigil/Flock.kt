package se.sanctorum.vigil

object Flock {
    fun hex(flock: Long): String = java.lang.Long.toHexString(flock)

    fun hex(flock: String): String = hex(java.lang.Long.parseUnsignedLong(flock, 16))

    fun id(flock: Long, peer: Int): String = "%s-%02x".format(hex(flock), peer)

    fun id(flock: String, peer: String): String = "$flock-$peer"

    fun of(id: String): String = id.substringBefore('-')

    fun number(id: String): Long = java.lang.Long.parseUnsignedLong(of(id), 16)

    fun local(id: String): String = id.substringAfter('-')

    fun peer(id: String): Int = local(id).toInt(16)

    fun valid(id: String): Boolean = id.matches(Regex(PEER)) && peer(id) in 1..0xfe

    const val PEER = "[0-9a-f]{1,16}-[0-9a-f]{2}"
}
