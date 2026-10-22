package dev.cyclone.padtool

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

class Br23Loader(private val scsi: Scsi, private val progress: (String, Int, Int) -> Unit = { _, _, _ -> }) {
    var flashSize = 0; private set
    var chipKey = 0; private set
    private var io = 512
    private fun args(address: Int, count: Int? = null): ByteArray = ByteBuffer.allocate(if (count == null) 4 else 6).order(ByteOrder.BIG_ENDIAN).putInt(address).apply { if (count != null) putShort(count.toShort()) }.array()
    private fun transfer(command: Int, args: ByteArray = ByteArray(0), length: Int = 0, out: ByteArray? = null): ByteArray {
        val cdb = ByteArray(16) { 0xff.toByte() }
        cdb[0] = (command shr 8).toByte(); cdb[1] = command.toByte()
        require(args.size <= 14); args.copyInto(cdb, 2)
        val data = scsi.transfer(cdb, length, out, 20000, true)
        if (out == null && data.size != length) throw IOException("Short loader response")
        return data
    }
    private fun execute(command: Int, args: ByteArray = ByteArray(0)): ByteArray {
        val data = transfer(command, args, 16)
        require(data.u8(0) == command shr 8 && data.u8(1) == command and 255) { "Loader did not acknowledge %04x".format(command) }
        return data.copyOfRange(2, 16)
    }
    private fun write(command: Int, address: Int, data: ByteArray) {
        val crc = FirmwareImage.crc(data)
        transfer(command, args(address, data.size) + byteArrayOf(0, crc.toByte(), (crc shr 8).toByte()), out = data)
    }
    fun start(blob: ByteArray) {
        require(blob.isNotEmpty() && blob.size <= 0x20000) { "Select the BR23 RAM loader binary" }
        val inquiry = scsi.transfer(byteArrayOf(0x12, 0, 0, 0, 36), 36)
        require(inquiry.size == 36 && inquiry.copyOfRange(8, 12).cString() == "BR23" && inquiry.copyOfRange(16, 21).cString() == "UBOOT") { "Selected device is not a BR23 ROM loader" }
        for (at in blob.indices step 512) {
            val data = blob.copyOfRange(at, minOf(at + 512, blob.size))
            write(0xfb06, 0x12000 + at, data); progress("Loading flash helper", at + data.size, blob.size)
        }
        execute(0xfb08, args(0x12000, 1))
        var info: ByteArray? = null
        for (attempt in 0 until 20) {
            try { info = execute(0xfc0a); break } catch (e: Exception) { if (attempt == 19) throw e; Thread.sleep(100) }
        }
        val capacity = info!!.u8(2) and 31
        // Flash ID is little-endian info[2:5]; its low 5 bits carry capacity.
        require(capacity in 16..24) { "Invalid flash capacity" }
        flashSize = 1 shl capacity
        runCatching { ByteBuffer.wrap(execute(0xfc14)).order(ByteOrder.BIG_ENDIAN).int }.getOrNull()?.let { if (it in 64..0x8000) io = it }
        val record = execute(0xfc09, args(0xac6900))
        chipKey = FirmwareImage.decodeKey(byteArrayOf(record[5], record[4]))
    }
    fun read(address: Int, size: Int, stage: String = "Reading flash"): ByteArray {
        require(address >= 0 && size >= 0 && address.toLong() + size <= flashSize) { "Read exceeds flash capacity" }
        val data = ByteArray(size)
        for (at in 0 until size step io) {
            val count = minOf(io, size - at)
            transfer(0xfd05, args(address + at, count), count).copyInto(data, at)
            progress(stage, at + count, size)
        }
        return data
    }
    fun pid() = FirmwareImage.identity(read(0, minOf(flashSize, 0x11000)), false).first
    fun prepare(image: ByteArray): ByteArray {
        val (pid, key) = FirmwareImage.identity(image)
        require(pid in FirmwareImage.products && pid() == pid) { "Image product does not match this device" }
        require(flashSize == FirmwareImage.products[pid] && image.isNotEmpty() && image.size <= flashSize) { "Image or flash capacity does not match product" }
        return if (key == chipKey) image else FirmwareImage.rekey(image, chipKey)
    }
    /** Caller finishes and closes any backup destination before calling this. */
    fun flash(prepared: ByteArray) {
        val image = prepare(prepared)
        val top = (image.size + 4095) and 4095.inv()
        var at = 0
        while (at < top) {
            val block = at % 0x10000 == 0 && top - at >= 0x10000
            execute(if (block) 0xfb00 else 0xfb01, args(at))
            at += if (block) 0x10000 else 0x1000
            progress("Erasing flash", at, top)
        }
        for (offset in image.indices step io) {
            val chunk = image.copyOfRange(offset, minOf(offset + io, image.size))
            if (chunk.any { it != 0xff.toByte() }) write(0xfb04, offset, chunk)
            progress("Writing firmware", offset + chunk.size, image.size)
        }
        require(read(0, image.size, "Verifying firmware").contentEquals(image)) { "Flash verification failed; device left in loader for recovery" }
        progress("Restarting device", 1, 1)
        try { execute(0xfc0c, args(1)) } catch (_: IOException) { /* Reboot normally disconnects before the reply. */ }
    }
}
