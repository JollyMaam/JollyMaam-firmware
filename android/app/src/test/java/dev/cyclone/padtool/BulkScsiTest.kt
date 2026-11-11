package dev.cyclone.padtool

import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

private fun le32(n: Long) = java.nio.ByteBuffer.allocate(4).order(java.nio.ByteOrder.LITTLE_ENDIAN).putInt(n.toInt()).array()

class BulkScsiTest {
    private class Pipe : BulkPipe {
        val writes = mutableListOf<ByteArray>()
        val reads = ArrayDeque<ByteArray>()
        var reset = false
        var shortWrite = false
        override fun write(bytes: ByteArray, timeout: Int): Int { writes += bytes; return bytes.size - if (shortWrite) 1 else 0 }
        override fun read(bytes: ByteArray, timeout: Int): Int { val data = reads.removeFirst(); data.copyInto(bytes); return data.size }
        override fun reset() { reset = true }
        fun status(tag: Int = 1, residue: Int = 0, code: Int = 0) { reads += le32(0x53425355) + le32(tag.toLong()) + le32(residue.toLong()) + byteArrayOf(code.toByte()) }
    }
    @Test fun wrapsCommandsAndHonorsResidue() {
        val pipe = Pipe(); pipe.reads += byteArrayOf(1,2,3,4); pipe.status(residue = 2)
        assertArrayEquals(byteArrayOf(1,2),BulkScsi(pipe).transfer(byteArrayOf(0xf5.toByte(),0),4))
        val cbw = pipe.writes.single()
        assertEquals(31,cbw.size); assertEquals(0x43425355L,cbw.u32()); assertEquals(1L,cbw.u32(4)); assertEquals(4L,cbw.u32(8)); assertEquals(0x80,cbw.u8(12)); assertEquals(16,cbw.u8(14))
        assertFalse(pipe.reset)
    }
    @Test fun nativeScsiAdapterKeepsLoaderResiduePolicyAndInquiryLength() {
        val inquiry = Pipe(); inquiry.reads += ByteArray(36); inquiry.status()
        val cdb = byteArrayOf(0x12, 0, 0, 0, 36, 0)
        assertEquals(36, AndroidNativeUsb.transferScsi(BulkScsi(inquiry), cdb, 1, ByteArray(36), 2000))
        assertEquals(6, inquiry.writes.single().u8(14))
        val loader = Pipe(); loader.reads += ByteArray(16); loader.status(residue = 16)
        val command = byteArrayOf(0xfc.toByte(), 0x0a).copyOf(16)
        assertEquals(16, AndroidNativeUsb.transferScsi(BulkScsi(loader), command, 1, ByteArray(16), 20000))
        assertEquals(16, loader.writes.single().u8(14))
        val short = Pipe(); short.reads += ByteArray(8); short.status(residue = 16)
        assertThrows(IOException::class.java) { AndroidNativeUsb.transferScsi(BulkScsi(short), command, 1, ByteArray(16), 20000) }
        val write = Pipe(); write.status(residue = 4)
        assertEquals(4, AndroidNativeUsb.transferScsi(BulkScsi(write), byteArrayOf(0xfb.toByte(), 4).copyOf(16), 2, ByteArray(4), 20000))
    }
    @Test fun loaderIgnoresBogusResidueButNeverPadsShortReads() {
        val p = Pipe(); p.reads += byteArrayOf(1,2,3,4); p.status(residue = 4)
        assertEquals(4,BulkScsi(p).transfer(byteArrayOf(0xfc.toByte(),0x0a),4,rawLength=true).size)
        val q = Pipe(); q.reads += byteArrayOf(1,2); q.status(residue = 4)
        assertThrows(IOException::class.java) { BulkScsi(q).transfer(byteArrayOf(0xfc.toByte(),0x0a),4,rawLength=true) }; assertTrue(q.reset)
    }
    @Test fun invalidTagsAndStatusesResetWithoutRetry() {
        for ((tag, code) in listOf(7 to 0, 1 to 1, 1 to 2)) {
            val p = Pipe(); p.status(tag=tag,code=code)
            assertThrows(IOException::class.java) { BulkScsi(p).transfer(byteArrayOf(0xf5.toByte(),0x23)) }
            assertTrue(p.reset); assertEquals(1,p.writes.size)
        }
    }
    @Test fun chunksLargeWritesAndChecksEveryByteWasAccepted() {
        val p = Pipe(); p.status()
        BulkScsi(p).transfer(byteArrayOf(0xfb.toByte(),4),out=ByteArray(32768))
        assertEquals(listOf(31,16384,16384),p.writes.map { it.size })
        val q = Pipe().apply { shortWrite=true }
        assertThrows(IOException::class.java) { BulkScsi(q).transfer(byteArrayOf(0xf5.toByte(),0x22),out=byteArrayOf(1)) }
        assertTrue(q.reset)
    }
}
