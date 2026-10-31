@file:Suppress("DEPRECATION")
package dev.cyclone.padtool

import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.*
import org.junit.Test
import java.util.zip.ZipInputStream

/** Real Android JNI calls with fake endpoints: no USB permission or hardware. */
class NativeCoreDeviceTest {
    private fun bundle() = InstrumentationRegistry.getInstrumentation().context.assets.open("firmware-fixture.zip").use { it.readBytes() }
    private class Fake : NativeUsb() {
        var command = 0; var tag = 0; var updater = false; var begun = 0; var written = 0
        var committed = false; var starts = 0; var reconnects = 0; var rom = 0
        val flash = ByteArray(4096)
        override fun close() {}
        override fun waitMillis(ms: Int) {}
        override fun featureSize() = 356
        override fun featureSet(data: ByteArray): Int {
            check(data.size == 357)
            command = data.u8(2); tag = data.u8(20)
            val arg = data.u32(3).toInt(); val count = data.u16(17)
            when (command) {
                0x11 -> rom++
                0x40 -> updater = true
                0x42 -> { check(count == 44); begun = arg }
                0x43 -> { check(arg == written); data.copyInto(flash, written, 21, 21 + count); written += count }
                0x44 -> { check(written == begun); committed = true }
                0x10 -> { check(committed); starts++ }
            }
            return data.size
        }
        private fun put(data: ByteArray, offset: Int, value: Int) { le32(value.toLong()).copyInto(data, offset) }
        override fun featureGet(data: ByteArray): Int {
            data.fill(0); data[1] = tag.toByte(); data[2] = 3
            if (command == 0) {
                data[3] = 88
                val identity = ByteArray(88)
                "C2PD".toByteArray().copyInto(identity); identity[7] = if (updater) 1 else 0; identity[24] = 42
                "U2C_WIRELESS".toByteArray().copyInto(identity, 40); identity.copyInto(data, 5)
            } else if (command == 0x41) {
                data[3] = 32; data[5] = 1; data[6] = if (committed) 2 else if (begun > 0) 1 else 0; data[8] = if (committed) 1 else 0
                put(data, 13, 4096); put(data, 17, written); put(data, 21, begun); put(data, 29, 128)
            }
            return data.size
        }
        override fun reconnect(uid: ByteArray): Boolean { check(uid[0] == 42.toByte()); reconnects++; return true }
    }
    @Test fun testStockLoaderFromUpdater() {
        val fake = Fake().apply { updater = true }; val uid = ByteArray(16).apply { this[0] = 42 }
        assertEquals(0, NativeCore.stockLoader(uid, fake)); assertEquals(1, fake.rom); assertEquals(0, fake.begun)
    }
    @Test fun testPackageInspection() {
        val products = NativeCore.products(bundle())!!
        assertEquals(3, products.size); assertEquals("U2C_WIRELESS", products[0])
        assertNull(NativeCore.products(ByteArray(4)))
    }
    @Test fun testCompleteUpdateWithReconnection() {
        val bytes = bundle(); val fake = Fake(); val uid = ByteArray(16).apply { this[0] = 42 }
        assertEquals(88, NativeCore.probe(fake, 0)!!.size)
        assertEquals(0, NativeCore.run(bytes, "U2C_WIRELESS", 0, uid, fake))
        assertTrue(fake.committed); assertEquals(1, fake.starts); assertEquals(1, fake.reconnects)
        val expected = ZipInputStream(bytes.inputStream()).use { zip ->
            var image: ByteArray? = null
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.name == "update.bin") { image = zip.readBytes(); break }
            }
            image!!
        }
        assertTrue(expected.contentEquals(fake.flash.copyOf(fake.written)))
    }
    @Test fun testIdentityMismatchStopsBeforeBegin() {
        val fake = Fake(); val uid = ByteArray(16).apply { this[0] = 43 }
        assertTrue(NativeCore.run(bundle(), "U2C_WIRELESS", 0, uid, fake) != 0)
        assertEquals(0, fake.begun); assertFalse(fake.committed); assertEquals(0, fake.starts)
    }
}
