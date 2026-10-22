package dev.cyclone.padtool

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Handler
import android.os.Looper
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.ConcurrentHashMap

class PadSession(val identity: Identity, val fields: List<Field>) {
    var values: Map<Int, Map<Int, Value>> = emptyMap()
    var defaults: Map<Int, Map<Int, Value>> = emptyMap()
    val drafts = Drafts()
    var status = ""
    var needsRefresh = false
}

/** Retained across activity recreation; only this executor touches USB. */
class AppModel(context: Context) {
    val context: Context = context.applicationContext
    val usb = context.getSystemService(UsbManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val preferences = context.getSharedPreferences("drafts", Context.MODE_PRIVATE)
    var changed: (() -> Unit)? = null
    var progressChanged: (() -> Unit)? = null
    var busy = false; private set
    @Volatile var message = "Connect the pad with a USB data cable. End its gamepad session to use tool mode."
    var devices = emptyList<UsbDevice>()
    var selected: UsbDevice? = null
    var simulation: SimulatedPad? = null; private set
    val hasDevice get() = selected != null || simulation != null
    var session: PadSession? = null
    val sessions: MutableMap<String, PadSession> = ConcurrentHashMap()
    var loaderMode = false
    var page = -1
    var developer = false
    var console = ""
    var live: String? = null
    var logSequence = 1L
    var documentAction = ""
    var documentTarget = -1
    var documentIdentity: String? = null
    var pendingImage: ByteArray? = null
    var flashDeviceId: Int? = null
    var loaderPid: String? = null
    val loaderFile get() = File(context.filesDir, "br23loader.bin")

    fun refreshDevices() {
        devices = usb.deviceList.values.filter { UsbTransport.massStorage(it) != null }.sortedBy { it.deviceName }
        val current = selected
        if (current != null && devices.none { it.deviceId == current.deviceId }) {
            selected = null; live = null; loaderMode = false; loaderPid = null
            message = "USB device disconnected. Pending edits are retained."
        }
        changed?.invoke()
    }
    fun run(label: String, block: () -> Unit) {
        if (busy) return
        busy = true; message = label; changed?.invoke()
        worker.execute {
            var error: Exception? = null
            try { block() } catch (e: Exception) { error = e }
            handler.post {
                busy = false
                message = error?.let { "${it.message ?: it.javaClass.simpleName}" } ?: "$label — done"
                changed?.invoke()
            }
        }
    }
    fun progress(stage: String, done: Int, total: Int) {
        val text = if (total > 0) "$stage · ${done * 100L / total}%" else stage
        if (text == message) return
        message = text
        handler.post { progressChanged?.invoke() }
    }
    fun <T> transport(block: (UsbTransport) -> T): T {
        require(simulation == null) { "USB operations are unavailable in simulation mode" }
        val device = selected ?: error("Select the reconnected USB device first")
        require(usb.deviceList[device.deviceName]?.deviceId == device.deviceId) { "USB device changed; select it again" }
        return UsbTransport.open(usb, device).use(block)
    }
    fun <T> withPad(block: (Protocol, PadSession) -> T): T {
        val simulated = simulation
        return if (simulated != null) checkedPad(simulated, block) else transport { checkedPad(it, block) }
    }
    private fun <T> checkedPad(tr: Scsi, block: (Protocol, PadSession) -> T): T {
        val pad = Protocol(tr); val s = session ?: error("Read the device first")
        require(pad.identity() == s.identity) { "Device identity changed; select it again" }
        return block(pad, s)
    }
    fun load(device: UsbDevice) {
        if (busy) return
        simulation = null
        selected = device; session = null; loaderMode = false; pendingImage = null
        page = -1; live = null; console = ""; logSequence = 1; loaderPid = null
        run("Reading device") {
            transport { tr ->
                val inquiry = tr.transfer(byteArrayOf(0x12, 0, 0, 0, 36), 36)
                require(inquiry.size >= 32 && inquiry.copyOfRange(8, 12).cString() == "BR23") { "This is not a BR23 tool device" }
                loaderMode = inquiry.copyOfRange(16, 32).cString().startsWith("UBOOT")
                if (loaderMode) { session = null; return@transport }
                val pad = Protocol(tr); val id = pad.identity()
                require(id.protocol == 1) { "Unsupported firmware protocol ${id.protocol}" }
                val existing = sessions[id.flashId]
                val s = if (existing?.identity == id) existing else PadSession(id, if (id.board == 0) pad.fields() else emptyList())
                read(pad, s)
                if (existing == null) loadDrafts(s)
                session = s; sessions[id.flashId] = s
            }
        }
    }
    fun loadSimulation(reset: Boolean = false) {
        require(BuildConfig.DEBUG) { "Simulation is available in debug builds only" }
        if (busy) return
        if (reset) {
            preferences.edit().remove("simulation-state").remove(SimulatedPad.FLASH_ID).apply()
            sessions.remove(SimulatedPad.FLASH_ID)
        }
        selected = null; loaderMode = false; pendingImage = null; loaderPid = null
        page = -1; live = null; console = ""; logSequence = 1
        run("Opening simulated pad") {
            val sim = SimulatedPad(context.assets.open("simulated-pad.json").bufferedReader().use { it.readText() },
                preferences.getString("simulation-state", null),
                { preferences.edit().putString("simulation-state", it).apply() })
            val pad = Protocol(sim)
            val s = sessions[sim.identity.flashId] ?: PadSession(sim.identity, pad.fields()).also { loadDrafts(it) }
            read(pad, s)
            simulation = sim; session = s; sessions[s.identity.flashId] = s
        }
    }
    fun disconnectSimulation() {
        if (busy) return
        simulation = null; session = null; live = null; page = -2
        message = "Simulated pad disconnected. Pending edits are retained."
        changed?.invoke()
    }
    fun read(pad: Protocol, s: PadSession) {
        if (s.identity.board == 0) {
            val values = Protocol.targets(s.fields).associateWith { pad.read(it) }
            val defaults = Protocol.targets(s.fields).associateWith { pad.read(it, true) }
            for (t in values.keys) for (f in s.fields.filter { it.kind == Protocol.kind(t) }) {
                require(values[t]?.get(f.id)?.bytes?.size == f.size && defaults[t]?.get(f.id)?.bytes?.size == f.size) { "Missing or invalid setting ${f.name}" }
            }
            s.values = values; s.defaults = defaults
        }
        s.status = status(pad.command(1, length = 16)); s.needsRefresh = false
    }
    fun saveDrafts(s: PadSession) {
        val edits = JSONArray()
        s.drafts.edits.forEach { (key, value) -> edits.put(JSONArray().put(key.first).put(key.second).put(value.hex())) }
        preferences.edit().putString(s.identity.flashId, JSONObject().put("identity", s.identity.toString()).put("edits", edits).toString()).apply()
    }
    private fun loadDrafts(s: PadSession) {
        runCatching {
            val saved = JSONObject(preferences.getString(s.identity.flashId, "{}")!!)
            if (saved.optString("identity") != s.identity.toString()) return
            val edits = saved.getJSONArray("edits")
            repeat(edits.length()) {
                val e = edits.getJSONArray(it); val t = e.getInt(0); val id = e.getInt(1); val raw = unhex(e.getString(2))
                if (t in Protocol.targets(s.fields) && (id == 0xfe && t < 8 && raw.isEmpty() || s.fields.any { f -> f.id == id && f.kind == Protocol.kind(t) && (raw.isEmpty() || raw.size == f.size) })) s.drafts.edits[t to id] = raw
            }
        }
    }
    fun apply(target: Int?) {
        val s = session ?: return
        if (s.needsRefresh) { message = "Refresh settings before another Apply"; changed?.invoke(); return }
        val items = s.drafts.items(s.fields, target)
        run("Applying settings") {
            withPad { pad, current ->
                try {
                    pad.send(current.identity, items) {
                        current.drafts.discard(target); saveDrafts(current)
                    }
                } catch (e: Exception) { current.needsRefresh = true; throw e }
                // A lost read after a successful COMMIT must not re-stage Unpair.
                current.needsRefresh = true
                read(pad, current)
            }
        }
    }
    fun stage(s: PadSession, edits: List<Edit>) {
        for (edit in edits) {
            val field = s.fields.first { it.kind == Protocol.kind(edit.target) && it.id == edit.id }
            s.drafts.stage(edit.target, field, edit.bytes, s.values.getValue(edit.target).getValue(edit.id))
        }
        saveDrafts(s); changed?.invoke()
    }
    fun poll() {
        val mode = live ?: return
        if (busy || !hasDevice) return
        busy = true
        worker.execute {
            var text = ""
            var failed: String? = null
            try {
                withPad { pad, _ ->
                    text = if (mode == "console") {
                        val raw = pad.command(2, logSequence, 512)
                        require(raw.size >= 8 && raw.u32(4) <= raw.size - 8) { "Invalid log response" }
                        val value = raw.copyOfRange(8, 8 + raw.u32(4).toInt()).toString(Charsets.UTF_8)
                        logSequence = raw.u32() + value.lines().let { if (value.isEmpty()) 0 else it.size - if (value.endsWith('\n')) 1 else 0 }
                        value
                    } else input(pad.command(4, length = 36))
                }
            } catch (e: Exception) { failed = e.message }
            handler.post {
                busy = false
                if (failed != null) { live = null; message = failed!!; changed?.invoke() }
                else {
                    console = if (mode == "console") (console + text).takeLast(32000) else text
                    progressChanged?.invoke()
                }
            }
        }
    }
    fun close() { worker.shutdown() }
    companion object {
        fun status(raw: ByteArray): String {
            require(raw.size >= 16) { "Short status" }
            val state = listOf("boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting").getOrElse(raw.u8(0)) { "unknown" }
            return "$state · slot ${raw.u8(1)} · ${raw.u16(4)} mV · uptime ${raw.u32(8) / 1000}s" + if (raw.u8(12) != 0) " · ephemeral (not saved to flash)" else ""
        }
        fun input(raw: ByteArray): String {
            require(raw.size >= 36 && raw.u8(0) == 1) { "Invalid input response" }
            fun signed(at: Int) = raw.u16(at).toShort().toInt()
            val keys = Remap.keys.indices.filter { Remap.keys[it].isNotEmpty() && raw.u32(4) and (1L shl it) != 0L }.joinToString(" ") { Remap.keys[it].uppercase() }.ifEmpty { "—" }
            return "Keys: $keys\n\nLeft: ${signed(8)}, ${signed(10)}\nRight: ${signed(12)}, ${signed(14)}\nTriggers: ${raw.u16(16)}, ${raw.u16(18)}\n\nRaw sticks: ${(20..26 step 2).joinToString { raw.u16(it).toString() }}\nRaw triggers: ${raw.u16(28)}, ${raw.u16(30)}"
        }
    }
}
