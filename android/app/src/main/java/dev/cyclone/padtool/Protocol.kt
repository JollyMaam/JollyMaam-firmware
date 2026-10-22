package dev.cyclone.padtool

import java.io.IOException
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap

fun ByteArray.u8(at: Int) = this[at].toInt() and 255
fun ByteArray.u16(at: Int = 0) = u8(at) or (u8(at + 1) shl 8)
fun ByteArray.u32(at: Int = 0): Long = (0..3).fold(0L) { v, i -> v or (u8(at + i).toLong() shl (i * 8)) }
fun ByteArray.hex() = joinToString("") { "%02x".format(it.toInt() and 255) }
fun unhex(text: String): ByteArray {
    val clean = text.replace(Regex("\\s"), "")
    require(clean.length % 2 == 0 && clean.all { it.digitToIntOrNull(16) != null }) { "Expected pairs of hex digits" }
    return ByteArray(clean.length / 2) { clean.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
}
fun le32(n: Long) = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(n.toInt()).array()
fun ByteArray.cString() = takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)

interface Scsi {
    fun transfer(cdb: ByteArray, length: Int = 0, out: ByteArray? = null, timeout: Int = 3000, rawLength: Boolean = false): ByteArray
}

class Protocol(private val scsi: Scsi) {
    fun command(sub: Int, arg: Long = 0, length: Int = 0, out: ByteArray? = null): ByteArray =
        scsi.transfer(byteArrayOf(0xf5.toByte(), sub.toByte()) + le32(arg), length, out)

    fun identity() = Identity.decode(command(0, length = 64))
    fun fields(): List<Field> {
        require(identity().protocol == 1) { "Unsupported firmware protocol; use the app from the same build" }
        val result = mutableListOf<Field>()
        for (i in 0..255) {
            val raw = command(0x20, i.toLong(), 512)
            if (raw.isEmpty() || raw.all { it == 0.toByte() }) return result
            result += Field.decode(raw)
        }
        throw IOException("Firmware field table did not end")
    }
    fun read(target: Int, defaults: Boolean = false): Map<Int, Value> =
        decodeValues(command(if (defaults) 0x27 else 0x21, target.toLong(), 512), target)

    fun send(expected: Identity, items: List<Edit>, committed: () -> Unit = {}) {
        require(identity() == expected) { "Device identity changed; reconnect before applying" }
        if (items.isEmpty()) return
        // Firmware has a 1024-byte staging area; never silently split one Apply into commits.
        require(items.sumOf { 3 + it.bytes.size } <= 1024) { "Too many changes for one Apply; apply one page at a time" }
        command(0x24)
        var commitStarted = false
        try {
            val chunks = mutableListOf<Pair<ByteArray, Int>>()
            var chunk = ByteArrayOutputStream()
            var count = 0
            for (item in items) {
                require(item.target in 0..18 && item.id in 0..255 && item.bytes.size <= 255)
                if (chunk.size() + 3 + item.bytes.size > 512) {
                    chunks += chunk.toByteArray() to count
                    chunk = ByteArrayOutputStream(); count = 0
                }
                chunk.write(byteArrayOf(item.target.toByte(), item.id.toByte(), item.bytes.size.toByte()) + item.bytes)
                count++
            }
            if (count > 0) chunks += chunk.toByteArray() to count
            for ((bytes, n) in chunks) {
                command(0x22, out = bytes)
                val result = command(0x26, length = 256)
                require(result.isNotEmpty() && result.u8(0) == n && result.size >= n + 1) { "Incomplete WRITE result; nothing committed" }
                val errors = result.drop(1).take(n).filter { it != 0.toByte() }
                require(errors.isEmpty()) { "Firmware refused changes: ${errors.joinToString { resultName(it.toInt() and 255) }}" }
            }
            commitStarted = true
            command(0x23)
            committed()
        } catch (e: Exception) {
            runCatching { command(0x24) }
            if (commitStarted) throw IOException("Commit reply lost; refresh to check what was saved before retrying", e)
            throw e
        }
    }
    companion object {
        val targetNames = listOf("slot-a", "slot-b", "slot-x", "slot-y", "slot-up", "slot-down", "slot-left", "slot-right", "palette", "calibration", "remap-a", "remap-b", "remap-x", "remap-y", "remap-up", "remap-down", "remap-left", "remap-right", "remap-base")
        fun kind(target: Int) = if (target < 8) 0 else if (target >= 10) 3 else target - 7
        fun targets(fields: List<Field>) = (0..9).toList() + if (fields.any { it.kind == 3 }) listOf(18) + (10..17) else emptyList()
        fun resultName(code: Int) = listOf("ok", "unknown field", "bad length", "out of range", "staging area full").getOrElse(code) { "error $code" }
        fun decodeValues(raw: ByteArray, target: Int): Map<Int, Value> {
            require(raw.size >= 3 && raw.u8(0) == 1 && raw.u8(1) == target) { "Invalid settings response" }
            val values = linkedMapOf<Int, Value>()
            var at = 3
            repeat(raw.u8(2)) {
                require(at + 3 <= raw.size) { "Short settings header" }
                val id = raw.u8(at); val explicit = raw.u8(at + 1) != 0; val size = raw.u8(at + 2)
                require(at + 3 + size <= raw.size && id !in values) { "Short or duplicate setting" }
                values[id] = Value(raw.copyOfRange(at + 3, at + 3 + size), explicit)
                at += 3 + size
            }
            return values
        }
    }
}

data class Identity(val protocol: Int, val board: Int, val build: String, val flashId: String) {
    companion object {
        fun decode(raw: ByteArray): Identity {
            require(raw.size >= 40 && raw.copyOfRange(0, 4).contentEquals("C2PD".toByteArray()) && raw.u8(6) in 0..1) { "Not Cyclone firmware" }
            return Identity(raw.u16(4), raw.u8(6), raw.copyOfRange(8, 24).cString(), raw.copyOfRange(24, 40).hex())
        }
    }
}
data class Value(val bytes: ByteArray, val explicit: Boolean)
data class Edit(val target: Int, val id: Int, val bytes: ByteArray)
data class Field(val kind: Int, val id: Int, val type: Int, val size: Int, val max: Int, val developer: Boolean, val name: String, val help: String, val names: List<String>) {
    fun show(raw: ByteArray): String {
        require(raw.size == size) { "$name: wrong byte length" }
        return when {
            kind == 3 && name == "map" -> Remap.showMap(raw)
            kind == 3 && name == "rules" -> Remap.showRules(raw)
            type == 4 -> raw.cString()
            type == 0 -> names.getOrElse(raw.u8(0)) { raw.u8(0).toString() }
            type == 1 || type == 5 -> raw.indices.fold(0L) { v, i -> v or (raw.u8(i).toLong() shl (8 * i)) }.toString()
            type == 2 -> "#" + raw.hex()
            type == 6 || name.endsWith("curve_points") -> raw.joinToString(",") { (it.toInt() and 255).toString() }
            name == "address" -> if (raw.all { it == 0.toByte() }) "derived" else raw.reversedArray().hex().chunked(2).joinToString(":")
            else -> if (raw.all { it == 0.toByte() }) "none" else raw.hex()
        }
    }
    fun parse(input: String): ByteArray {
        val text = input.trim()
        val raw = when {
            kind == 3 && name == "map" -> Remap.parseMap(text)
            kind == 3 && name == "rules" -> Remap.parseRules(text)
            type == 4 -> input.toByteArray(Charsets.UTF_8).also { require(it.size <= size) { "Name is at most $size UTF-8 bytes" } }.copyOf(size)
            type == 0 -> byteArrayOf(names.indexOf(text).also { require(it >= 0) { "Select one of ${names.joinToString()}" } }.toByte())
            type == 1 || type == 5 -> {
                val n = if (text.startsWith("0x")) text.drop(2).toLong(16) else text.toLong()
                require(size in 1..4 && n >= 0 && n <= max && n < (1L shl (8 * size))) { "Expected 0–$max" }
                le32(n).copyOf(size)
            }
            type == 2 -> unhex(text.removePrefix("#"))
            type == 6 || name.endsWith("curve_points") -> {
                val points = if (text in listOf("linear", "concave", "s-curve")) Remap.curve(text, size) else
                    text.split(Regex("[,\\s]+")).map { it.toInt().also { n -> require(n in 0..(if (type == 6) max else 255)) { "Each value must be 0–$max" } }.toByte() }.toByteArray()
                require(points.zipWithNextUnsigned().all { (a, b) -> a <= b } || !name.endsWith("curve_points")) { "Curve points must be nondecreasing" }
                points
            }
            name == "address" -> if (text == "derived") ByteArray(size) else unhex(text.replace(":", "")).reversedArray()
            else -> if (text == "none") ByteArray(size) else unhex(text)
        }
        require(raw.size == size) { "$name needs $size bytes, received ${raw.size}" }
        return raw
    }
    companion object {
        fun decode(raw: ByteArray): Field {
            require(raw.size >= 8 && raw.u8(0) == 1 && raw.u8(1) in 0..3 && raw.u8(3) in 0..6) { "Invalid field header" }
            var at = 8
            fun string(): String {
                val end = (at until raw.size).firstOrNull { raw[it] == 0.toByte() } ?: throw IllegalArgumentException("Unterminated field string")
                val result = raw.copyOfRange(at, end).toString(Charsets.UTF_8); at = end + 1
                return result
            }
            val name = string(); val help = string()
            val names = if (raw.u8(3) == 0) List(raw.u16(6) + 1) { string() } else emptyList()
            require(raw.u8(4) > 0) { "Empty field" }
            return Field(raw.u8(1), raw.u8(2), raw.u8(3), raw.u8(4), raw.u16(6), raw.u8(5) and 1 != 0, name, help, names)
        }
    }
}
private fun ByteArray.zipWithNextUnsigned() = map { it.toInt() and 255 }.zipWithNext()

/** Drafts belong to identity + target + field, independently of visible controls. */
class Drafts {
    // Rotation/disconnect may render a snapshot while the USB worker finishes a commit.
    val edits: MutableMap<Pair<Int, Int>, ByteArray> = ConcurrentHashMap()
    fun stage(target: Int, field: Field, bytes: ByteArray, saved: Value) {
        if (bytes.contentEquals(saved.bytes) && saved.explicit || bytes.isEmpty() && !saved.explicit) edits.remove(target to field.id)
        else edits[target to field.id] = bytes
    }
    fun discard(target: Int? = null) { edits.keys.removeAll { target == null || it.first == target } }
    fun stageImport(fields: List<Field>, values: Map<Int, Map<Int, Value>>, items: List<Edit>, restore: Boolean) {
        for (item in items) {
            val field = fields.first { it.kind == Protocol.kind(item.target) && it.id == item.id }
            stage(item.target, field, item.bytes, values.getValue(item.target).getValue(item.id))
        }
        if (!restore) return
        // Changing type renews identity in firmware. Reapply backup pairing after type,
        // including bytes equal to the previous cache; those writes are still necessary.
        val type = fields.firstOrNull { it.kind == 0 && it.name == "type" } ?: return
        val pairingIds = fields.filter { it.kind == 0 && it.name in listOf("address", "bond") }.map { it.id }
        for (item in items) if (item.target < 8 && edits.containsKey(item.target to type.id) && item.id in pairingIds)
            edits[item.target to item.id] = item.bytes
    }
    fun items(fields: List<Field>, target: Int? = null): List<Edit> = edits.entries
        .filter { target == null || it.key.first == target }
        .sortedWith(compareBy({ Protocol.targets(fields).indexOf(it.key.first) }, { e -> fields.indexOfFirst { it.kind == Protocol.kind(e.key.first) && it.id == e.key.second }.let { if (it < 0) Int.MAX_VALUE else it } }))
        .map { Edit(it.key.first, it.key.second, it.value) }
}
