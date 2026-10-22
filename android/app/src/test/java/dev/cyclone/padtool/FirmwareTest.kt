package dev.cyclone.padtool

import org.junit.Assert.*
import org.junit.Test
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

class FirmwareTest {
    private fun fixture(name: String) = javaClass.getResourceAsStream("/$name")!!.use { it.readBytes() }
    @Test fun matchesPythonImageIdentityAndKeyConversionExactly() {
        val image = fixture("image.bin")
        assertEquals("GS_C2_ADC_DEVICE" to 65535, FirmwareImage.identity(image))
        val converted = FirmwareImage.rekey(image,0x1234)
        assertArrayEquals(fixture("image-rekey.bin"),converted)
        assertEquals("GS_C2_ADC_DEVICE" to 0x1234,FirmwareImage.identity(converted))
        assertArrayEquals(image,FirmwareImage.rekey(converted,65535))
        assertEquals(0x31c3,FirmwareImage.crc("123456789".toByteArray()))
    }
    @Test fun rejectsCorruptAndTruncatedImagesBeforeConversion() {
        val image = fixture("image.bin")
        image[0x1130] = (image.u8(0x1130) xor 1).toByte()
        assertThrows(IllegalArgumentException::class.java) { FirmwareImage.rekey(image,0x1234) }
        assertThrows(IllegalArgumentException::class.java) { FirmwareImage.rekey(image.copyOf(0x1005),0x1234) }
        assertThrows(IllegalStateException::class.java) { FirmwareImage.identity(ByteArray(60)) }
    }
    private class Device(image: ByteArray) : Scsi {
        val memory = ByteArray(0x100000) { -1 }.apply { image.copyInto(this); "keep".toByteArray().copyInto(this,0x70000) }
        val commands = mutableListOf<Int>()
        val ram = java.io.ByteArrayOutputStream()
        var corrupt = false
        var written = false
        private fun big(c: ByteArray,at: Int) = ByteBuffer.wrap(c,at,4).order(ByteOrder.BIG_ENDIAN).int
        override fun transfer(cdb: ByteArray, length: Int, out: ByteArray?, timeout: Int, rawLength: Boolean): ByteArray {
            if (cdb.u8(0) == 0x12) return ByteArray(36).apply { "BR23".toByteArray().copyInto(this,8); "UBOOT1.00".toByteArray().copyInto(this,16) }
            val cmd = (cdb.u8(0) shl 8) or cdb.u8(1); commands += cmd
            if (cmd == 0xfb06 || cmd == 0xfb04) {
                require(rawLength)
                val address = big(cdb,2); val size = (cdb.u8(6) shl 8) or cdb.u8(7)
                require(size == out!!.size && cdb.u16(9) == FirmwareImage.crc(out))
                if (cmd == 0xfb06) { require(address == 0x12000 + ram.size()); ram.write(out) }
                else { require(out.indices.all { (memory.u8(address+it) and out.u8(it)) == out.u8(it) }); out.copyInto(memory,address); written=true }
                return ByteArray(0)
            }
            if (cmd == 0xfd05) {
                val at = big(cdb,2); val count = (cdb.u8(6) shl 8) or cdb.u8(7)
                require(count == length)
                return memory.copyOfRange(at,at+count).apply { if (corrupt && written && at == 0) this[0] = (u8(0) xor 1).toByte() }
            }
            val reply = ByteArray(14)
            when(cmd) {
                0xfb00,0xfb01 -> { val at=big(cdb,2); val size=if(cmd==0xfb00)65536 else 4096; memory.fill(-1,at,at+size) }
                0xfc0a -> { reply[2]=20; reply[3]=0x40; reply[4]=0xef.toByte() }
                0xfc14 -> { reply[2]=2 } // 512, big-endian.
                0xfc09 -> {
                    var crc = FirmwareImage.crc(byteArrayOf(-1,-1),65535)
                    crc = FirmwareImage.crc(byteArrayOf(0xc3.toByte()),crc); val low=255 xor (crc and 255)
                    crc = FirmwareImage.crc(byteArrayOf(0xcf.toByte()),crc); val high=255 xor (crc and 255)
                    reply[4]=high.toByte(); reply[5]=low.toByte()
                }
                0xfb08,0xfc0c -> {}
                else -> error("Unexpected command %04x".format(cmd))
            }
            return cdb.copyOfRange(0,2)+reply
        }
    }
    @Test fun uploadsFlashesVerifiesAndPreservesUntouchedFlash() {
        val image = fixture("image.bin"); val d = Device(image); val l=Br23Loader(d)
        val blob=ByteArray(768) { it.toByte() }; l.start(blob)
        assertArrayEquals(blob,d.ram.toByteArray()); assertEquals(65535,l.chipKey); assertEquals(0x100000,l.flashSize)
        val before=d.memory.copyOf(); val converted=fixture("image-rekey.bin")
        l.flash(converted)
        assertArrayEquals(image,d.memory.copyOfRange(0,image.size))
        assertArrayEquals(before.copyOfRange(image.size,before.size),d.memory.copyOfRange(image.size,d.memory.size))
        assertEquals(0xfc0c,d.commands.last()); assertTrue(d.commands.indexOf(0xfd05)<d.commands.indexOf(0xfb01))
    }
    @Test fun wrongProductAndBadSourceNeverErase() {
        val d=Device(fixture("image.bin")); val l=Br23Loader(d); l.start(byteArrayOf(1))
        assertThrows(IllegalArgumentException::class.java) { l.flash(fixture("image-dongle.bin")) }
        assertFalse(d.commands.any { it in listOf(0xfb00,0xfb01,0xfb04) })
    }
    @Test fun failedVerificationLeavesDeviceInLoader() {
        val d=Device(fixture("image.bin")).apply { corrupt=true }; val l=Br23Loader(d); l.start(byteArrayOf(1))
        assertThrows(IllegalArgumentException::class.java) { l.flash(fixture("image.bin")) }
        assertFalse(0xfc0c in d.commands)
    }
}
