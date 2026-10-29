package se.sanctorum.vigil

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.FileNotFoundException

class OpenProvider : ContentProvider() {
    override fun onCreate(): Boolean {
        context?.let { Files.wipe(it) }
        return true
    }

    override fun getType(uri: Uri): String = Files.type(uri.lastPathSegment ?: "")

    override fun query(
        uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?,
        sortOrder: String?
    ): Cursor? {
        val file = Files.file(context ?: return null, uri) ?: return null
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        val row = columns.map {
            when (it) {
                OpenableColumns.DISPLAY_NAME -> uri.lastPathSegment
                OpenableColumns.SIZE -> file.length()
                else -> null
            }
        }

        return MatrixCursor(columns).apply { addRow(row) }
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") {
            throw FileNotFoundException("read only")
        }

        val file = Files.file(context ?: throw FileNotFoundException(), uri) ?: throw FileNotFoundException()

        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int = 0

    override fun update(
        uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?
    ): Int = 0
}
