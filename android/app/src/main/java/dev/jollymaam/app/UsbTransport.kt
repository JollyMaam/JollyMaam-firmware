package dev.jollymaam.app

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import java.io.Closeable
import java.io.IOException

class UsbTransport private constructor(private val connection: UsbDeviceConnection, private val intf: UsbInterface, input: UsbEndpoint, output: UsbEndpoint) : Closeable, Scsi {
    private val scsi = BulkScsi(object : BulkPipe {
        override fun read(bytes: ByteArray, timeout: Int) = connection.bulkTransfer(input, bytes, bytes.size, timeout)
        override fun write(bytes: ByteArray, timeout: Int) = connection.bulkTransfer(output, bytes, bytes.size, timeout)
        override fun reset() {
            // Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(HALT) on both endpoints.
            connection.controlTransfer(0x21, 0xff, 0, intf.id, null, 0, 1000)
            for (ep in listOf(input, output)) connection.controlTransfer(0x02, 1, 0, ep.address, null, 0, 1000)
        }
    })
    override fun transfer(cdb: ByteArray, length: Int, out: ByteArray?, timeout: Int, rawLength: Boolean) = scsi.transfer(cdb, length, out, timeout, rawLength)
    override fun close() { connection.releaseInterface(intf); connection.close() }
    companion object {
        fun massStorage(device: UsbDevice): UsbInterface? = (0 until device.interfaceCount).map { device.getInterface(it) }.firstOrNull {
            it.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE && it.interfaceSubclass == 6 && it.interfaceProtocol == 0x50 &&
                (0 until it.endpointCount).map(it::getEndpoint).let { eps ->
                    eps.any { ep -> ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK && ep.direction == UsbConstants.USB_DIR_IN } &&
                        eps.any { ep -> ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK && ep.direction == UsbConstants.USB_DIR_OUT }
                }
        }
        fun open(manager: UsbManager, device: UsbDevice): UsbTransport {
            require(manager.hasPermission(device)) { "USB access has not been granted" }
            val intf = massStorage(device) ?: throw IOException("No tool mass-storage interface; end the gamepad session first")
            val conn = manager.openDevice(device) ?: throw IOException("Cannot open USB device")
            try {
                if (!conn.claimInterface(intf, true)) throw IOException("Cannot claim USB interface")
                val eps = (0 until intf.endpointCount).map(intf::getEndpoint).filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
                return UsbTransport(conn, intf, eps.first { it.direction == UsbConstants.USB_DIR_IN }, eps.first { it.direction == UsbConstants.USB_DIR_OUT })
            } catch (e: Exception) { conn.close(); throw e }
        }
    }
}
