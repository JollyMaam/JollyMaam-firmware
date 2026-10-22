package dev.cyclone.padtool

/** Packed-image validation and chip-key conversion, matching padflash.py. */
object FirmwareImage {
    val products = mapOf("GS_C2_ADC_DEVICE" to 0x100000, "GS_C2_Dongle" to 0x80000)
    fun crc(data: ByteArray, seed: Int = 0): Int {
        var value = seed
        for (b in data) {
            value = value xor ((b.toInt() and 255) shl 8)
            repeat(8) { value = ((value shl 1) xor (if (value and 0x8000 != 0) 0x1021 else 0)) and 0xffff }
        }
        return value
    }
    fun enc(data: ByteArray, seed: Int = 0xffff): ByteArray {
        var key = seed
        return ByteArray(data.size) { i ->
            val b = (data.u8(i) xor (key and 255)).toByte()
            key = ((key shl 1) xor (if (key and 0x8000 != 0) 0x1021 else 0)) and 0xffff
            b
        }
    }
    private fun slice(data: ByteArray, at: Int, size: Int): ByteArray {
        require(at >= 0 && size >= 0 && at.toLong() + size <= data.size) { "Truncated image" }
        return data.copyOfRange(at, at + size)
    }
    private fun num(data: ByteArray, at: Int) = data.u32(at).also { require(it <= Int.MAX_VALUE) { "Image offset out of range" } }.toInt()
    private fun name(data: ByteArray) = data.takeWhile { it != 0.toByte() && it != 0xff.toByte() }.toByteArray().toString(Charsets.US_ASCII)
    private fun header(data: ByteArray, at: Int, scrambled: Boolean = true): ByteArray {
        val raw = slice(data, at, 32).let { if (scrambled) enc(it) else it }
        require(crc(raw.copyOfRange(2, 32)) == raw.u16()) { "Image directory CRC mismatch" }
        return raw
    }
    private fun valid(data: ByteArray, at: Int) = runCatching { header(data, at).u16() != 0 }.getOrDefault(false)
    private data class Entry(val at: Int, val h: ByteArray) {
        val name get() = name(h.copyOfRange(16, 32))
        val offset get() = num(h, 4)
        val size get() = num(h, 8)
        val last get() = h.u16(14) != 0
    }
    private fun entries(data: ByteArray, base: Int): List<Entry> {
        val result = mutableListOf<Entry>()
        for (at in base + 32 until minOf(data.size - 31, base + 0x400) step 32) {
            val h = runCatching { header(data, at) }.getOrNull() ?: break
            result += Entry(at, h)
            if (h.u16(14) != 0) break
        }
        return result
    }
    fun base(data: ByteArray): Int {
        var base = listOf(0, 0x1000, 0x10000).firstOrNull { valid(data, it) } ?: error("No valid firmware flash header")
        for (entry in entries(data, base)) {
            if (entry.name == "isd_config.ini" && base.toLong() + entry.offset + entry.size <= data.size) {
                val config = slice(data, base + entry.offset, entry.size)
                val needle = "EOFFSET\u0000".toByteArray()
                val at = (0..(config.size - needle.size)).firstOrNull { i -> needle.indices.all { config[i + it] == needle[it] } }
                if (at != null && at + 12 <= config.size) {
                    val other = config.u32(at + 8) * 0x1000
                    if (other in 1..Int.MAX_VALUE.toLong() && valid(data, other.toInt())) base = other.toInt()
                }
                break
            }
        }
        return base
    }
    fun identity(data: ByteArray, requireKey: Boolean = true): Pair<String, Int?> {
        val base = base(data)
        val pid = name(slice(data, base + 16, 16))
        if (!requireKey) return pid to null
        val entry = entries(data, base).firstOrNull { it.name == "isd_config.ini" && it.size >= 34 } ?: error("No chip-key record")
        val record = slice(data, base + entry.offset, 34)
        require(crc(record.copyOfRange(0, 32)) == record.u16(32)) { "Chip-key record CRC mismatch" }
        val threshold = threshold(record)
        var key = 0
        repeat(16) { if ((record.u8(16 + it) xor record.u8(15 - it)) < threshold) key = key or (1 shl it) }
        return pid to key
    }
    private fun threshold(data: ByteArray): Int = (data.take(16).sumOf { it.toInt() and 255 } and 255).let { if (it >= 0xe0) 0xaa else if (it <= 0x10) 0x55 else it }
    private fun put16(data: ByteArray, at: Int, n: Int) { data[at] = n.toByte(); data[at + 1] = (n shr 8).toByte() }
    private fun sfc(data: ByteArray, key: Int): ByteArray {
        val out = ByteArray(data.size)
        for (at in data.indices step 32) enc(data.copyOfRange(at, minOf(at + 32, data.size)), key xor (at shr 2)).copyInto(out, at)
        return out
    }
    fun rekey(data: ByteArray, key: Int): ByteArray {
        require(key in 0..65535)
        val (pid, oldKey) = identity(data)
        if (oldKey == key) return data.copyOf()
        val base = base(data)
        val table = entries(data, base)
        require(table.isNotEmpty() && table.last().last) { "Unterminated image directory" }
        require(table.none { it.name == "app_dir_head2" }) { "Secondary application directories are unsupported" }
        val config = table.filter { it.name == "isd_config.ini" }.single()
        val app = base + table.filter { it.name == "app_dir_head" }.single().offset
        val configStart = base + config.offset
        require(configStart >= table.last().at + 32 && config.size >= 34 && configStart.toLong() + config.size <= app && app < data.size) { "Image areas outside expected bounds" }
        val configData = slice(data, configStart, config.size)
        require(crc(configData) == config.h.u16(2)) { "Configuration CRC mismatch" }
        val check = runCatching { header(data, app - 32) }.getOrNull()
        require(check == null || name(check.copyOfRange(16, 32)) != "check") { "Stock check records are unsupported" }
        val plain = sfc(slice(data, app, data.size - app), oldKey!!)
        var end = 0
        while (true) {
            val h = header(plain, end, false)
            require(h.u8(12) == 0x83 && (end != 0 || name(h.copyOfRange(16, 32)) == "app_area_head")) { "Invalid application area" }
            val size = num(h, 8)
            require(size >= 32 && end.toLong() + size <= plain.size) { "Truncated application area" }
            require(crc(slice(plain, end + 32, size - 32)) == h.u16(2)) { "Application/resource CRC mismatch" }
            end += size
            if (h.u16(14) != 0) break
        }
        val threshold = threshold(configData)
        repeat(16) { bit ->
            var value = configData.u8(16 + bit) xor configData.u8(15 - bit)
            val want = key and (1 shl bit) != 0
            if ((value < threshold) != want) {
                value = if (want) value % threshold else threshold + value % (256 - threshold)
                configData[16 + bit] = (configData.u8(15 - bit) xor value).toByte()
            }
        }
        put16(configData, 32, crc(configData.copyOfRange(0, 32)))
        val ch = config.h.copyOf()
        put16(ch, 2, crc(configData)); put16(ch, 0, crc(ch.copyOfRange(2, 32)))
        val out = data.copyOf()
        configData.copyInto(out, configStart); enc(ch).copyInto(out, config.at)
        sfc(plain.copyOfRange(0, end), key).copyInto(out, app)
        require(identity(out) == pid to key) { "Converted image has wrong identity" }
        return out
    }
    fun unwrap(data: ByteArray): ByteArray {
        if (data.size < 64) return data
        val h = enc(data.copyOfRange(0, 64))
        if (crc(h.copyOfRange(2, 64)) != h.u16()) return data
        val count = h.u16(8); val end = 64 + count * 80
        require(count > 0 && end <= data.size && crc(slice(data, 64, end - 64)) == h.u16(2)) { "Invalid UFW entry list" }
        for (at in 64 until end step 80) {
            val item = enc(slice(data, at, 80))
            if (item.u16() == 0) {
                val flash = slice(data, num(item, 8), num(item, 12))
                require(crc(flash) == item.u16(4)) { "UFW flash.bin CRC mismatch" }
                return flash
            }
        }
        error("UFW contains no flash.bin")
    }
    fun decodeKey(data: ByteArray): Int {
        val magic = unhex("c3cfc0e8ced2b0aec4e3a3acd3f1c1d6")
        var crc = crc(byteArrayOf(-1, -1), 0xffff)
        var result = 0
        data.forEachIndexed { i, b -> crc = crc(byteArrayOf(magic[i % 16]), crc); result = result or (((b.toInt() and 255) xor (crc and 255)) shl (i * 8)) }
        return result
    }
}
