package dev.jollymaam.app

import java.io.Closeable
import java.io.IOException

/** JNI entry points own no Android handles and retain no arrays after returning. */
object NativeCore {
    init { System.loadLibrary("jollymaam-jni") }
    external fun stockLoader(uid: ByteArray, transport: NativeUsb): Int
    external fun products(packageBytes: ByteArray): Array<String>?
    external fun probe(transport: NativeUsb, kind: Int): ByteArray?
    external fun run(packageBytes: ByteArray, product: String, kind: Int, uid: ByteArray?, transport: NativeUsb): Int
}

/** Synchronous callbacks are invoked only from the updater worker thread. */
abstract class NativeUsb : Closeable {
    open fun scsi(cdb: ByteArray, direction: Int, data: ByteArray, timeout: Int): Int = throw IOException("No SCSI interface")
    open fun hidWrite(data: ByteArray): Int = throw IOException("No HID output")
    open fun hidRead(data: ByteArray, timeout: Int): Int = throw IOException("No HID input")
    open fun featureSet(data: ByteArray): Int = throw IOException("No feature output")
    open fun featureSize(): Int = 0
    open fun waitMillis(ms: Int) { if (ms > 0) Thread.sleep(ms.toLong()) }
    open fun reconnect(uid: ByteArray): Boolean = false
    open fun onProgress(done: Long, total: Long) {}
    /** A line of what libjollymaam does (its say hook). */
    open fun onSay(line: String) {}
}
