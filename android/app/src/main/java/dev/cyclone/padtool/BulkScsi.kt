package dev.cyclone.padtool

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

interface BulkPipe {
    fun write(bytes: ByteArray, timeout: Int): Int
    fun read(bytes: ByteArray, timeout: Int): Int
    fun reset()
}

/** USB mass-storage Bulk-Only Transport, LUN 0. No command is retried after failure. */
class BulkScsi(private val pipe: BulkPipe) : Scsi {
    private var tag = 0
    @Synchronized
    override fun transfer(cdb: ByteArray, length: Int, out: ByteArray?, timeout: Int, rawLength: Boolean): ByteArray {
        require(cdb.size in 1..16 && length >= 0 && timeout > 0)
        val size = out?.size ?: length
        val current = ++tag
        val cbw = ByteBuffer.allocate(31).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(0x43425355).putInt(current).putInt(size)
            .put(if (out == null && size > 0) 0x80.toByte() else 0.toByte()).put(0.toByte()).put(16.toByte()).put(cdb.copyOf(16)).array()
        try {
            if (pipe.write(cbw, timeout) != 31) throw IOException("Short USB command wrapper")
            var received = ByteArray(0)
            if (size > 0 && out != null) {
                var at = 0
                while (at < size) {
                    val chunk = out.copyOfRange(at, minOf(at + 16384, size))
                    if (pipe.write(chunk, timeout) != chunk.size) throw IOException("Short USB data write")
                    at += chunk.size
                }
            } else if (size > 0) {
                val buffer = ByteArray(size)
                var at = 0
                while (at < size) {
                    val chunk = ByteArray(minOf(16384, size - at))
                    val n = pipe.read(chunk, timeout)
                    if (n < 0 || n > chunk.size) throw IOException("USB data read failed")
                    chunk.copyInto(buffer, at, 0, n); at += n
                    if (n < chunk.size) break
                }
                received = buffer.copyOf(at)
            }
            val csw = ByteArray(13)
            if (pipe.read(csw, timeout) != 13 || csw.u32() != 0x53425355L || csw.u32(4) != (current.toLong() and 0xffffffffL)) throw IOException("Invalid USB status wrapper or tag")
            val residue = csw.u32(8)
            if (residue > size || csw.u8(12) > 2) throw IOException("Invalid USB status or residue")
            if (csw.u8(12) != 0) throw IOException("SCSI command failed (status ${csw.u8(12)})")
            if (out != null) {
                if (residue != 0L && !rawLength) throw IOException("Device did not accept all output bytes")
                return ByteArray(0)
            }
            val actual = if (rawLength) size else size - residue.toInt()
            if (received.size < actual) throw IOException("Short USB response: ${received.size}/$actual bytes")
            return received.copyOf(actual)
        } catch (e: Exception) {
            runCatching { pipe.reset() }
            throw e
        }
    }
}
