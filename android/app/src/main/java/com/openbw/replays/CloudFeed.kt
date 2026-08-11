package com.openbw.replays

import android.content.Context
import androidx.core.content.edit
import org.json.JSONArray
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
 * By default it searches **every branch**, because a session's push protection
 * normally confines it to its own working branch. Watching a single branch
 * would mean retyping a branch name here every time a session reported one,
 * which is exactly the friction this avoids.
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

    /** Only consulted when [searchAllBranches] is off. */
    var branch: String
        get() = prefs.getString(KEY_BRANCH, DEFAULT_BRANCH)!!
        set(value) = prefs.edit { putString(KEY_BRANCH, value.trim().ifBlank { DEFAULT_BRANCH }) }

    /** Search every branch rather than just [branch]. */
    var searchAllBranches: Boolean
        get() = prefs.getBoolean(KEY_ALL_BRANCHES, true)
        set(value) = prefs.edit { putBoolean(KEY_ALL_BRANCHES, value) }

    /** Required for a private repository. */
    var token: String
        get() = prefs.getString(KEY_TOKEN, "")!!
        set(value) = prefs.edit { putString(KEY_TOKEN, value.trim()) }

    val isConfigured: Boolean
        get() = isEnabled && repo.isNotBlank()

    class Result(val imported: Int, val error: String?)

    private class Entry(val path: String, val size: Long, val sha: String, val ref: String) {
        val name: String get() = path.substringAfterLast('/')
    }

    /**
     * Downloads any replay not already taken, across the branches being
     * searched.
     */
    fun sync(store: ReplayStore): Result {
        if (!isConfigured) return Result(0, null)

        val seen = prefs.getStringSet(KEY_SEEN, emptySet())!!.toMutableSet()
        var imported = 0

        val entries = try {
            collect { entry ->
                entry.path.startsWith(pathPrefix()) && entry.name.endsWith(".rep", ignoreCase = true)
            }
        } catch (e: IOException) {
            return Result(0, e.message ?: "could not reach GitHub")
        }

        var lastError: String? = null
        for (entry in entries) {
            // Legacy marker: earlier versions keyed on path and size. Honour it
            // so upgrading does not re-download the whole library as duplicates.
            val legacy = "${entry.path}:${entry.size}"
            if (entry.sha in seen || legacy in seen) continue
            try {
                download(entry.ref, entry.path) { input -> store.importStream(entry.name, input) }
                seen += entry.sha
                imported++
            } catch (e: IOException) {
                lastError = e.message
            }
        }

        prefs.edit { putStringSet(KEY_SEEN, seen) }
        return Result(imported, lastError)
    }

    /**
     * Downloads whichever StarCraft archives are still missing. Saves moving
     * ~90 MB onto the phone by hand, and the archives stay in the private repo
     * rather than being redistributed.
     *
     * Explicitly invoked rather than run on launch: this is a large download,
     * potentially over mobile data.
     */
    fun fetchGameData(gameData: GameData): Result {
        if (repo.isBlank()) return Result(0, "no repository configured")

        val wanted = gameData.missing
        if (wanted.isEmpty()) return Result(0, null)

        val entries = try {
            collect { entry -> wanted.any { it.equals(entry.name, ignoreCase = true) } }
        } catch (e: IOException) {
            return Result(0, e.message ?: "could not reach GitHub")
        }
        if (entries.isEmpty()) {
            return Result(0, "not found in $repo: ${wanted.joinToString(", ")}")
        }

        var imported = 0
        var lastError: String? = null
        // One copy of each archive is enough, wherever it was found.
        for (entry in entries.distinctBy { it.name.lowercase() }) {
            try {
                download(entry.ref, entry.path) { input -> gameData.importStream(entry.name, input) }
                imported++
            } catch (e: IOException) {
                lastError = e.message
            }
        }
        return Result(imported, lastError)
    }

    private fun pathPrefix(): String = if (path.isEmpty()) "" else "$path/"

    /**
     * Walks the branches being searched and returns matching blobs, one per
     * distinct content hash. Branches sharing a commit are walked once, and a
     * replay present on several branches is only downloaded from the first.
     */
    private fun collect(keep: (Entry) -> Boolean): List<Entry> {
        val refs = if (searchAllBranches) listBranches() else listOf(branch to branch)

        val seenCommits = mutableSetOf<String>()
        val byContent = LinkedHashMap<String, Entry>()
        var firstError: IOException? = null

        for ((name, commit) in refs) {
            if (commit.isNotEmpty() && !seenCommits.add(commit)) continue
            try {
                for (entry in listTree(name, keep)) {
                    // Not putIfAbsent: that is API 24+, and minSdk here is 21.
                    if (!byContent.containsKey(entry.sha)) byContent[entry.sha] = entry
                }
            } catch (e: IOException) {
                // One unreadable branch should not sink the whole sync.
                if (firstError == null) firstError = e
            }
        }

        if (byContent.isEmpty() && firstError != null) throw firstError
        return byContent.values.toList()
    }

    /** Branch names paired with their head commit sha. */
    private fun listBranches(): List<Pair<String, String>> {
        val url = URL("https://api.github.com/repos/$repo/branches?per_page=$MAX_BRANCHES")
        val body = request(url) { it.bufferedReader().readText() }
        return try {
            val array = JSONArray(body)
            val result = mutableListOf<Pair<String, String>>()
            for (i in 0 until array.length()) {
                val node = array.optJSONObject(i) ?: continue
                val name = node.optString("name")
                if (name.isEmpty()) continue
                result += name to (node.optJSONObject("commit")?.optString("sha") ?: "")
            }
            result
        } catch (e: JSONException) {
            throw IOException("unexpected branch list from GitHub: ${e.message}")
        }
    }

    /** One recursive tree request per ref, filtered locally. */
    private fun listTree(ref: String, keep: (Entry) -> Boolean): List<Entry> {
        val url = URL("https://api.github.com/repos/$repo/git/trees/${encode(ref)}?recursive=1")
        val body = request(url) { it.bufferedReader().readText() }

        return try {
            val root = JSONObject(body)
            val tree = root.optJSONArray("tree") ?: return emptyList()
            val result = mutableListOf<Entry>()
            for (i in 0 until tree.length()) {
                val node = tree.optJSONObject(i) ?: continue
                if (node.optString("type") != "blob") continue
                val entry = Entry(
                    path = node.optString("path"),
                    size = node.optLong("size", -1L),
                    sha = node.optString("sha"),
                    ref = ref,
                )
                if (entry.sha.isNotEmpty() && keep(entry)) result += entry
            }
            result
        } catch (e: JSONException) {
            throw IOException("unexpected response from GitHub: ${e.message}")
        }
    }

    private fun <T> download(ref: String, repoPath: String, consume: (java.io.InputStream) -> T): T {
        val encoded = repoPath.split('/').joinToString("/") { encode(it) }
        val url = URL("https://api.github.com/repos/$repo/contents/$encoded?ref=${encode(ref)}")
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
        const val KEY_ALL_BRANCHES = "all_branches"
        const val KEY_TOKEN = "token"
        const val KEY_SEEN = "seen_replays"

        // The bot repo is private, which is the point: replays reveal build
        // orders, so they do not belong in a public repository.
        const val DEFAULT_REPO = "ix999/starcraft-broodwar-bot"
        const val DEFAULT_PATH = "replays"
        // Only used when searching a single branch; carries gitignored assets.
        const val DEFAULT_BRANCH = "with-assets"

        // One tree request per branch, so cap it. Well above this repo's count.
        const val MAX_BRANCHES = 50

        const val TIMEOUT_MS = 20_000
    }
}
