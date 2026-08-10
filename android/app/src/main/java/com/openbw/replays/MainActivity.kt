package com.openbw.replays

import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.CheckBox
import android.widget.EditText
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.openbw.replays.databinding.ActivityMainBinding
import java.io.IOException

/**
 * The library screen: one-time import of the StarCraft data files, then the
 * list of replays on the device, plus the two ways replays arrive on their own
 * — a watched folder, and a repository fetched over the network.
 *
 * Playback is always local and nothing is ever uploaded. The cloud feed is the
 * only outbound traffic the app makes, and it is off unless enabled.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var gameData: GameData
    private lateinit var replayStore: ReplayStore
    private lateinit var watchedFolder: WatchedFolder
    private lateinit var cloudFeed: CloudFeed
    private lateinit var adapter: ReplayAdapter

    private val pickGameData = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris -> importGameData(uris) }

    private val pickReplays = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris -> importReplays(uris) }

    private val pickWatchedFolder = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri != null) {
            watchedFolder.remember(uri)
            syncWatchedFolder()
            refresh()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        gameData = GameData(this)
        replayStore = ReplayStore(this)
        watchedFolder = WatchedFolder(this)
        cloudFeed = CloudFeed(this)

        adapter = ReplayAdapter(
            onClick = { replay -> openReplay(replay) },
            onLongClick = { replay -> confirmDelete(replay) },
        )
        binding.replayList.layoutManager = LinearLayoutManager(this)
        binding.replayList.adapter = adapter

        binding.importGameData.setOnClickListener {
            // Vendors disagree on a MIME type for .mpq, so filter by hand after
            // the pick rather than getting an empty picker.
            pickGameData.launch(arrayOf("*/*"))
        }
        binding.importReplays.setOnClickListener { pickReplays.launch(arrayOf("*/*")) }
        binding.cloudFeed.setOnClickListener { showCloudFeedDialog() }
        binding.fetchGameData.setOnClickListener { fetchGameDataFromRepo() }
        binding.watchFolder.setOnClickListener {
            if (watchedFolder.isConfigured) {
                confirmStopWatching()
            } else {
                pickWatchedFolder.launch(null)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        syncWatchedFolder()
        syncCloudFeed()
        refresh()
        maybeAutoPlayLatest()
    }

    /** Pulls in anything new that landed in the watched folder since last launch. */
    private fun syncWatchedFolder() {
        if (!watchedFolder.isConfigured) return
        val imported = watchedFolder.sync(replayStore)
        if (imported > 0) {
            toast(resources.getQuantityString(R.plurals.synced_replays, imported, imported))
        }
    }

    /**
     * Fetches replays committed by a cloud machine. Network work, so it runs off
     * the main thread and refreshes the list when it lands.
     */
    private fun syncCloudFeed() {
        if (!cloudFeed.isEnabled) return
        Thread {
            val result = cloudFeed.sync(replayStore)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                if (result.imported > 0) {
                    toast(
                        resources.getQuantityString(
                            R.plurals.synced_replays, result.imported, result.imported
                        )
                    )
                    refresh()
                }
                result.error?.let { toast(getString(R.string.cloud_failed, it)) }
            }
        }.start()
    }

    /**
     * Pulls the StarCraft archives from the configured repository, so a new
     * phone does not need ~90 MB moved onto it by hand. Explicit rather than
     * automatic, because of the size.
     */
    private fun fetchGameDataFromRepo() {
        toast(getString(R.string.setup_fetching))
        binding.fetchGameData.isEnabled = false
        Thread {
            val result = cloudFeed.fetchGameData(gameData)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                binding.fetchGameData.isEnabled = true
                when {
                    result.error != null -> toast(getString(R.string.cloud_failed, result.error))
                    gameData.isComplete -> toast(getString(R.string.setup_fetched))
                }
                refresh()
            }
        }.start()
    }

    /**
     * Opens the newest replay straight away, so launching the app puts you in a
     * game rather than in a file list.
     *
     * Only on a cold start: coming back from the viewer must land on the
     * library, otherwise backing out would immediately relaunch the same replay
     * and there would be no way to reach this screen.
     */
    private fun maybeAutoPlayLatest() {
        if (hasAutoPlayed) return
        hasAutoPlayed = true

        if (!gameData.isComplete) return
        val latest = replayStore.list().firstOrNull() ?: return
        startActivity(ViewerActivity.intent(this, latest.file))
    }

    private fun showCloudFeedDialog() {
        val view = layoutInflater.inflate(R.layout.dialog_cloud_feed, null)
        val enabled = view.findViewById<CheckBox>(R.id.cloud_enabled)
        val repo = view.findViewById<EditText>(R.id.cloud_repo)
        val branch = view.findViewById<EditText>(R.id.cloud_branch)
        val path = view.findViewById<EditText>(R.id.cloud_path)
        val token = view.findViewById<EditText>(R.id.cloud_token)

        enabled.isChecked = cloudFeed.isEnabled
        repo.setText(cloudFeed.repo)
        branch.setText(cloudFeed.branch)
        path.setText(cloudFeed.path)
        token.setText(cloudFeed.token)

        AlertDialog.Builder(this)
            .setTitle(R.string.cloud_title)
            .setView(view)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                cloudFeed.repo = repo.text.toString()
                cloudFeed.branch = branch.text.toString()
                cloudFeed.path = path.text.toString()
                cloudFeed.token = token.text.toString()
                cloudFeed.isEnabled = enabled.isChecked
                refresh()
                syncCloudFeed()
            }
            .show()
    }

    private fun confirmStopWatching() {
        AlertDialog.Builder(this)
            .setTitle(R.string.watch_folder_stop_title)
            .setMessage(getString(R.string.watch_folder_stop, watchedFolder.displayName().orEmpty()))
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.watch_folder_stop_confirm) { _, _ ->
                watchedFolder.forget()
                refresh()
            }
            .show()
    }

    private fun refresh() {
        val ready = gameData.isComplete
        binding.setupCard.visibility = if (ready) View.GONE else View.VISIBLE
        binding.importReplays.isEnabled = ready

        binding.cloudFeed.text =
            if (cloudFeed.isEnabled) getString(R.string.cloud_feed_active)
            else getString(R.string.cloud_feed)

        binding.watchFolder.text = watchedFolder.displayName()
            ?.let { getString(R.string.watch_folder_active, it) }
            ?: getString(R.string.watch_folder)

        if (!ready) {
            binding.setupStatus.text =
                getString(R.string.setup_missing, gameData.missing.joinToString(", "))
        }

        val replays = replayStore.list()
        adapter.submit(replays)
        binding.emptyMessage.visibility =
            if (replays.isEmpty() && ready) View.VISIBLE else View.GONE
    }

    private fun importGameData(uris: List<Uri>) {
        if (uris.isEmpty()) return
        var imported = 0
        val problems = mutableListOf<String>()
        for (uri in uris) {
            try {
                if (gameData.import(uri) != null) imported++
            } catch (e: IOException) {
                problems += (e.message ?: "import failed")
            }
        }
        if (problems.isNotEmpty()) {
            toast(getString(R.string.import_failed, problems.first()))
        } else if (imported == 0) {
            toast(getString(R.string.setup_wrong_files, GameData.CANONICAL_NAMES.joinToString(", ")))
        }
        refresh()
    }

    private fun importReplays(uris: List<Uri>) {
        if (uris.isEmpty()) return
        var imported = 0
        val problems = mutableListOf<String>()
        for (uri in uris) {
            try {
                if (replayStore.import(uri) != null) imported++
            } catch (e: IOException) {
                problems += (e.message ?: "import failed")
            }
        }
        when {
            problems.isNotEmpty() -> toast(getString(R.string.import_failed, problems.first()))
            imported == 0 -> toast(getString(R.string.import_not_replays))
        }
        refresh()
    }

    private fun openReplay(replay: ReplayStore.Replay) {
        if (!gameData.isComplete) {
            toast(getString(R.string.setup_missing, gameData.missing.joinToString(", ")))
            return
        }
        startActivity(ViewerActivity.intent(this, replay.file))
    }

    private fun confirmDelete(replay: ReplayStore.Replay) {
        AlertDialog.Builder(this)
            .setTitle(replay.name)
            .setMessage(R.string.delete_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.delete) { _, _ ->
                replayStore.delete(replay)
                refresh()
            }
            .show()
    }

    private fun toast(message: String) = Toast.makeText(this, message, Toast.LENGTH_LONG).show()

    private companion object {
        /**
         * Process-scoped rather than per-activity, so returning from the viewer
         * — which recreates this activity — does not autoplay again.
         */
        var hasAutoPlayed = false
    }
}
