package dev.jollymaam.app

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.util.Log
import java.io.IOException
import java.util.concurrent.atomic.AtomicBoolean

/** Android's endpoint implementation; all protocol decisions remain in C. */
class AndroidNativeUsb(
    private val manager: UsbManager,
    private var device: UsbDevice,
    kind: Int,
    private val permission: (UsbDevice) -> Boolean,
    private val cancelled: AtomicBoolean,
    private val progress: (Long, Long) -> Unit,
    private val say: (String) -> Unit = {},
    /** A backup's blocks, in order. */
    private val sink: ((Long, ByteArray) -> Boolean)? = null
) : NativeUsb() {
    var kind = kind
        private set
    private var generation = 0
    private var connection: UsbDeviceConnection? = null
    private var intf: UsbInterface? = null
    private var input: UsbEndpoint? = null
    private var output: UsbEndpoint? = null
    private var bulk: UsbTransport? = null
    private var report = 0
    init { open(device) }
    private fun checkActive() {
        if (cancelled.get() || Thread.currentThread().isInterrupted) throw IOException("Update cancelled; recovery may be required")
    }
    private fun open(device: UsbDevice) {
        checkActive()
        if (!manager.hasPermission(device)) throw IOException("USB permission has not been granted")
        if (kind == 2) { bulk = UsbTransport.open(manager, device); return }
        val conn = manager.openDevice(device) ?: throw IOException("Cannot open USB device")
        try {
            if (kind == 0) {
                val (candidate, sizes) = ToolHid.claim(device, conn) ?: throw IOException("No HID tool interface")
                report = sizes.feature
                use(conn, candidate)
                return
            }
            for (i in 0 until device.interfaceCount) {
                val candidate = device.getInterface(i)
                if (candidate.interfaceClass != UsbConstants.USB_CLASS_HID || !conn.claimInterface(candidate, true)) continue
                if (use(conn, candidate)) return
                conn.releaseInterface(candidate)
            }
            throw IOException("No supported HID interface")
        } catch (e: Exception) { conn.close(); throw e }
    }
    /** The interface's interrupt endpoints; the 8BitDo loader needs an IN one. */
    private fun use(conn: UsbDeviceConnection, candidate: UsbInterface): Boolean {
        val endpoints = (0 until candidate.endpointCount).map(candidate::getEndpoint).filter { it.type == UsbConstants.USB_ENDPOINT_XFER_INT }
        input = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_IN }
        output = endpoints.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT }
        if (kind == 1 && input == null) return false
        intf = candidate; connection = conn
        return true
    }
    @Synchronized override fun close() {
        bulk?.close(); bulk = null
        connection?.let { conn -> intf?.let { conn.releaseInterface(it) }; conn.close() }
        connection = null; intf = null; input = null; output = null
    }
    override fun featureSize() = report
    override fun featureSet(data: ByteArray): Int {
        checkActive()
        val payload = data.copyOfRange(1, data.size)
        val n = connection?.controlTransfer(0x21, 9, 0x300, intf!!.id, payload, payload.size, 10000) ?: -1
        return if (n < 0) -1 else n + 1
    }
    // bulkTransfer also moves interrupt transfers, and its timeout cancels
    // the transfer before it returns (as HidLink reads).  Not UsbRequest:
    // after requestWait's timeout, the cancelled request comes back to a
    // later wait and native code writes into it after it was freed (SIGSEGV).
    // -1 is a timeout or an error alike: for a read, the library's 0.
    // A read asks for one packet: a full packet does not end a longer
    // transfer, so the read would wait for the next report and lose this one.
    private fun interrupt(endpoint: UsbEndpoint, data: ByteArray, timeout: Int, reading: Boolean): Int {
        checkActive()
        val conn = connection ?: throw IOException("Device disconnected")
        val length = if (reading) minOf(data.size, endpoint.maxPacketSize) else data.size
        val n = conn.bulkTransfer(endpoint, data, length, timeout)
        return if (n < 0 && reading) 0 else n
    }
    override fun hidWrite(data: ByteArray): Int {
        checkActive()
        return output?.let { interrupt(it, data, 10000, false) }
            ?: connection?.controlTransfer(0x21, 9, 0x200, intf!!.id, data, data.size, 10000) ?: -1
    }
    override fun hidRead(data: ByteArray, timeout: Int): Int {
        if (timeout == 0) return 0
        return input?.let { interrupt(it, data, timeout, true) } ?: -1
    }
    override fun scsi(cdb: ByteArray, direction: Int, data: ByteArray, timeout: Int): Int {
        checkActive()
        // A failure is a failed command for the library (JNI clears it): logged here.
        try { return transferScsi(bulk ?: throw IOException("No mass-storage connection"), cdb, direction, data, timeout) }
        catch (e: IOException) { Log.w("JollyMaam", "SCSI %02x: %s".format(cdb.u8(0), e.message)); throw e }
    }
    private fun opened() = connection != null || bulk != null
    override fun generation() = generation
    /** The pad across its restarts: into our updater (found by UID), into
     * its vendor loader (the 8BitDo one, or a Jieli one: by its vendor ID,
     * not any USB disk). */
    override fun follow(uid: ByteArray?, was: Int): Int {
        checkActive()
        if (opened() && manager.deviceList[device.deviceName] != null) return this.kind
        close()
        val candidates = manager.deviceList.values.filter {
            val k = kind(it)
            k == 0 || k == 1 || (k == 2 && it.vendorId == JIELI)
        }.sortedBy { kind(it) == 0 }   // loaders first
        for (next in candidates) {
            if (!manager.hasPermission(next) && !permission(next)) continue
            val k = kind(next)!!
            try {
                this.kind = k; device = next; open(next)
                generation++
                if (k == 0 && uid != null) {
                    val identity = NativeCore.probe(this, 0)
                    if (identity == null || !identity.copyOfRange(24, 40).contentEquals(uid)) { close(); continue }
                }
                return k
            } catch (_: IOException) { checkActive() }
            close()
        }
        return -1
    }
    override fun backupBlock(address: Long, data: ByteArray) = sink?.invoke(address, data) ?: false
    override fun onProgress(done: Long, total: Long) { progress(done, total) }
    /** libjollymaam's lines: logcat, and kept for the Flash panel. */
    val said = mutableListOf<String>()
    override fun onSay(line: String) {
        Log.i("JollyMaam", line.trim())
        said += line.trim()
        say(line.trim())
    }
    companion object {
        const val JIELI = 0x4c4a   // the ROM loaders: 4c4a:2342 BR23UBOOT1.00, 4c4a:3442 BD19
        internal fun transferScsi(transport: Scsi, cdb: ByteArray, direction: Int, data: ByteArray, timeout: Int): Int {
            // Jieli helpers report bogus BOT residue; still require every actual
            // input/output byte. Standard INQUIRY retains normal residue handling.
            val loader = cdb.isNotEmpty() && cdb.u8(0) in 0xfb..0xfd
            val response = transport.transfer(cdb, if (direction == 1) data.size else 0,
                if (direction == 2) data else null, timeout, rawLength = loader)
            if (direction == 1) response.copyInto(data)
            return if (direction == 2) data.size else response.size
        }
        fun kind(device: UsbDevice): Int? = when {
            ToolHid.isTool(device) -> 0
            device.vendorId == 0x2dc8 && device.productId == 0x3208 -> 1
            UsbTransport.massStorage(device) != null -> 2
            else -> null
        }
    }
}
