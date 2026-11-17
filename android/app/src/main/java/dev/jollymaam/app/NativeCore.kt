package dev.jollymaam.app

import java.io.Closeable
import java.io.IOException

/** JNI entry points own no Android handles and retain no arrays after returning. */
object NativeCore {
    init { System.loadLibrary("jollymaam-jni") }
    external fun products(packageBytes: ByteArray): Array<String>?
    /** What the device is: [what, product ID, board, name, chip, stock mode,
     * build, flash ID (hex), "1" updater, "1" can enter its loader, by hand];
     * null: nothing known. */
    external fun probe(transport: NativeUsb, kind: Int): Array<String>?
    /** update.bin through our updater (our firmware with uid). */
    external fun update(packageBytes: ByteArray, product: String, uid: ByteArray, transport: NativeUsb): Int
    /** One pad session through its vendor loader: with backup the whole
     * flash to transport.backupBlock first, then action (ACTION_*).  0 done;
     * -1 the pad, -2 another pad, -3 the backup, -4 the action failed. */
    external fun session(packageBytes: ByteArray, kind: Int, uid: ByteArray?, backup: Boolean, action: Int,
                         image: ByteArray?, raw: Boolean, transport: NativeUsb): Int
    /** A stock pad's USB identity (libjollymaam's table). */
    external fun isStock(vid: Int, pid: Int): Boolean
    const val ACTION_NONE = 0     // it stays in its loader
    const val ACTION_INSTALL = 1
    const val ACTION_RESTORE = 2
    const val ACTION_REBOOT = 3
}

/** Synchronous callbacks are invoked only from the updater worker thread. */
abstract class NativeUsb : Closeable {
    open fun scsi(cdb: ByteArray, direction: Int, data: ByteArray, timeout: Int): Int = throw IOException("No SCSI interface")
    open fun hidWrite(data: ByteArray): Int = throw IOException("No HID output")
    open fun hidRead(data: ByteArray, timeout: Int): Int = throw IOException("No HID input")
    open fun featureSet(data: ByteArray): Int = throw IOException("No feature output")
    open fun featureSize(): Int = 0
    /** A stock pad (kind 3): [vid, pid, bcdDevice (hex), manufacturer, product]. */
    open fun identity(): Array<String> = arrayOf("0", "0", "0", "", "")
    /** Its HID interfaces, each one's report descriptor, and the one used. */
    open fun hidInterfaces(): Int = 0
    open fun hidDescriptor(index: Int): ByteArray? = null
    open fun hidSelect(index: Int): Boolean = false
    open fun waitMillis(ms: Int) { if (ms > 0) Thread.sleep(ms.toLong()) }
    /** The device now, at each look at USB: its kind (0..3), -1 when none.
     * The one opened while it stays; after a restart what took its place
     * (our firmware only with uid, when given), with a new generation(). */
    open fun follow(uid: ByteArray?, was: Int): Int = was
    open fun generation(): Int = 0
    /** A block of a backup, at address: false stops it. */
    open fun backupBlock(address: Long, data: ByteArray): Boolean = false
    /** what: libjollymaam's word for the step ("reading", "writing", ...). */
    open fun onProgress(what: String, done: Long, total: Long) {}
    /** A line of what libjollymaam does (its say hook). */
    open fun onSay(line: String) {}
}
