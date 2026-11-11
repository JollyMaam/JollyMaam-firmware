package dev.jollymaam.app

import android.util.Base64

fun ByteArray.u8(at: Int) = this[at].toInt() and 255
fun ByteArray.u16(at: Int = 0) = u8(at) or (u8(at + 1) shl 8)
fun ByteArray.u32(at: Int = 0): Long = (0..3).fold(0L) { v, i -> v or (u8(at + i).toLong() shl (i * 8)) }
fun ByteArray.hex() = joinToString("") { "%02x".format(it.toInt() and 255) }
fun ByteArray.cString() = takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
fun unhex(text: String) = ByteArray(text.length / 2) { text.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
fun ByteArray.base64(): String = Base64.encodeToString(this, Base64.NO_WRAP)
fun unbase64(text: String): ByteArray = Base64.decode(text, Base64.DEFAULT)

interface Scsi {
    fun transfer(cdb: ByteArray, length: Int = 0, out: ByteArray? = null, timeout: Int = 3000, rawLength: Boolean = false): ByteArray
}
