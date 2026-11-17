package dev.jollymaam.app

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import java.io.Closeable
import java.io.IOException
import java.util.concurrent.Executors

/** The tool device's HID interface (16c0:05df, usage page 0xffc2): what the
 * page's link-hid.js opens through WebHID, here through Android's USB host.
 * Raw: feature reports out, input reports up; web/frames-hid.js frames. */
object ToolHid {
    const val VENDOR = 0x16c0
    const val PRODUCT = 0x05df
    const val USAGE_PAGE = 0xffc2

    fun isTool(device: UsbDevice) = device.vendorId == VENDOR && device.productId == PRODUCT

    /** A report descriptor's usage page and largest feature and input report
     * in bytes: jm_hid_descriptor (src/lib/device.c), with inputs. */
    data class Sizes(val usagePage: Int, val feature: Int, val input: Int)

    fun sizes(d: ByteArray, n: Int = d.size): Sizes {
        var page = 0; var size = 0L; var count = 0L; var id = 0
        val feature = LongArray(256); val input = LongArray(256)
        var i = 0
        while (i < n) {
            val b = d.u8(i)
            if (b == 0xfe) { i += 3 + (if (i + 1 < n) d.u8(i + 1) else 0); continue }   // a long item
            val k = intArrayOf(0, 1, 2, 4)[b and 3]
            var v = 0L
            for (j in 0 until k) if (i + 1 + j < n) v = v or (d.u8(i + 1 + j).toLong() shl (8 * j))
            when (b and 0xfc) {
                0x04 -> if (page == 0) page = v.toInt()
                0x74 -> size = v
                0x94 -> count = v
                0x84 -> id = (v and 255).toInt()
                0xb0 -> feature[id] += size * count
                0x80 -> input[id] += size * count
            }
            i += 1 + k
        }
        return Sizes(page, (feature.max() / 8).toInt(), (input.max() / 8).toInt())
    }

    /** The interface's report descriptor, read through the open connection. */
    fun sizes(conn: UsbDeviceConnection, intf: UsbInterface): Sizes? {
        val d = ByteArray(1024)
        val n = conn.controlTransfer(0x81, 6, 0x2200, intf.id, d, d.size, 1000)
        return if (n > 0) sizes(d, n) else null
    }

    /** The claimed tool interface, or null (the connection's interfaces released). */
    fun claim(device: UsbDevice, conn: UsbDeviceConnection): Pair<UsbInterface, Sizes>? {
        for (i in 0 until device.interfaceCount) {
            val intf = device.getInterface(i)
            if (intf.interfaceClass != UsbConstants.USB_CLASS_HID || !conn.claimInterface(intf, true)) continue
            val s = sizes(conn, intf)
            if (s != null && s.usagePage == USAGE_PAGE && s.feature > 0) return intf to s
            conn.releaseInterface(intf)
        }
        return null
    }
}

/** One open tool device: a thread reads its input reports, another sends
 * feature reports in order. */
class HidLink private constructor(
    val device: UsbDevice,
    private val conn: UsbDeviceConnection,
    private val intf: UsbInterface,
    private val input: UsbEndpoint,
    val reportSize: Int,
    private val onInput: (ByteArray) -> Unit,
    private val onGone: () -> Unit,
) : Closeable {
    @Volatile private var open = true
    private val sender = Executors.newSingleThreadExecutor()
    private val reader = Thread({
        val buffer = ByteArray(maxOf(64, input.maxPacketSize))
        while (open) {
            val n = conn.bulkTransfer(input, buffer, buffer.size, 250)
            if (n > 0 && open) onInput(buffer.copyOf(n))
        }
    }, "hid-input").apply { isDaemon = true }

    /** A feature report (no report ID: the tool has none), as long as given:
     * the pad goes by the request's length. */
    fun send(report: ByteArray, done: (String?) -> Unit) {
        if (!open) return done("the pad went away")
        sender.execute {
            val n = if (open) conn.controlTransfer(0x21, 9, 0x0300, intf.id, report, report.size, 2000) else -1
            done(if (n == report.size) null else if (open) "the pad did not take the feature report" else "the pad went away")
        }
    }

    /** The device left (unplugged, or restarted): closed, and the page told. */
    fun gone() {
        if (!open) return
        close()
        onGone()
    }

    override fun close() {
        if (!open) return
        open = false
        sender.shutdown()
        reader.join(500)
        conn.releaseInterface(intf)
        conn.close()
    }

    companion object {
        fun open(manager: UsbManager, device: UsbDevice, onInput: (ByteArray) -> Unit, onGone: () -> Unit): HidLink {
            val conn = manager.openDevice(device) ?: throw IOException("${device.productName}: cannot open it")
            try {
                val (intf, sizes) = ToolHid.claim(device, conn)
                    ?: throw IOException("${device.productName}: not a tool device (no feature report on page 0xffc2)")
                val input = (0 until intf.endpointCount).map(intf::getEndpoint).firstOrNull {
                    it.type == UsbConstants.USB_ENDPOINT_XFER_INT && it.direction == UsbConstants.USB_DIR_IN
                }
                if (input == null || sizes.input < 64) {
                    conn.releaseInterface(intf)
                    throw IOException("${device.productName}: the tool device has no input report " +
                        "(firmware from before 2026-10-27: update it through its boot loader)")
                }
                return HidLink(device, conn, intf, input, sizes.feature, onInput, onGone).also { it.reader.start() }
            } catch (e: Exception) { conn.close(); throw e }
        }
    }
}
