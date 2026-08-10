package com.openbw.replays

import android.content.Context
import androidx.core.content.edit
import org.json.JSONException
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Pulls replays out of a GitHub repository.
 *
 * This is the path for replays produced somewhere the phone cannot see — a
 * cloud session running the bot, typically. That machine commits a `.rep`; the
 * phone picks it up on next launch, with no desktop in between.
 *
 * Two GitHub APIs, chosen for a private repository:
 *
 *  - **Listing** uses the git trees API recursively, so it does not care how the
 *    replays are arranged. That matters because the existing corpus is sharded
 *    by content hash (`replays/library/03/<hash>.rep`) rather than flat, and a
 *    plain directory listing would miss all of it.
 *  - **Downloading** uses the contents API with a raw `Accept` header rather
 *    than `raw.githubusercontent.com`, which does not reliably honour a personal
 *    access token.
 *
 * The token is entered by the user and stored in app preferences; nothing is
 * baked into the APK. Sync is opt-in and off until enabled — with it disabled
 * the app makes no network requests at all, and nothing is ever uploaded.
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

    /** Path prefix within the repo. Everything with a `.rep` suffix below it counts. */
    var path: String
        get() = prefs.getString(KEY_PATH, DEFAULT_PATH)!!
        set(value) = prefs.edit {
            putString(KEY_PATH, value.trim().trim('/').ifBlank { DEFAULT_PATH })
        }

    var branch: String
        get() = prefs.getString(KEY_BRANCH, DEFAULT_BRANCH)!!
        set(value) = prefs.edit { putString(KEY_BRANCH, value.trim().ifBlank { DEFAULT_BRANCH }) }

    /** Required for a private repository. */
    var token: String
        get() = prefs.getString(KEY_TOKEN, "")!!
        set(value) = prefs.edit { putString(KEY_TOKEN, value.trim()) }

    val isConfigured: Boolean
        get() = isEnabled && repo.isNotBlank()

    class Result(val imported: Int, val error: String?)

    private class Entry(val path: String, val size: Long) {
        val name: String get() = path.substringAfterLast('/')
    }

    /**
     * Downloads any replay not already taken. Matching is on path and size, so
     * a replay that gets recommitted unchanged is not fetched twice.
     */
    fun sync(store: ReplayStore): Result {
        if (!isConfigured) return Result(0, null)

        val seen = prefs.getStringSet(KEY_SEEN, emptySet())!!.toMutableSet()
        var imported = 0

        val entries = try {
            listReplays()
        } catch (e: IOException) {
            return Result(0, e.message ?: "could not reach GitHub")
        }

        var lastError: String? = null
        for (entry in entries) {
            val marker = "${entry.path}:${entry.size}"
            if (!seen.add(marker)) continue
            try {
                download(entry.path) { input -> store.importStream(entry.name, input) }
                imported++
            } catch (e: IOException) {
                // Leave it unseen so the next launch retries.
                seen.remove(marker)
                lastError = e.message
            }
        }

        prefs.edit { putStringSet(KEY_SEEN, seen) }
        return Result(imported, lastError)
    }

    /**
     * Downloads whichever StarCraft archives are still missing, from the same
     * repository. Saves moving ~90 MB onto the phone by hand, and the archives
     * stay in the private repo rather than being redistributed.
     *
     * Explicitly invoked rather than run on launch: this is a large download,
     * potentially over mobile data.
     */
    fun fetchGameData(gameData: GameData): Result {
        if (repo.isBlank()) return Result(0, "no repository configured")

        val wanted = gameData.missing
        if (wanted.isEmpty()) return Result(0, null)

        val entries = try {
            listBlobs { name -> wanted.any { it.equals(name, ignoreCase = true) } }
        } catch (e: IOException) {
            return Result(0, e.message ?: "could not reach GitHub")
        }
        if (entries.isEmpty()) {
            return Result(0, "not found in $repo: ${wanted.joinToString(", ")}")
        }

        var imported = 0
        var lastError: String? = null
        for (entry in entries) {
            try {
                download(entry.path) { input -> gameData.importStream(entry.name, input) }
                imported++
            } catch (e: IOException) {
                lastError = e.message
            }
        }
        return Result(imported, lastError)
    }

    /** Every `.rep` under [path], at any depth. */
    private fun listReplays(): List<Entry> {
        val prefix = if (path.isEmpty()) "" else "$path/"
        return listTree { entry ->
            entry.path.startsWith(prefix) && entry.name.endsWith(".rep", ignoreCase = true)
        }
    }

    /** Blobs anywhere in the tree whose filename satisfies [matches]. */
    private fun listBlobs(matches: (String) -> Boolean): List<Entry> =
        listTree { entry -> matches(entry.name) }

    /**
     * One recursive tree request, filtered locally. Recursive rather than a
     * directory listing because the replay corpus is sharded by content hash,
     * and the archives live somewhere else entirely.
     */
    private fun listTree(keep: (Entry) -> Boolean): List<Entry> {
        val url = URL("https://api.github.com/repos/$repo/git/trees/${encode(branch)}?recursive=1")
        val body = request(url) { it.bufferedReader().readText() }

        return try {
            val root = JSONObject(body)
            val tree = root.optJSONArray("tree") ?: return emptyList()
            val result = mutableListOf<Entry>()
            for (i in 0 until tree.length()) {
                val node = tree.optJSONObject(i) ?: continue
                if (node.optString("type") != "blob") continue
                val entry = Entry(node.optString("path"), node.optLong("size", -1L))
                if (keep(entry)) result += entry
            }
            if (root.optBoolean("truncated") && result.isEmpty()) {
                throw IOException("repository tree too large to list")
            }
            result
        } catch (e: JSONException) {
            throw IOException("unexpected response from GitHub: ${e.message}")
        }
    }

    private fun <T> download(repoPath: String, consume: (java.io.InputStream) -> T): T {
        val encoded = repoPath.split('/').joinToString("/") { encode(it) }
        val url = URL("https://api.github.com/repos/$repo/contents/$encoded?ref=${encode(branch)}")
        return request(url, raw = true, consume = consume)
    }

    private fun <T> request(
        url: URL,
        raw: Boolean = false,
        consume: (java.io.InputStream) -> T,
    ): T {
        val connection = url.openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty(
                "Accept",
                if (raw) "application/vnd.github.raw" else "application/vnd.github+json"
            )
            connection.setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            connection.setRequestProperty("User-Agent", "openbw-replays")
            val auth = token
            if (auth.isNotEmpty()) connection.setRequestProperty("Authorization", "Bearer $auth")

            when (val code = connection.responseCode) {
                in 200..299 -> return connection.inputStream.use(consume)
                HttpURLConnection.HTTP_UNAUTHORIZED, HttpURLConnection.HTTP_FORBIDDEN ->
                    throw IOException("access denied ($code) — check the token and its repo access")
                HttpURLConnection.HTTP_NOT_FOUND ->
                    throw IOException("not found — check the repo, branch and folder")
                else -> throw IOException("GitHub returned $code")
            }
        } finally {
            connection.disconnect()
        }
    }

    private fun encode(value: String): String =
        URLEncoder.encode(value, "UTF-8").replace("+", "%20")

    private companion object {
        const val PREFS = "cloud_feed"
        const val KEY_ENABLED = "enabled"
        const val KEY_REPO = "repo"
        const val KEY_PATH = "path"
        const val KEY_BRANCH = "branch"
        const val KEY_TOKEN = "token"
        const val KEY_SEEN = "seen_replays"

        // The bot repo is private, which is the point: replays reveal build
        // orders, so they do not belong in a public repository.
        const val DEFAULT_REPO = "ix999/starcraft-broodwar-bot"
        const val DEFAULT_PATH = "replays"
        // The branch that carries gitignored assets; .rep is excluded elsewhere.
        const val DEFAULT_BRANCH = "with-assets"

        const val TIMEOUT_MS = 20_000
    }
}
