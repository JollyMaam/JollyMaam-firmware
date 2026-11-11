package dev.cyclone.padtool

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
import android.webkit.JavascriptInterface
import android.webkit.WebView
import org.json.JSONArray
import org.json.JSONObject
import java.io.Closeable
import java.io.IOException
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
        } catch (e: Exception) {
            fail(call, "Error", e.message ?: e.toString())
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
        return mapOf("handle" to handle, "name" to (device.productName ?: "USB pad"), "reportSize" to link.reportSize)
    }

    @JavascriptInterface fun usbChoose(call: Int) = async(call) {
        val found = tools()
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

    /** Pads in config mode or their updater, as they are found; the one chosen. */
    @JavascriptInterface fun bleChoose(call: Int) = async(call) {
        val scanner = bluetooth().bluetoothLeScanner ?: throw IOException("Bluetooth is off: turn it on.")
        val found = mutableListOf<BluetoothDevice>()
        val chooser = activity.liveChooser("Pads in config mode (hold Home+M for 3 s)")
        val scan = object : ScanCallback() {
            override fun onScanResult(type: Int, r: ScanResult) = activity.runOnUiThread {
                if (found.none { it.address == r.device.address }) {
                    found += r.device
                    chooser.add("${r.scanRecord?.deviceName ?: r.device.name ?: "Pad"}\n${r.device.address}")
                }
            }
            override fun onScanFailed(code: Int) = activity.runOnUiThread { chooser.add("(scanning failed: error $code)") }
        }
        val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(BleLink.SERVICE)).build()) +
            BleLink.NAME_ONLY.map { ScanFilter.Builder().setDeviceName(it).build() }
        scanner.startScan(filters, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scan)
        val chosen = try { chooser.await() } finally { scanner.stopScan(scan) }
        val device = chosen?.let { found.getOrNull(it) } ?: throw NotChosen("No pad chosen.")
        openBle(device, 5.0)
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
     * [product, board, chip] each.  Kept for flashRun. */
    @JavascriptInterface fun flashPackage(call: Int, data: String) = async(call) {
        val bytes = unbase64(data)
        val raw = NativeCore.products(bytes) ?: throw IOException("not a firmware package, or a file in it is damaged")
        pkg = bytes
        raw.toList().chunked(3).map { mapOf("product" to it[0], "board" to it[1], "chip" to it[2]) }
    }

    /** What is on USB that the flasher can talk to: our tool device (kind 0),
     * the 8BitDo boot loader (1), a Jieli ROM loader (2). */
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

    private fun transport(device: UsbDevice, kind: Int, progress: (Long, Long) -> Unit = { _, _ -> }) =
        AndroidNativeUsb(usb, device, kind, activity::usbPermission, cancelled, progress)

    /** What the device is: ours (product, build, flash ID, updater or app),
     * the 8BitDo loader (its model checked), a Jieli loader (its chip). */
    @JavascriptInterface fun flashProbe(call: Int, name: String) = async(call) {
        val (device, kind) = device(name)
        val reply = transport(device, kind).use { NativeCore.probe(it, kind) }
            ?: throw IOException("it did not answer (is it in the mode it showed?)")
        when (kind) {
            0 -> mapOf("kind" to 0, "build" to reply.copyOfRange(8, 24).cString(), "uid" to reply.copyOfRange(24, 40).hex(),
                "product" to reply.copyOfRange(40, 56).cString(), "updater" to (reply.u8(7) == 1))
            1 -> {
                if (reply.u16() != 0x301b) throw IOException("an 8BitDo boot loader of another model")
                mapOf("kind" to 1, "product" to "U2C_WIRELESS")
            }
            else -> mapOf("kind" to 2, "chip" to reply.cString())
        }
    }

    /** The package's firmware for product onto the device: through our
     * updater (kind 0, the pad found again by flash ID after its restart),
     * else through the vendor loader.  Progress as events on handle 0. */
    @JavascriptInterface fun flashRun(call: Int, name: String, product: String, uid: String) = async(call) {
        val bytes = pkg ?: throw IOException("choose a firmware package first")
        val (device, kind) = device(name)
        var last = -1L
        val result = transport(device, kind) { done, total ->
            val percent = if (total > 0) done * 100 / total else 0
            if (percent != last) { last = percent; event(0, "progress", mapOf("done" to done, "total" to total)) }
        }.use { NativeCore.run(bytes, product, kind, if (kind == 0) unhex(uid) else null, it) }
        if (result != 0) throw IOException("failed ($result): the device may stay in its loader; scan again before trying again")
        null
    }

    /** Our firmware or updater into its vendor loader (it leaves USB; scan again). */
    @JavascriptInterface fun flashStockLoader(call: Int, name: String, uid: String) = async(call) {
        val (device, kind) = device(name)
        if (kind != 0) throw IOException("only our firmware or updater takes that request")
        val result = transport(device, 0).use { NativeCore.stockLoader(unhex(uid), it) }
        if (result != 0) throw IOException("unconfirmed ($result): scan again before trying again")
        null
    }
}
