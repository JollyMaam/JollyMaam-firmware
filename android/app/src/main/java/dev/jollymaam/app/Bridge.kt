package dev.jollymaam.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.ParcelUuid
import android.util.Log
import android.webkit.JavascriptInterface
import android.webkit.WebView
import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/** What the page (web/link-android.js) calls as window.JollyMaamAndroid.
 *
 * Raw links only, as WebHID and Web Bluetooth give them: the framing stays
 * in the page.  A call takes the page's call number and returns at once;
 * the answer comes as jmAndroid.done(call, error, value), and what a link
 * hears as jmAndroid.event(handle, type, value).  Bytes go as base64.
 *
 * And the flasher: libjollymaam over JNI (NativeCore) for what the page
 * cannot do itself, a vendor loader (the Jieli ROM loader is USB mass
 * storage). */
@SuppressLint("MissingPermission")   // asked for in bluetooth() before any scan or link
class Bridge(private val activity: MainActivity, private val web: WebView) : Closeable {
    private class NotChosen(message: String) : Exception(message)

    private val usb = activity.getSystemService(UsbManager::class.java)
    private val work = Executors.newCachedThreadPool()
    private val links = ConcurrentHashMap<Int, Closeable>()
    private val handles = AtomicInteger(1)
    private val cancelled = AtomicBoolean(false)
    @Volatile private var pkg: ByteArray? = null

    // ---- to the page ----

    private fun json(v: Any?): String = when (v) {
        null, Unit -> "null"
        is String -> JSONObject.quote(v)
        is Boolean, is Number -> v.toString()
        is Map<*, *> -> JSONObject(v).toString()
        is List<*> -> JSONArray(v).toString()
        else -> JSONObject.quote(v.toString())
    }

    private fun js(code: String) { web.post { web.evaluateJavascript(code, null) } }

    private fun done(call: Int, value: Any?) = js("jmAndroid.done($call,null,${json(value)})")

    private fun fail(call: Int, name: String, message: String) =
        js("jmAndroid.done($call,${json(mapOf("name" to name, "message" to message))},null)")

    private fun event(handle: Int, type: String, value: Any?) = js("jmAndroid.event($handle,${json(type)},${json(value)})")

    /** The answer to a call, worked out off the page's thread. */
    private fun async(call: Int, block: () -> Any?) = work.execute {
        try {
            done(call, block())
        } catch (e: NotChosen) {
            fail(call, "NotFoundError", e.message ?: "")
        } catch (e: Throwable) {   // an Error too (a JNI one): the page gets an answer
            Log.w("JollyMaam", "call $call", e)
            fail(call, "Error", e.message?.takeIf { it.isNotBlank() } ?: e.toString())
        }
    }

    private fun answer(call: Int): (String?) -> Unit = { error -> if (error == null) done(call, null) else fail(call, "Error", error) }

    @JavascriptInterface fun version() = 1

    @JavascriptInterface fun closeLink(handle: Int) { links.remove(handle)?.close() }

    override fun close() {
        cancelled.set(true)
        links.values.forEach { it.close() }
        links.clear()
        work.shutdownNow()
    }

    // ---- USB: the tool device ----

    private fun held() = links.values.filterIsInstance<HidLink>().map { it.device.deviceName }.toSet()

    private fun tools() = usb.deviceList.values.filter { ToolHid.isTool(it) && it.deviceName !in held() }.sortedBy { it.deviceName }

    private fun openHid(device: UsbDevice): Map<String, Any> {
        if (!activity.usbPermission(device)) throw IOException("USB access to ${device.productName} was not granted")
        val handle = handles.getAndIncrement()
        val link = HidLink.open(usb, device, { event(handle, "input", it.base64()) }) {
            links.remove(handle)
            event(handle, "gone", null)
        }
        links[handle] = link
        return mapOf("handle" to handle, "name" to (device.productName ?: "USB pad"), "reportSize" to link.reportSize,
            "device" to device.deviceName)
    }

    /** device: the one to take (from the flasher's scan); "": the only
     * one, or the user chooses. */
    @JavascriptInterface fun usbChoose(call: Int, device: String) = async(call) {
        val found = tools().filter { device.isEmpty() || it.deviceName == device }
        if (found.isEmpty()) throw IOException("No pad on USB: plug it in (through an OTG adapter or a USB-C cable).")
        val device = if (found.size == 1) found[0]
        else activity.pick("Choose your pad", found.map { "${it.productName ?: "USB pad"}\n${it.deviceName}" })?.let(found::get)
        openHid(device ?: throw NotChosen("No pad chosen."))
    }

    /** The same pad again after a restart: a tool device that comes up within
     * the time, the same board first (the updater's name ends " Update"); the
     * page checks the flash ID. */
    @JavascriptInterface fun usbReconnect(call: Int, name: String, seconds: Double) = async(call) {
        val end = System.currentTimeMillis() + (seconds * 1000).toLong()
        val board = { n: String? -> (n ?: "").removeSuffix(" Update") }
        val failed = mutableSetOf<String>()
        while (true) {
            for (device in tools().sortedByDescending { board(it.productName) == board(name) }) {
                if (device.deviceName in failed) continue
                try {
                    return@async openHid(device)
                } catch (e: IOException) {
                    failed += device.deviceName
                }
            }
            if (System.currentTimeMillis() > end) {
                throw IOException("the pad did not come back on USB in ${seconds.toInt()} s")
            }
            Thread.sleep(250)
        }
        @Suppress("UNREACHABLE_CODE") null
    }

    @JavascriptInterface fun usbSend(call: Int, handle: Int, report: String) {
        val link = links[handle] as? HidLink ?: return fail(call, "Error", "the pad went away")
        link.send(unbase64(report), answer(call))
    }

    /** A USB device left: its link ends. */
    fun detached(device: UsbDevice) {
        links.values.filterIsInstance<HidLink>().filter { it.device.deviceName == device.deviceName }.forEach { it.gone() }
    }

    // ---- Bluetooth ----

    private companion object {
        /** A pad heard again is told to the page at most this often. */
        const val FOUND_MS = 500L
    }

    private val adapter get() = activity.getSystemService(BluetoothManager::class.java)?.adapter

    @JavascriptInterface fun bleAvailable() = adapter != null

    private fun bluetooth() = adapter.also {
        if (it == null) throw IOException("This phone has no Bluetooth.")
        if (!activity.bluetoothPermission()) throw IOException("Bluetooth access was not granted.")
        if (!it.isEnabled) throw IOException("Bluetooth is off: turn it on.")
    }!!

    private fun openBle(device: BluetoothDevice, seconds: Double): Map<String, Any> {
        val handle = handles.getAndIncrement()
        val link = BleLink.open(activity, device, seconds, { event(handle, "reply", it.base64()) }) {
            links.remove(handle)?.close()
            event(handle, "gone", null)
        }
        links[handle] = link
        return mapOf("handle" to handle, "name" to (device.name ?: "Bluetooth pad"), "id" to device.address, "mtu" to link.mtu)
    }

    /** Scanning and connecting allowed, asked for if need be: true or false. */
    @JavascriptInterface fun blePermission(call: Int) = async(call) { activity.bluetoothPermission() }

    /** Pads in config mode or their updater while they advertise: events
     * "found" {name, id} on the scan's handle, again at most every
     * FOUND_MS per pad while it is heard, until the page closes it
     * (closeLink); the page shows them and drops the ones gone quiet. */
    @JavascriptInterface fun bleScan(call: Int) = async(call) {
        val scanner = bluetooth().bluetoothLeScanner ?: throw IOException("Bluetooth is off: turn it on.")
        val handle = handles.getAndIncrement()
        val heard = HashMap<String, Long>()
        val scan = object : ScanCallback() {
            override fun onScanResult(type: Int, r: ScanResult) {
                val now = System.currentTimeMillis()
                val again = synchronized(heard) {
                    val last = heard[r.device.address]
                    if (last != null && now - last < FOUND_MS) false else { heard[r.device.address] = now; true }
                }
                if (!again) return
                event(handle, "found", mapOf("name" to (r.scanRecord?.deviceName ?: r.device.name ?: "Pad"), "id" to r.device.address))
            }
            override fun onScanFailed(code: Int) = event(handle, "failed", "Scanning failed (error $code).")
        }
        val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(BleLink.SERVICE)).build()) +
            BleLink.NAME_ONLY.map { ScanFilter.Builder().setDeviceName(it).build() }
        scanner.startScan(filters, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scan)
        links[handle] = Closeable { runCatching { scanner.stopScan(scan) } }
        mapOf("handle" to handle)
    }

    /** The pad the page chose from the scan. */
    @JavascriptInterface fun bleOpen(call: Int, address: String) = async(call) {
        openBle(bluetooth().getRemoteDevice(address), 5.0)
    }

    /** The same pad again (a restart into its updater keeps the address). */
    @JavascriptInterface fun bleReconnect(call: Int, handle: Int, address: String, seconds: Double) = async(call) {
        (links.remove(handle) as? BleLink)?.close()
        openBle(bluetooth().getRemoteDevice(address), seconds)
    }

    @JavascriptInterface fun bleWrite(call: Int, handle: Int, frame: String, quick: Boolean) {
        val link = links[handle] as? BleLink ?: return fail(call, "Error", "the pad went away")
        link.write(unbase64(frame), quick, answer(call))
    }

    @JavascriptInterface fun bleQuiet(call: Int, handle: Int) {
        val link = links[handle] as? BleLink ?: return done(call, null)
        link.quiet(answer(call))
    }

    // ---- the flasher: libjollymaam ----

    /** A firmware package (firmware.zip), checked: what it has, a
     * [product, board, chip, name, byHand] each.  Kept for flashRun. */
    @JavascriptInterface fun flashPackage(call: Int, data: String) = async(call) {
        val bytes = unbase64(data)
        val raw = NativeCore.products(bytes) ?: throw IOException("not a firmware package, or a file in it is damaged")
        pkg = bytes
        raw.toList().chunked(5).map { mapOf("product" to it[0], "board" to it[1], "chip" to it[2], "name" to it[3], "byHand" to it[4]) }
    }

    /** What is on USB that the flasher can talk to: our tool device (kind 0),
     * the 8BitDo boot loader (1), a Jieli ROM loader (2), a stock pad (3). */
    @JavascriptInterface fun flashScan(call: Int) = async(call) {
        usb.deviceList.values.filter { AndroidNativeUsb.kind(it) != null && it.deviceName !in held() }.sortedBy { it.deviceName }.map {
            mapOf("device" to it.deviceName, "name" to (it.productName ?: "USB device"),
                "id" to "%04x:%04x".format(it.vendorId, it.productId), "kind" to AndroidNativeUsb.kind(it))
        }
    }

    private fun device(name: String): Pair<UsbDevice, Int> {
        val device = usb.deviceList[name] ?: throw IOException("the device went away: scan again")
        val kind = AndroidNativeUsb.kind(device) ?: throw IOException("not a device the flasher knows")
        if (!activity.usbPermission(device)) throw IOException("USB access was not granted")
        return device to kind
    }

    /** libjollymaam's lines go to the page as they come (events on handle
     * 0, as progress); a backup's blocks to sink. */
    private fun transport(device: UsbDevice, kind: Int, progress: (String, Long, Long) -> Unit = { _, _, _ -> },
                          sink: ((Long, ByteArray) -> Boolean)? = null) =
        AndroidNativeUsb(usb, device, kind, activity::usbPermission, cancelled, progress, { event(0, "log", it) }, sink)

    /** A failure with the library's last line, which says why. */
    private fun failed(t: AndroidNativeUsb, what: String) = IOException(t.said.lastOrNull()?.let { "$what: $it" } ?: what)

    /** What the device is (libjollymaam's scan): kind, what, product, board,
     * name, chip, mode (stock), build, uid, updater, canEnter, byHand. */
    @JavascriptInterface fun flashProbe(call: Int, name: String) = async(call) {
        val (device, kind) = device(name)
        val t = transport(device, kind)
        val r = t.use { NativeCore.probe(it, kind) } ?: throw failed(t, "not a pad the app knows, or it did not answer")
        mapOf("kind" to kind, "what" to r[0], "product" to r[1], "board" to r[2], "name" to r[3], "chip" to r[4],
            "mode" to r[5], "build" to r[6], "uid" to r[7], "updater" to (r[8] == "1"), "canEnter" to (r[9] == "1"),
            "byHand" to r[10])
    }

    /** Progress as events on handle 0, one per percent of each step. */
    private fun progress(): (String, Long, Long) -> Unit {
        var last = -1L
        var step = ""
        return { what, done, total ->
            val percent = if (total > 0) done * 100 / total else 0
            if (percent != last || what != step) {
                last = percent; step = what
                event(0, "progress", mapOf("what" to what, "done" to done, "total" to total))
            }
        }
    }

    /** update.bin through our updater (our firmware, uid). */
    @JavascriptInterface fun flashUpdate(call: Int, name: String, product: String, uid: String) = async(call) {
        val bytes = pkg ?: throw IOException("choose a firmware package first")
        val (device, kind) = device(name)
        if (kind != 0) throw IOException("only our firmware takes an update")
        val t = transport(device, kind, progress())
        val result = t.use { NativeCore.update(bytes, product, unhex(uid), it) }
        if (result != 0) throw failed(t, "failed ($result); scan again before trying again")
        null
    }

    /** One pad session (NativeCore.session): the whole flash backed up first
     * into Downloads (before Android 10: the app's own files) when backup,
     * then the action; image (base64) for a restore.  {backup: where} back. */
    @JavascriptInterface fun flashSession(call: Int, name: String, uid: String, backup: Boolean, label: String,
                                          action: Int, image: String, raw: Boolean) = async(call) {
        val bytes = pkg ?: throw IOException("choose a firmware package first")
        val (device, kind) = device(name)
        val file = if (!backup) null else BackupFile.create(activity, "${label.replace(Regex("[^A-Za-z0-9_.-]"), "_")}-" +
            SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(Date()) + ".bin")
        var next: Long? = null
        val t = transport(device, kind, progress()) { at, block ->
            // Blocks come in order, from the first address read (not 0 on the 8BitDo pads).
            if (file == null || (next != null && at != next)) false
            else { file.out.write(block); next = at + block.size; true }
        }
        val data = if (action == NativeCore.ACTION_RESTORE) unbase64(image) else null
        var where: String? = null
        val result = try {
            t.use { NativeCore.session(bytes, kind, if (uid.isEmpty()) null else unhex(uid), backup, action, data, raw, it) }
        } catch (e: Throwable) { file?.discard(); throw e }
        // A backup read whole is kept, also when the step after it failed.
        if (file != null) { if (result == 0 || result == -4) where = file.keep() else file.discard() }
        if (result != 0) {
            val step = when (result) { -1 -> "the pad"; -2 -> "another pad"; -3 -> "backup failed"; -4 -> "failed"; else -> "failed ($result)" }
            throw failed(t, if (where != null) "$step (backup kept: $where)" else step)
        }
        mapOf("backup" to where)
    }
}
