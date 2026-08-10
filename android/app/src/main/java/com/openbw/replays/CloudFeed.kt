package com.openbw.replays

import android.content.Context
import androidx.core.content.edit
import org.json.JSONArray
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Pulls replays out of a folder in a GitHub repository.
 *
 * This is the path for replays produced somewhere the phone cannot see — a
 * cloud session running the bot, typically. That machine commits a `.rep` into
 * the repo; the phone picks it up on next launch. No desktop in the middle.
 *
 * GitHub's contents API already returns exactly the listing needed — name, size
 * and a direct download URL — so there is no manifest file to keep in sync. A
 * public repository needs no credentials at all, which is why the default
 * points at one; a token is only required for a private repo, and it is entered
 * by the user rather than shipped in the APK.
 *
 * Sync is opt-in and off until enabled: with it disabled the app makes no
 * network requests whatsoever, which keeps the offline guarantee intact for
 * anyone who does not want this.
 *
 * All methods block. Call from a background thread.
 */
class CloudFeed(context: Context) {

    private val appContext = context.applicationContext
    private val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    var isEnabled: Boolean
        get() = prefs.getBoolean(KEY_ENABLED, false)
        set(value) = prefs.edit { putBoolean(KEY_ENABLED, value) }

    /** "owner/repo" holding the replays. */
    var repo: String
        get() = prefs.getString(KEY_REPO, DEFAULT_REPO)!!
        set(value) = prefs.edit { putString(KEY_REPO, value.trim().ifBlank { DEFAULT_REPO }) }

    /** Folder within the repo. */
    var path: String
        get() = prefs.getString(KEY_PATH, DEFAULT_PATH)!!
        set(value) = prefs.edit { putString(KEY_PATH, value.trim().trim('/').ifBlank { DEFAULT_PATH }) }

    var branch: String
        get() = prefs.getString(KEY_BRANCH, DEFAULT_BRANCH)!!
        set(value) = prefs.edit { putString(KEY_BRANCH, value.trim().ifBlank { DEFAULT_BRANCH }) }

    /** Only needed for a private repository. Empty means unauthenticated. */
    var token: String
        get() = prefs.getString(KEY_TOKEN, "")!!
        set(value) = prefs.edit { putString(KEY_TOKEN, value.trim()) }

    class Result(val imported: Int, val error: String?)

    /**
     * Downloads any replay in the folder that is not already in the library.
     *
     * Files are matched on name and size, the same rule the folder sync uses,
     * so a replay that gets recommitted is not downloaded twice.
     */
    fun sync(store: ReplayStore): Result {
        if (!isEnabled) return Result(0, null)

        val seen = prefs.getStringSet(KEY_SEEN, emptySet())!!.toMutableSet()
        var imported = 0

        try {
            val listing = fetchListing()
            for (i in 0 until listing.length()) {
                val entry = listing.optJSONObject(i) ?: continue
                if (entry.optString("type") != "file") continue

                val name = entry.optString("name")
                if (!name.endsWith(".rep", ignoreCase = true)) continue

                val size = entry.optLong("size", -1L)
                val marker = "$name:$size"
                if (!seen.add(marker)) continue

                val downloadUrl = entry.optString("download_url")
                if (downloadUrl.isBlank() || downloadUrl == "null") {
                    seen.remove(marker)
                    continue
                }

                try {
                    download(downloadUrl) { input -> store.importStream(name, input) }
                    imported++
                } catch (e: IOException) {
                    // Leave unseen so the next launch retries.
                    seen.remove(marker)
                }
            }
        } catch (e: IOException) {
            prefs.edit { putStringSet(KEY_SEEN, seen) }
            return Result(imported, e.message ?: "could not reach GitHub")
        }

        prefs.edit { putStringSet(KEY_SEEN, seen) }
        return Result(imported, null)
    }

    private fun fetchListing(): JSONArray {
        val url = URL("https://api.github.com/repos/$repo/contents/$path?ref=$branch")
        val connection = open(url)
        try {
            val code = connection.responseCode
            if (code == HttpURLConnection.HTTP_NOT_FOUND) {
                // An empty folder is indistinguishable from a missing one here,
                // and neither is worth surfacing as an error every launch.
                return JSONArray()
            }
            if (code !in 200..299) {
                throw IOException("GitHub returned $code for $repo/$path")
            }
            val body = connection.inputStream.bufferedReader().use { it.readText() }
            return JSONArray(body)
        } catch (e: org.json.JSONException) {
            throw IOException("unexpected response from GitHub: ${e.message}")
        } finally {
            connection.disconnect()
        }
    }

    private fun <T> download(rawUrl: String, consume: (java.io.InputStream) -> T): T {
        val connection = open(URL(rawUrl))
        try {
            if (connection.responseCode !in 200..299) {
                throw IOException("download failed: ${connection.responseCode}")
            }
            return connection.inputStream.use(consume)
        } finally {
            connection.disconnect()
        }
    }

    private fun open(url: URL): HttpURLConnection {
        val connection = url.openConnection() as HttpURLConnection
        connection.connectTimeout = TIMEOUT_MS
        connection.readTimeout = TIMEOUT_MS
        connection.setRequestProperty("Accept", "application/vnd.github+json")
        connection.setRequestProperty("User-Agent", "openbw-replays")
        val auth = token
        if (auth.isNotEmpty()) connection.setRequestProperty("Authorization", "Bearer $auth")
        return connection
    }

    private companion object {
        const val PREFS = "cloud_feed"
        const val KEY_ENABLED = "enabled"
        const val KEY_REPO = "repo"
        const val KEY_PATH = "path"
        const val KEY_BRANCH = "branch"
        const val KEY_TOKEN = "token"
        const val KEY_SEEN = "seen_replays"

        const val DEFAULT_REPO = "ix999/bwapi"
        const val DEFAULT_PATH = "replays"
        // This fork's default branch, not "main".
        const val DEFAULT_BRANCH = "develop-openbw"

        const val TIMEOUT_MS = 20_000
    }
}
