package dev.cyclone.padtool

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.hardware.usb.UsbRequest
import java.io.IOException
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/** Android's endpoint implementation; all protocol decisions remain in C. */
class AndroidNativeUsb(
    private val manager: UsbManager,
    device: UsbDevice,
    val kind: Int,
    private val permission: (UsbDevice) -> Boolean,
    private val cancelled: AtomicBoolean,
    private val progress: (Long, Long) -> Unit
) : NativeUsb() {
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
    private fun interrupt(endpoint: UsbEndpoint, data: ByteArray, timeout: Int, reading: Boolean): Int {
        checkActive()
        val conn = connection ?: throw IOException("Device disconnected")
        val request = UsbRequest()
        if (!request.initialize(conn, endpoint)) { request.close(); return -1 }
        try {
            val buffer = ByteBuffer.allocateDirect(data.size)
            if (!reading) { buffer.put(data); buffer.flip() }
            if (!request.queue(buffer)) return -1
            if (conn.requestWait(timeout.toLong()) !== request) return -1
            val n = buffer.position()
            if (reading) { buffer.flip(); buffer.get(data, 0, n) }
            return n
        } finally { request.cancel(); request.close() }
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
        return transferScsi(bulk ?: throw IOException("No mass-storage connection"), cdb, direction, data, timeout)
    }
    override fun reconnect(uid: ByteArray): Boolean {
        checkActive(); close()
        for (device in manager.deviceList.values.filter { kind(it) == 0 }) {
            if (!manager.hasPermission(device) && !permission(device)) continue
            try {
                open(device)
                val identity = NativeCore.probe(this, 0)
                if (identity != null && identity.copyOfRange(24, 40).contentEquals(uid)) return true
            } catch (_: IOException) { checkActive() }
            close()
        }
        return false
    }
    override fun onProgress(done: Long, total: Long) { progress(done, total) }
    companion object {
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
