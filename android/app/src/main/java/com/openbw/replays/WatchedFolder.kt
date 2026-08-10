package com.openbw.replays

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.content.edit
import java.io.IOException

/**
 * An optional folder the app pulls new replays out of on launch.
 *
 * This is how replays get from a desktop to the phone without the app needing
 * the network: whatever puts files in the folder — a cloud client's sync
 * folder, a file manager, a USB copy — is somebody else's problem, and the app
 * only ever reads through the Storage Access Framework. The user grants access
 * to exactly one folder, the grant is persisted across reboots, and no storage
 * permission is involved.
 *
 * Deliberately not a background service or an observer: syncing on launch is
 * enough for a replay viewer, and it costs no battery when the app is closed.
 */
class WatchedFolder(context: Context) {

    private val appContext = context.applicationContext
    private val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** The folder currently being watched, or null. */
    val treeUri: Uri?
        get() = prefs.getString(KEY_TREE, null)?.let(Uri::parse)

    val isConfigured: Boolean
        get() = treeUri != null

    /** Human-readable name for the watched folder, for display. */
    fun displayName(): String? {
        val uri = treeUri ?: return null
        return uri.lastPathSegment?.substringAfterLast(':')?.substringAfterLast('/')
    }

    /**
     * Records the folder the user picked and takes a persistable grant so the
     * app can still read it after a reboot.
     */
    fun remember(uri: Uri) {
        val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
        try {
            appContext.contentResolver.takePersistableUriPermission(uri, flags)
        } catch (e: SecurityException) {
            // Some providers hand out a one-shot grant. Syncing still works for
            // this session; it just has to be re-picked next launch.
        }
        prefs.edit { putString(KEY_TREE, uri.toString()) }
    }

    fun forget() {
        treeUri?.let { uri ->
            try {
                appContext.contentResolver.releasePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            } catch (e: SecurityException) {
                // Already gone; nothing to release.
            }
        }
        prefs.edit { remove(KEY_TREE).remove(KEY_SEEN) }
    }

    /**
     * Copies any replay in the folder the app has not already taken into the
     * library. Returns how many were imported.
     *
     * Files are tracked by name and size rather than by URI, because sync
     * clients routinely delete and recreate a file — which changes its document
     * id — without the contents changing. Doing it this way means a re-synced
     * replay is not imported twice.
     */
    fun sync(store: ReplayStore): Int {
        val tree = treeUri ?: return 0

        val childrenUri = try {
            DocumentsContract.buildChildDocumentsUriUsingTree(
                tree, DocumentsContract.getTreeDocumentId(tree)
            )
        } catch (e: IllegalArgumentException) {
            // The stored URI is not a tree URI any more; drop it rather than
            // failing every launch from here on.
            forget()
            return 0
        }

        val seen = prefs.getStringSet(KEY_SEEN, emptySet())!!.toMutableSet()
        var imported = 0

        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_SIZE,
        )

        try {
            appContext.contentResolver.query(childrenUri, projection, null, null, null)
        } catch (e: SecurityException) {
            // The persisted grant was revoked, e.g. the sync app was reinstalled.
            forget()
            return 0
        }?.use { cursor ->
            while (cursor.moveToNext()) {
                val documentId = cursor.getString(0) ?: continue
                val name = cursor.getString(1) ?: continue
                if (!name.endsWith(".rep", ignoreCase = true)) continue

                val size = if (cursor.isNull(2)) -1L else cursor.getLong(2)
                val token = "$name:$size"
                if (!seen.add(token)) continue

                val documentUri = DocumentsContract.buildDocumentUriUsingTree(tree, documentId)
                try {
                    if (store.import(documentUri) != null) imported++
                } catch (e: IOException) {
                    // Leave it unseen so the next launch retries; a sync client
                    // may simply not have finished writing the file yet.
                    seen.remove(token)
                }
            }
        }

        prefs.edit { putStringSet(KEY_SEEN, seen) }
        return imported
    }

    private companion object {
        const val PREFS = "watched_folder"
        const val KEY_TREE = "tree_uri"
        const val KEY_SEEN = "seen_replays"
    }
}
