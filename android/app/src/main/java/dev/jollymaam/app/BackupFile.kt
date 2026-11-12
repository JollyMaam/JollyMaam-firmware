package dev.jollymaam.app

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream

/** A pad's flash backup as it is read: in Downloads/JollyMaam (Android 10
 * and later, no permission needed), else in the app's own external files.
 * Kept whole or not at all. */
class BackupFile private constructor(
    private val context: Context, val out: OutputStream, private val uri: Uri?, private val file: File?, private val where: String
) {
    /** Done: closed and visible; where it is, for the page. */
    fun keep(): String {
        out.close()
        uri?.let { context.contentResolver.update(it, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null) }
        return where
    }

    fun discard() {
        runCatching { out.close() }
        uri?.let { runCatching { context.contentResolver.delete(it, null, null) } }
        file?.delete()
    }

    companion object {
        fun create(context: Context, name: String): BackupFile {
            if (Build.VERSION.SDK_INT >= 29) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, name)
                    put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream")
                    put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/JollyMaam")
                    put(MediaStore.Downloads.IS_PENDING, 1)
                }
                val resolver = context.contentResolver
                val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: throw IOException("cannot create $name in Downloads")
                val out = resolver.openOutputStream(uri) ?: throw IOException("cannot write $name")
                return BackupFile(context, out.buffered(), uri, null, "Download/JollyMaam/$name")
            }
            val dir = context.getExternalFilesDir(null) ?: context.filesDir
            val file = File(dir, name)
            return BackupFile(context, FileOutputStream(file).buffered(), null, file, file.absolutePath)
        }
    }
}
