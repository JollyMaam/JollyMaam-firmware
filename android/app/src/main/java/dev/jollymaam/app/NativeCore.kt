package dev.jollymaam.app

import java.io.Closeable
import java.io.IOException

/** JNI entry points own no Android handles and retain no arrays after returning. */
object NativeCore {
    init { System.loadLibrary("jollymaam-jni") }
    external fun stockLoader(uid: ByteArray, transport: NativeUsb): Int
    external fun products(packageBytes: ByteArray): Array<String>?
    external fun probe(transport: NativeUsb, kind: Int): ByteArray?
    /** install: our firmware (kind 0) gets the whole install image through
     * its vendor loader, not update.bin through our updater. */
    external fun run(packageBytes: ByteArray, product: String, kind: Int, uid: ByteArray?, install: Boolean, transport: NativeUsb): Int
    /** The whole flash to transport.backupBlock, through the vendor loader
     * (our firmware, uid given, is restarted into it and stays there). */
    external fun backup(packageBytes: ByteArray, kind: Int, uid: ByteArray?, transport: NativeUsb): Int
    /** A vendor loader (kind 1, 2) starts the firmware it holds. */
    external fun start(packageBytes: ByteArray, kind: Int, transport: NativeUsb): Int
}

/** Synchronous callbacks are invoked only from the updater worker thread. */
abstract class NativeUsb : Closeable {
    open fun scsi(cdb: ByteArray, direction: Int, data: ByteArray, timeout: Int): Int = throw IOException("No SCSI interface")
    open fun hidWrite(data: ByteArray): Int = throw IOException("No HID output")
    open fun hidRead(data: ByteArray, timeout: Int): Int = throw IOException("No HID input")
    open fun featureSet(data: ByteArray): Int = throw IOException("No feature output")
    open fun featureSize(): Int = 0
    open fun waitMillis(ms: Int) { if (ms > 0) Thread.sleep(ms.toLong()) }
    /** The device now, at each look at USB: its kind (0..2), -1 when none.
     * The one opened while it stays; after a restart what took its place
     * (our firmware only with uid, when given), with a new generation(). */
    open fun follow(uid: ByteArray?, was: Int): Int = was
    open fun generation(): Int = 0
    /** A block of a backup, at address: false stops it. */
    open fun backupBlock(address: Long, data: ByteArray): Boolean = false
    open fun onProgress(done: Long, total: Long) {}
    /** A line of what libjollymaam does (its say hook). */
    open fun onSay(line: String) {}
}
