package com.openbw.replays

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException

/**
 * The on-device replay library.
 *
 * Replays picked through the document picker are copied into app-private
 * storage so the engine can open them as ordinary files; a SAF content URI is
 * not something native code can pass to fopen(), and the permission behind it
 * would not outlive the picker anyway.
 */
class ReplayStore(context: Context) {

    private val appContext = context.applicationContext

    val directory: File = File(appContext.filesDir, "replays")

    data class Replay(val file: File) {
        val name: String get() = file.nameWithoutExtension
        val sizeBytes: Long get() = file.length()
        val modified: Long get() = file.lastModified()
    }

    fun list(): List<Replay> {
        val files = directory.listFiles { file ->
            file.isFile && file.extension.equals("rep", ignoreCase = true)
        } ?: return emptyList()
        return files.sortedByDescending { it.lastModified() }.map { Replay(it) }
    }

    /**
     * Copies [uri] into the library. Returns the stored replay, or null if the
     * source is not a .rep file.
     */
    @Throws(IOException::class)
    fun import(uri: Uri): Replay? {
        val sourceName = displayName(uri) ?: return null
        if (!sourceName.endsWith(".rep", ignoreCase = true)) return null

        if (!directory.isDirectory && !directory.mkdirs()) {
            throw IOException("could not create ${directory.absolutePath}")
        }

        val target = uniqueTarget(sourceName)
        val temp = File(directory, "${target.name}.part")
        try {
            appContext.contentResolver.openInputStream(uri)?.use { input ->
                temp.outputStream().use { output -> input.copyTo(output) }
            } ?: throw IOException("could not open $sourceName")

            if (!temp.renameTo(target)) throw IOException("could not store ${target.name}")
        } finally {
            temp.delete()
        }
        return Replay(target)
    }

    /**
     * Stores a replay that arrived as a byte stream rather than a document —
     * a download, typically. [sourceName] only suggests the filename; the
     * store still picks a non-clashing one.
     */
    @Throws(IOException::class)
    fun importStream(sourceName: String, input: java.io.InputStream): Replay {
        if (!directory.isDirectory && !directory.mkdirs()) {
            throw IOException("could not create ${directory.absolutePath}")
        }
        val target = uniqueTarget(sourceName)
        val temp = File(directory, "${target.name}.part")
        try {
            temp.outputStream().use { output -> input.copyTo(output) }
            if (!temp.renameTo(target)) throw IOException("could not store ${target.name}")
        } finally {
            temp.delete()
        }
        return Replay(target)
    }

    fun delete(replay: Replay): Boolean = replay.file.delete()

    /** Avoids clobbering an existing replay that happens to share a name. */
    private fun uniqueTarget(sourceName: String): File {
        val sanitized = sourceName.replace(Regex("[^A-Za-z0-9._ -]"), "_")
        // Case-insensitive: ".Rep" is as valid as ".rep", and stripping only
        // the exact-case forms would leave the old extension in the new name.
        val base = sanitized.dropLast(if (sanitized.endsWith(".rep", ignoreCase = true)) 4 else 0)
            .ifBlank { "replay" }
        var candidate = File(directory, "$base.rep")
        var counter = 2
        while (candidate.exists()) {
            candidate = File(directory, "$base ($counter).rep")
            counter++
        }
        return candidate
    }

    private fun displayName(uri: Uri): String? {
        appContext.contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0 && cursor.moveToFirst()) return cursor.getString(index)
        }
        return uri.lastPathSegment?.substringAfterLast('/')
    }
}
