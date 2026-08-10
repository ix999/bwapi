package com.openbw.replays

import android.app.AlertDialog
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.openbw.replays.databinding.ActivityMainBinding
import java.io.IOException

/**
 * The library screen: one-time import of the StarCraft data files, then the
 * list of replays on the device.
 *
 * Everything is local. The app declares no network permission, so imported
 * files never leave the device.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var gameData: GameData
    private lateinit var replayStore: ReplayStore
    private lateinit var adapter: ReplayAdapter

    private val pickGameData = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris -> importGameData(uris) }

    private val pickReplays = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()
    ) { uris -> importReplays(uris) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        gameData = GameData(this)
        replayStore = ReplayStore(this)

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
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        val ready = gameData.isComplete
        binding.setupCard.visibility = if (ready) View.GONE else View.VISIBLE
        binding.importReplays.isEnabled = ready

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
}
