package dev.cyclone.padtool

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.os.Build
import java.io.Closeable
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

/** One GATT link to a pad in config mode or its updater
 * (src/profiles/profile_config.c, src/updater/): what the page's link-web.js
 * opens through Web Bluetooth.  Raw: frames written to the request
 * characteristic, notifications of the reply one handed up; pad.js frames.
 * Android runs one GATT operation at a time, so they queue on one thread. */
@SuppressLint("MissingPermission")   // the bridge asks for the permissions before any link
class BleLink private constructor(
    private val context: Context,
    val device: BluetoothDevice,
    private val onReply: (ByteArray) -> Unit,
    private val onGone: () -> Unit,
) : Closeable {
    companion object {
        val SERVICE: UUID = UUID.fromString("c2550000-3e2d-66b1-8a4f-d5037c2b419e")
        private val REQUEST = UUID.fromString("c2550001-3e2d-66b1-8a4f-d5037c2b419e")
        private val REPLY = UUID.fromString("c2550002-3e2d-66b1-8a4f-d5037c2b419e")
        private val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        /** Pads whose radio advertises only a name (the U2C Bluetooth's core). */
        val NAME_ONLY = listOf("U2C BT Config")
        private const val STEP_MS = 10000L
        /** The updater's (its log: "le mtu 512"); Android asks for 517 at most. */
        private const val MTU = 517

        /** A link, tried again for up to seconds: Android's first connect
         * often fails (status 133), and after a restart the pad takes a moment
         * to advertise again. */
        fun open(context: Context, device: BluetoothDevice, seconds: Double, onReply: (ByteArray) -> Unit, onGone: () -> Unit): BleLink {
            val end = System.currentTimeMillis() + (seconds * 1000).toLong()
            while (true) {
                val link = BleLink(context, device, onReply, onGone)
                try {
                    link.start()
                    return link
                } catch (e: IOException) {
                    link.close()
                    if (System.currentTimeMillis() > end) throw e
                    Thread.sleep(500)
                }
            }
        }
    }

    private val ops = Executors.newSingleThreadExecutor()
    @Volatile private var pending: CompletableFuture<Int>? = null
    @Volatile private var up = false
    @Volatile private var ready = false     // start() done: a disconnect from now on is the page's to hear
    @Volatile private var closed = false
    private var gatt: BluetoothGatt? = null
    private lateinit var request: BluetoothGattCharacteristic
    private lateinit var reply: BluetoothGattCharacteristic
    @Volatile var mtu = 23
        private set

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, state: Int) {
            if (state == BluetoothProfile.STATE_CONNECTED) return finished(status)
            if (state != BluetoothProfile.STATE_DISCONNECTED) return
            pending?.completeExceptionally(IOException("the link ended (status $status)"))
            up = false
            if (ready && !closed) onGone()
        }
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = finished(status)
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) this@BleLink.mtu = mtu
            finished(status)
        }
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) = finished(status)
        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) = finished(status)
        // Android 13 and newer call only this one; older ones only the other.
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (c.uuid == REPLY) onReply(value.clone())
        }
        @Deprecated("Android 12 and older")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION") val value = c.value ?: return
            if (c.uuid == REPLY) onReply(value.clone())
        }
    }

    private fun finished(status: Int) { pending?.complete(status) }

    /** One GATT operation: started, and its callback's status waited for. */
    private fun op(what: String, start: () -> Boolean): Int {
        val f = CompletableFuture<Int>()
        pending = f
        try {
            if (!start()) throw IOException("$what: Android would not start it")
            return f.get(STEP_MS, TimeUnit.MILLISECONDS)
        } catch (e: TimeoutException) {
            throw IOException("$what: no answer in ${STEP_MS / 1000} s")
        } catch (e: ExecutionException) {
            throw e.cause as? IOException ?: IOException("$what: ${e.cause?.message}")
        } finally {
            pending = null
        }
    }

    private fun check(what: String, status: Int) {
        if (status != BluetoothGatt.GATT_SUCCESS) throw IOException("$what failed (GATT status $status)")
    }

    private fun writeCccd(g: BluetoothGatt, value: ByteArray): Int {
        val d = reply.getDescriptor(CCCD) ?: throw IOException("the reply characteristic has no notifications")
        return op("notifications") {
            if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(d, value) == BluetoothStatusCodes.SUCCESS
            else @Suppress("DEPRECATION") run { d.value = value; g.writeDescriptor(d) }
        }
    }

    private fun start() {
        val g = op("connect") {
            gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
            gatt != null
        }.let { status -> check("connect", status); gatt!! }
        up = true
        check("service discovery", op("service discovery") { g.discoverServices() })
        op("MTU") { g.requestMtu(MTU) }          // the default (23) works too, slowly
        val service = g.getService(SERVICE) ?: throw IOException("${device.name}: no config service (not in config mode?)")
        request = service.getCharacteristic(REQUEST) ?: throw IOException("no request characteristic")
        reply = service.getCharacteristic(REPLY) ?: throw IOException("no reply characteristic")
        if (!g.setCharacteristicNotification(reply, true)) throw IOException("notifications: Android refused")
        check("notifications", writeCccd(g, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE))
        ready = true
    }

    /** A frame; quick: without response.  A frame longer than the MTU goes as
     * a long write (Android does that for writes with response). */
    fun write(frame: ByteArray, quick: Boolean, done: (String?) -> Unit) = queue(done) { g ->
        val type = if (quick) BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE else BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        check("write", op("write") {
            if (Build.VERSION.SDK_INT >= 33) g.writeCharacteristic(request, frame, type) == BluetoothStatusCodes.SUCCESS
            else @Suppress("DEPRECATION") run { request.writeType = type; request.value = frame; g.writeCharacteristic(request) }
        })
    }

    /** Before a command that ends the link: no subscription left behind. */
    fun quiet(done: (String?) -> Unit) = queue(done) { g ->
        g.setCharacteristicNotification(reply, false)
        writeCccd(g, BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
    }

    private fun queue(done: (String?) -> Unit, block: (BluetoothGatt) -> Unit) {
        if (closed) return done("the link is closed")
        try {
            ops.execute {
                val g = gatt
                try {
                    if (g == null || !up) throw IOException("the pad went away")
                    block(g)
                    done(null)
                } catch (e: Exception) {
                    done(e.message ?: e.toString())
                }
            }
        } catch (e: Exception) {
            done("the link is closed")
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        up = false
        ready = false
        pending?.completeExceptionally(IOException("closed"))
        ops.shutdownNow()
        gatt?.let { it.disconnect(); it.close() }
        gatt = null
    }
}
