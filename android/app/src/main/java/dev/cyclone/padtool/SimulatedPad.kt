package dev.cyclone.padtool

import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import kotlin.math.cos
import kotlin.math.sin

/** A development device behind the real protocol client. Never opens USB.
 * Metadata/defaults come from tools/export_simulation.c, not a second field catalogue.
 * This models the settings workflow, not controller timing or flash hardware. */
class SimulatedPad(fixture: String, saved: String? = null, private val persist: (String) -> Unit = {}, private val now: () -> Long = System::currentTimeMillis) : Scsi {
    private val data = JSONObject(fixture).also { require(it.getInt("format") == 1) }
    private val descriptions = data.getJSONArray("fields").let { a -> List(a.length()) { unhex(a.getString(it)) } }
    val fields = descriptions.map(Field::decode)
    private val defaults = (0..18).associateWith { Protocol.decodeValues(unhex(data.getJSONArray("defaults").getString(it)), it) }
    private val original = (0..18).associateWith { Protocol.decodeValues(unhex(data.getJSONArray("values").getString(it)), it) }
    private val overrides = linkedMapOf<Pair<Int, Int>, ByteArray>()
    private val staged = mutableListOf<Edit>()
    private var results = ByteArray(0)
    private val started = now()
    private var state = 1
    private var slot = 0
    private var renewals = 0
    private val log = mutableListOf<String>()
    private var firstLog = 1L
    val identity = Identity(1, 0, "simulation-v1", FLASH_ID)
    companion object { const val FLASH_ID = "53494d554c415445442d504144303031" }

    init {
        require(fields.isNotEmpty() && defaults.values.all { it.isNotEmpty() })
        if (saved == null) seed() else load(saved)
        record("Simulated pad connected; no USB hardware is accessed")
    }
    private fun field(target: Int, id: Int) = fields.firstOrNull { it.kind == Protocol.kind(target) && it.id == id }
    private fun field(target: Int, name: String) = fields.first { it.kind == Protocol.kind(target) && it.name == name }
    private fun seed() {
        fun set(target: Int, name: String, text: String) { val f = field(target, name); overrides[target to f.id] = f.parse(text) }
        set(11, "name", "Demo game")
        set(11, "rules", "r4 held turbo_modifier a+b@15")
        set(11, "left_curve_points", "concave")
        set(11, "left_curve_enabled", "enabled")
        val bond = field(0, "bond")
        overrides[0 to bond.id] = ByteArray(bond.size) { if (it < 6) (it + 1).toByte() else 0 }
    }
    private fun load(saved: String) {
        runCatching {
            val entries = JSONArray(saved)
            val parsed = linkedMapOf<Pair<Int, Int>, ByteArray>()
            repeat(entries.length()) {
                val entry = entries.getJSONArray(it); val target = entry.getInt(0); val id = entry.getInt(1)
                val bytes = unhex(entry.getString(2)); val f = field(target, id)
                if (target in 0..18 && f != null && valid(f, bytes)) parsed[target to id] = bytes
            }
            overrides.putAll(parsed)
        }.onFailure { seed() }
    }
    private fun save() {
        val entries = JSONArray()
        overrides.forEach { (key, bytes) -> entries.put(JSONArray().put(key.first).put(key.second).put(bytes.hex())) }
        persist(entries.toString())
    }
    private fun record(text: String) {
        log += "[simulation] $text\n"
        if (log.size > 200) { log.removeAt(0); firstLog++ }
    }
    private fun default(target: Int, f: Field): ByteArray {
        if (target in 10..17 && f.name != "name" && !(target == 10 && f.name in listOf("map", "rules")))
            return value(18, f)
        return defaults.getValue(target).getValue(f.id).bytes
    }
    private fun value(target: Int, f: Field): ByteArray {
        val bytes = overrides[target to f.id] ?: default(target, f)
        return if (target < 8 && f.name == "address" && bytes.all { it == 0.toByte() }) original.getValue(target).getValue(f.id).bytes else bytes
    }
    private fun read(target: Int, onlyDefaults: Boolean): ByteArray {
        require(target in 0..18)
        val fs = fields.filter { it.kind == Protocol.kind(target) }
        return ByteArrayOutputStream().apply {
            write(byteArrayOf(1, target.toByte(), fs.size.toByte()))
            for (f in fs) {
                val bytes = if (onlyDefaults) default(target, f) else value(target, f)
                write(byteArrayOf(f.id.toByte(), if (!onlyDefaults && overrides.containsKey(target to f.id)) 1 else 0, bytes.size.toByte()))
                write(bytes)
            }
        }.toByteArray()
    }
    private fun valid(f: Field, bytes: ByteArray): Boolean {
        if (bytes.size != f.size) return false
        return runCatching {
            if (f.kind == 3 && f.name == "map") require(bytes.indices.all { i -> bytes.u8(i).let { it == 255 || it <= 20 && it != 16 && (i != 15 || it == 0) } })
            if (f.kind == 3 && f.name == "rules") for (at in bytes.indices step 10) {
                val trigger = bytes.u8(at)
                if (trigger != 0) require(trigger in 1..6 && bytes.u8(at + 1) in Remap.keys.indices && Remap.keys[bytes.u8(at + 1)].isNotEmpty() && bytes.u8(at + 3) in Remap.effects.indices)
            }
            f.parse(f.show(bytes)); true
        }.getOrDefault(false)
    }
    private fun write(bytes: ByteArray) {
        val codes = mutableListOf<Byte>()
        var at = 0
        while (at + 3 <= bytes.size) {
            val target = bytes.u8(at); val id = bytes.u8(at + 1); val size = bytes.u8(at + 2)
            if (at + 3 + size > bytes.size) { codes += 2; break }
            val raw = bytes.copyOfRange(at + 3, at + 3 + size); val f = field(target, id)
            val code = when {
                target !in 0..18 || id == 0xfe && target >= 8 -> 1
                id == 0 || id == 0xfe -> if (size == 0) 0 else 2
                f == null -> 1
                size != 0 && size != f.size -> 2
                size != 0 && !valid(f, raw) -> 3
                else -> 0
            }.let { if (it == 0 && staged.sumOf { edit -> edit.bytes.size + 3 } + size + 3 > 1024) 4 else it }
            if (code == 0) staged += Edit(target, id, raw)
            codes += code.toByte(); at += 3 + size
        }
        results = byteArrayOf(codes.size.toByte()) + codes.toByteArray()
    }
    private fun renew(target: Int) {
        val address = field(target, "address"); val bond = field(target, "bond")
        val raw = value(target, address).copyOf()
        renewals++; raw[0] = (raw.u8(0) + renewals).toByte(); raw[5] = ((raw.u8(5) or 2) and 254).toByte()
        overrides[target to address.id] = raw; overrides.remove(target to bond.id)
    }
    private fun commit() {
        for (edit in staged) {
            if (edit.id == 0xfe) { renew(edit.target); continue }
            if (edit.id == 0) {
                overrides.keys.removeAll { it.first == edit.target && field(edit.target, it.second)?.name !in listOf("address", "bond") }
                continue
            }
            val f = field(edit.target, edit.id)!!
            if (edit.target < 8 && f.name in listOf("type", "transport")) {
                val old = value(edit.target, f).u8(0)
                val next = if (edit.bytes.isEmpty()) default(edit.target, f).u8(0) else edit.bytes.u8(0)
                val transport = value(edit.target, field(edit.target, "transport")).u8(0)
                fun dongle(n: Int) = n == 3 || n == 4
                if (f.name == "type" && old != next && !dongle(transport) || f.name == "transport" && dongle(old) != dongle(next)) renew(edit.target)
            }
            if (edit.bytes.isEmpty()) overrides.remove(edit.target to edit.id) else overrides[edit.target to edit.id] = edit.bytes.copyOf()
        }
        record("Committed ${staged.size} settings changes")
        staged.clear(); save()
    }
    private fun status() = ByteArray(16).apply {
        this[0] = state.toByte(); this[1] = slot.toByte(); this[2] = 1
        this[4] = 0x68; this[5] = 0x10 // 4200 mV
        le32((now() - started).coerceAtLeast(0)).copyInto(this, 8); this[13] = staged.size.toByte()
    }
    private fun input() = ByteArray(36).apply {
        this[0] = 1; this[1] = 1
        val t = (now() - started) / 1000.0
        val key = listOf(0, 1, 6, 7, 16, 17, 18, 19)[((t / 2).toInt() % 8).coerceAtLeast(0)]
        le32(1L shl key).copyInto(this, 4)
        fun put16(at: Int, value: Int) { this[at] = value.toByte(); this[at + 1] = (value shr 8).toByte() }
        val sticks = listOf(sin(t), cos(t), sin(t * .7), cos(t * .7)).map { (it * 28000).toInt() }
        sticks.forEachIndexed { i, value -> put16(8 + i * 2, value); put16(20 + i * 2, (value + 32768) / 16) }
        put16(16, ((sin(t) + 1) * 16383).toInt()); put16(18, ((cos(t) + 1) * 16383).toInt())
        put16(28, ((sin(t) + 1) * 2047).toInt()); put16(30, ((cos(t) + 1) * 2047).toInt())
        le32((now() - started).coerceAtLeast(0)).copyInto(this, 32)
    }
    private fun calibrate() {
        val f = field(9, "calibration")
        val bytes = ByteArray(f.size)
        fun word(i: Int, n: Int) { bytes[i*2] = n.toByte(); bytes[i*2+1] = (n shr 8).toByte() }
        for (base in listOf(0, 24)) {
            for (i in 0..1) { word(base+i, 100); word(base+2+i, 2048); word(base+4+i, 4000) }
            for (i in 6..21) word(base+i, 32767)
        }
        for (base in listOf(48, 52)) { word(base, 4000); word(base+1, 100) }
        overrides[9 to f.id] = bytes; save(); record("Synthetic sensor calibration completed")
    }
    @Synchronized override fun transfer(cdb: ByteArray, length: Int, out: ByteArray?, timeout: Int, rawLength: Boolean): ByteArray {
        require(cdb.size >= 2 && cdb.u8(0) == 0xf5) { "Simulation accepts padtool commands only" }
        val arg = if (cdb.size >= 6) cdb.u32(2) else 0
        val result = when (val sub = cdb.u8(1)) {
            0 -> ByteArray(64).apply {
                "C2PD".toByteArray().copyInto(this); this[4] = 1
                identity.build.toByteArray().copyInto(this, 8); unhex(identity.flashId).copyInto(this, 24)
            }
            1 -> status()
            2 -> {
                val from = maxOf(arg, firstLog)
                val lines = log.drop((from - firstLog).toInt().coerceIn(0, log.size))
                var text = ""
                for (line in lines) { if ((text + line).toByteArray().size > 504) break; text += line }
                val raw = text.toByteArray(); le32(minOf(from, firstLog + log.size)) + le32(raw.size.toLong()) + raw
            }
            4 -> input()
            0x10 -> { state = 1; staged.clear(); record("Restarted simulated pad"); ByteArray(0) }
            0x11 -> error("Flashing is unavailable for the simulated pad")
            0x12 -> { record("Simulated relink"); ByteArray(0) }
            0x13, 0x17 -> {
                slot = (arg.toInt() and 255).let { if (it == 254) slot else it.coerceIn(0, 7) }
                state = if (sub == 0x17) 3 else 4
                record("${if (sub == 0x17) "Pairing" else "Connected"} slot ${Protocol.targetNames[slot]}"); ByteArray(0)
            }
            0x18 -> { calibrate(); ByteArray(0) }
            0x19 -> {
                val index = arg.toInt() and 255; val names = listOf("Idle", "Connecting", "Rainbow", "Heartbeat")
                if (arg.toInt() and 0x100 != 0 || index == 255) record("LED demo: ${names.getOrElse(index) { "stopped" }}")
                (names.getOrElse(index) { "" } + "\u0000").toByteArray()
            }
            0x1a -> { record("Rumble: left ${arg and 255}, right ${(arg shr 8) and 255}"); ByteArray(0) }
            0x20 -> descriptions.getOrElse(arg.toInt()) { ByteArray(0) }
            0x21 -> read(arg.toInt(), false)
            0x27 -> read(arg.toInt(), true)
            0x22 -> { write(requireNotNull(out)); ByteArray(0) }
            0x26 -> results
            0x23 -> { commit(); ByteArray(0) }
            0x24 -> { staged.clear(); ByteArray(0) }
            else -> error("Command 0x${sub.toString(16)} is not simulated")
        }
        return if (out != null || length == 0) ByteArray(0) else result.copyOfRange(0, minOf(result.size, length))
    }
}
