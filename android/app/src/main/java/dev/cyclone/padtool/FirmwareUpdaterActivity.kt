package dev.cyclone.padtool

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** A basic package updater backed by the same C core as the desktop app. */
class FirmwareUpdaterActivity : Activity() {
    private data class Target(val product: String, val board: String, val chip: String)
    private lateinit var usb: UsbManager
    private lateinit var content: LinearLayout
    private lateinit var status: TextView
    private val worker = Executors.newSingleThreadExecutor()
    private val cancelled = AtomicBoolean(false)
    private val permissions = ConcurrentHashMap<Int, CountDownLatch>()
    private val permissionAction get() = "$packageName.UPDATER_USB_PERMISSION"
    private var bundle: ByteArray? = null
    private var targets = emptyList<Target>()
    private var busy = false
    private var pendingPrompt: (() -> Unit)? = null
    @Volatile private var transport: AndroidNativeUsb? = null
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == permissionAction) {
                @Suppress("DEPRECATION") val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                device?.let { permissions[it.deviceId]?.countDown() }
            } else if (!busy) renderDevices()
        }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        usb = getSystemService(UsbManager::class.java)
        val layout = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(24, 32, 24, 24) }
        status = TextView(this).apply { text = "Select a JollyMaam firmware ZIP. Nothing is written until you confirm an update." }
        layout.addView(status)
        layout.addView(Button(this).apply {
            text = "Choose firmware ZIP"
            setOnClickListener { if (!busy) startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type = "*/*"; addCategory(Intent.CATEGORY_OPENABLE) }, 201) }
        })
        layout.addView(Button(this).apply { text = "Rescan USB"; setOnClickListener { if (!busy) renderDevices() } })
        content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        layout.addView(ScrollView(this).apply { addView(content) }, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(layout)
        val filter = IntentFilter(permissionAction).apply { addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED); addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED) else registerReceiver(receiver, filter)
        renderDevices()
    }
    private fun task(message: String, block: () -> Unit) {
        if (busy) return
        busy = true; pendingPrompt = null; status.text = message; renderDevices()
        worker.execute {
            var success = false
            try { block(); success = true } catch (e: Exception) { ui { status.text = e.message ?: e.javaClass.simpleName } }
            finally {
                val prompt = if (success) pendingPrompt else null
                pendingPrompt = null
                ui { busy = false; renderDevices(); prompt?.invoke() }
            }
        }
    }
    private fun ui(block: () -> Unit) { runOnUiThread { if (!isFinishing && !isDestroyed) block() } }
    private fun permission(device: UsbDevice): Boolean {
        if (usb.hasPermission(device)) return true
        val latch = CountDownLatch(1); permissions[device.deviceId] = latch
        ui {
            usb.requestPermission(device, PendingIntent.getBroadcast(this, device.deviceId,
                Intent(permissionAction).setPackage(packageName), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE))
        }
        return try { latch.await(30, TimeUnit.SECONDS) && usb.hasPermission(device) }
        finally { permissions.remove(device.deviceId) }
    }
    private fun renderDevices() {
        content.removeAllViews()
        val devices = usb.deviceList.values.filter { AndroidNativeUsb.kind(it) != null }.sortedBy { it.deviceName }
        content.addView(TextView(this).apply { text = if (devices.isEmpty()) "No supported USB interfaces. Stock gamepad modes are not supported yet." else "Select a device to identify it. USB permission is required." })
        for (device in devices) content.addView(Button(this).apply {
            text = "${device.productName ?: "USB device"} · %04x:%04x\n${device.deviceName}".format(device.vendorId, device.productId)
            isEnabled = !busy && (bundle != null || AndroidNativeUsb.kind(device) == 0)
            setOnClickListener { identify(device) }
        })
    }
    @Deprecated("Activity document result bridge")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 201 || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        task("Verifying firmware package") {
            val bytes = contentResolver.openInputStream(uri)?.use { stream ->
                val out = ByteArrayOutputStream(); val buffer = ByteArray(8192)
                while (true) {
                    val n = stream.read(buffer); if (n < 0) break
                    require(out.size() + n <= 32 * 1024 * 1024) { "Package exceeds 32 MiB" }; out.write(buffer, 0, n)
                }
                out.toByteArray()
            } ?: throw IOException("Cannot read selected file")
            val raw = NativeCore.products(bytes) ?: throw IOException("Invalid firmware package or artifact hashes")
            val parsed = raw.toList().chunked(3).map { Target(it[0], it[1], it[2]) }
            ui { bundle = bytes; targets = parsed; status.text = "Verified package: ${parsed.size} targets. Select a USB device." }
        }
    }
    private fun identify(device: UsbDevice) {
        task("Requesting USB access and identifying device") {
            require(permission(device)) { "USB permission was not granted" }
            val kind = AndroidNativeUsb.kind(device) ?: throw IOException("Unsupported USB interface")
            val endpoint = AndroidNativeUsb(usb, device, kind, ::permission, cancelled) { _, _ -> }
            val reply = endpoint.use { NativeCore.probe(it, kind) ?: throw IOException("Device identification failed; check USB mode") }
            when (kind) {
                0 -> {
                    val product = reply.copyOfRange(40, 56).cString()
                    val target = targets.firstOrNull { it.product == product }
                    pendingPrompt = { customActions(device, target, reply) }
                }
                1 -> {
                    require(reply.u16() == 0x301b) { "Unsupported 8BitDo model" }
                    val target = targets.firstOrNull { it.product == "U2C_WIRELESS" } ?: throw IOException("No image for this pad")
                    pendingPrompt = { confirm(device, kind, target, null, "Stock bootloader") }
                }
                else -> {
                    val chip = reply.copyOfRange(8, 12).cString()
                    require(chip in listOf("BR23", "BD19") && reply.copyOfRange(16, 32).cString().startsWith("UBOOT1.00")) { "Not a supported Jieli bootloader" }
                    val choices = targets.filter { it.chip == chip }
                    require(choices.isNotEmpty()) { "No images for $chip" }
                    pendingPrompt = { AlertDialog.Builder(this).setTitle("Select intended product").setMessage(null)
                        .setItems(choices.map { it.board }.toTypedArray()) { _, index -> confirm(device, kind, choices[index], null, "Installed product will be checked before erasing") }
                        .setNegativeButton("Cancel", null).show() }
                }
            }
        }
    }
    private fun customActions(device: UsbDevice, target: Target?, identity: ByteArray) {
        val uid = identity.copyOfRange(24, 40)
        val build = identity.copyOfRange(8, 24).cString()
        val product = identity.copyOfRange(40, 56).cString()
        val dialog = AlertDialog.Builder(this).setTitle(product)
            .setMessage("$build\n${if (identity.u8(7) == 1) "Our updater" else "Our firmware"}\n${device.deviceName}")
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Stock bootloader") { _, _ ->
                AlertDialog.Builder(this).setTitle("Enter stock bootloader?")
                    .setMessage("Reboot $product into its original bootloader. This does not write firmware.")
                    .setNegativeButton("Cancel", null).setPositiveButton("Enter") { _, _ ->
                        task("Requesting stock bootloader") {
                            require(permission(device)) { "USB permission was not granted" }
                            val endpoint = AndroidNativeUsb(usb, device, 0, ::permission, cancelled) { _, _ -> }
                            val result = endpoint.use { NativeCore.stockLoader(uid, it) }
                            require(result == 0) { "Loader request unconfirmed ($result). Rescan before retrying." }
                            ui { status.text = "Stock bootloader requested. Rescan USB for its new interface." }
                        }
                    }.show()
            }
        if (target != null) dialog.setPositiveButton("Update") { _, _ -> confirm(device, 0, target, uid, build) }
        dialog.show()
    }
    private fun confirm(device: UsbDevice, kind: Int, target: Target, uid: ByteArray?, current: String) {
        AlertDialog.Builder(this).setTitle("Update ${target.board}?")
            .setMessage("$current\n${device.deviceName}\n\nThis rewrites firmware. Keep the USB connection attached until verification finishes.")
            .setNegativeButton("Cancel", null).setPositiveButton("Update") { _, _ -> update(device, kind, target, uid) }.show()
    }
    private fun update(device: UsbDevice, kind: Int, target: Target, uid: ByteArray?) {
        val bytes = bundle ?: return
        task("Updating ${target.board}") {
            require(permission(device)) { "USB permission was not granted" }
            var lastPercent = -1L
            val endpoint = AndroidNativeUsb(usb, device, kind, ::permission, cancelled) { done, total ->
                val percent = if (total > 0) done * 100 / total else 0
                if (percent != lastPercent) { lastPercent = percent; ui { status.text = "Updating ${target.board}: $percent%" } }
            }
            transport = endpoint
            val result = try { endpoint.use { NativeCore.run(bytes, target.product, kind, uid, it) } } finally { transport = null }
            require(result == 0) { "Update failed ($result). Device may remain in its loader; rescan before retrying." }
            ui { status.text = if (kind == 2) "Written and verified. Device remains in its loader." else "Written and verified. Firmware restart requested." }
        }
    }
    override fun onDestroy() {
        cancelled.set(true); permissions.values.forEach { it.countDown() }; worker.shutdownNow(); transport?.close()
        unregisterReceiver(receiver); super.onDestroy()
    }
}
