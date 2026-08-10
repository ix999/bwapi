package com.openbw.replays

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException

/**
 * Manages the StarCraft data files the engine reads from.
 *
 * These are Blizzard's copyrighted archives, so the app cannot ship them; the
 * user imports their own copy once and it stays in app-private storage. openbw
 * opens them by exact name, and Android's filesystem is case-sensitive, so
 * imports are written under [CANONICAL_NAMES] regardless of how the source file
 * was capitalised.
 */
class GameData(context: Context) {

    private val appContext = context.applicationContext

    val directory: File = File(appContext.filesDir, "gamedata")

    /** Exactly the names openbw's data_files_directory() looks for. */
    val missing: List<String>
        get() = CANONICAL_NAMES.filter { !File(directory, it).isFile }

    val isComplete: Boolean
        get() = missing.isEmpty()

    /**
     * Copies [uri] into the data directory if it is one of the required
     * archives. Returns the canonical name it was stored as, or null if the
     * file is not one the engine needs.
     */
    @Throws(IOException::class)
    fun import(uri: Uri): String? {
        val sourceName = displayName(uri) ?: return null
        val canonical = CANONICAL_NAMES.firstOrNull { it.equals(sourceName, ignoreCase = true) }
            ?: return null

        if (!directory.isDirectory && !directory.mkdirs()) {
            throw IOException("could not create ${directory.absolutePath}")
        }

        // Write to a temporary file first so an interrupted copy cannot leave a
        // truncated archive behind that would later fail deep inside the engine.
        val target = File(directory, canonical)
        val temp = File(directory, "$canonical.part")
        try {
            appContext.contentResolver.openInputStream(uri)?.use { input ->
                temp.outputStream().use { output -> input.copyTo(output) }
            } ?: throw IOException("could not open $sourceName")

            if (!hasMpqSignature(temp)) {
                throw IOException("$sourceName is not an MPQ archive")
            }
            if (!temp.renameTo(target)) {
                throw IOException("could not store $canonical")
            }
        } finally {
            temp.delete()
        }
        return canonical
    }

    fun clear() {
        CANONICAL_NAMES.forEach { File(directory, it).delete() }
    }

    private fun displayName(uri: Uri): String? {
        appContext.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0 && cursor.moveToFirst()) return cursor.getString(index)
        }
        return uri.lastPathSegment?.substringAfterLast('/')
    }

    /** MPQ archives begin with "MPQ". */
    private fun hasMpqSignature(file: File): Boolean {
        val header = ByteArray(4)
        file.inputStream().use { input ->
            if (input.read(header) != header.size) return false
        }
        return header[0] == 'M'.code.toByte() &&
            header[1] == 'P'.code.toByte() &&
            header[2] == 'Q'.code.toByte() &&
            header[3] == 0x1A.toByte()
    }

    companion object {
        /**
         * Capitalisation matters: these are the literal strings openbw builds
         * its paths from in data_loading.h.
         */
        val CANONICAL_NAMES = listOf("Patch_rt.mpq", "BrooDat.mpq", "StarDat.mpq")
    }
}
