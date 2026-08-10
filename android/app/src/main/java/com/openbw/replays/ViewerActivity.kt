package com.openbw.replays

import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.Button
import android.widget.ImageButton
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import org.libsdl.app.SDLActivity
import java.io.File
import java.util.Locale

/**
 * Plays one replay.
 *
 * SDLActivity owns the surface and runs the engine on its own thread; this
 * subclass supplies the native libraries and the arguments SDL_main receives,
 * then lays a transport bar over the top of SDL's view. The bar is the only
 * part of the overlay that takes touches, so taps and drags anywhere else fall
 * through to the engine, which drives openbw's own minimap and unit selection.
 */
class ViewerActivity : SDLActivity() {

    private val handler = Handler(Looper.getMainLooper())

    private lateinit var playPauseButton: ImageButton
    private lateinit var seekBar: SeekBar
    private lateinit var timeLabel: TextView
    private lateinit var speedButton: Button
    private lateinit var titleLabel: TextView

    private lateinit var replayStore: ReplayStore

    private var userIsSeeking = false
    private var engineHasStarted = false
    private var speedIndex = DEFAULT_SPEED_INDEX
    private var zoom = 2f

    /** Name of the replay currently loaded, for the overlay title. */
    private var currentReplayName: String = ""

    /**
     * Order matters: SDLActivity treats the last entry as the library holding
     * SDL_main.
     */
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "bwreplay")

    /**
     * Becomes argv[1..] for SDL_main: the directory holding the mpqs, then the
     * replay to play.
     */
    override fun getArguments(): Array<String> = arrayOf(
        GameData(this).directory.absolutePath,
        intent.getStringExtra(EXTRA_REPLAY_PATH).orEmpty(),
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        replayStore = ReplayStore(this)
        currentReplayName = intent.getStringExtra(EXTRA_REPLAY_PATH)
            ?.let { File(it).nameWithoutExtension }.orEmpty()

        // Attach to the window's content view rather than SDLActivity's own
        // mLayout field: that field is SDL implementation detail, and the
        // content view is where SDL has already installed its surface, so the
        // overlay lands on top of it either way.
        val content = findViewById<ViewGroup>(android.R.id.content)
        val overlay = LayoutInflater.from(this).inflate(R.layout.viewer_overlay, content, false)
        content.addView(overlay)

        playPauseButton = overlay.findViewById(R.id.play_pause)
        seekBar = overlay.findViewById(R.id.seek_bar)
        timeLabel = overlay.findViewById(R.id.time_label)
        speedButton = overlay.findViewById(R.id.speed)

        playPauseButton.setOnClickListener { NativeBridge.togglePaused() }

        speedButton.setOnClickListener {
            speedIndex = (speedIndex + 1) % SPEEDS.size
            NativeBridge.setSpeed(SPEEDS[speedIndex])
            speedButton.text = formatSpeed(SPEEDS[speedIndex])
        }

        overlay.findViewById<Button>(R.id.zoom_in).setOnClickListener { applyZoom(zoom * 1.5f) }
        overlay.findViewById<Button>(R.id.zoom_out).setOnClickListener { applyZoom(zoom / 1.5f) }

        titleLabel = overlay.findViewById(R.id.replay_title)
        overlay.findViewById<Button>(R.id.pick_replay).setOnClickListener { showReplayPicker() }

        seekBar.max = SEEK_RESOLUTION
        seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                if (fromUser) NativeBridge.seekFraction(progress.toFloat() / SEEK_RESOLUTION)
            }

            override fun onStartTrackingTouch(bar: SeekBar) {
                userIsSeeking = true
            }

            override fun onStopTrackingTouch(bar: SeekBar) {
                userIsSeeking = false
            }
        })

        speedButton.text = formatSpeed(SPEEDS[speedIndex])
        titleLabel.text = currentReplayName
    }

    /**
     * Lists the library so another replay can be started without leaving the
     * viewer. The engine swaps replays in place, so the mpqs and image data
     * stay loaded and the switch is quick.
     */
    private fun showReplayPicker() {
        val replays = replayStore.list()
        if (replays.isEmpty()) {
            Toast.makeText(this, R.string.replays_empty, Toast.LENGTH_SHORT).show()
            return
        }

        val wasPaused = NativeBridge.status().paused
        val labels = replays.map { it.name }.toTypedArray()
        val current = replays.indexOfFirst { it.name == currentReplayName }

        AlertDialog.Builder(this)
            .setTitle(R.string.viewer_pick_replay)
            .setSingleChoiceItems(labels, current) { dialog, which ->
                dialog.dismiss()
                val chosen = replays[which]
                currentReplayName = chosen.name
                titleLabel.text = currentReplayName
                NativeBridge.loadReplay(chosen.file.absolutePath)
                // Loading resets to playing; honour the state the user was in.
                if (wasPaused) NativeBridge.setPaused(true)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /**
     * The viewer is `singleTask` — SDL does not support two live instances — so
     * launching it again while it is already running reuses this instance and
     * delivers the new intent here rather than restarting SDL_main. Without
     * this, picking a replay from the library after leaving the viewer with
     * Home would silently keep playing the previous one.
     */
    override fun onNewIntent(newIntent: Intent) {
        super.onNewIntent(newIntent)
        setIntent(newIntent)

        val path = newIntent.getStringExtra(EXTRA_REPLAY_PATH).orEmpty()
        if (path.isEmpty()) return

        currentReplayName = File(path).nameWithoutExtension
        titleLabel.text = currentReplayName
        NativeBridge.loadReplay(path)
    }

    override fun onStart() {
        super.onStart()
        handler.post(pollStatus)
    }

    override fun onStop() {
        super.onStop()
        handler.removeCallbacks(pollStatus)
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        // Stop the engine loop first so its thread unwinds before the activity
        // goes away.
        NativeBridge.quit()
        @Suppress("DEPRECATION")
        super.onBackPressed()
    }

    private fun applyZoom(requested: Float) {
        zoom = requested.coerceIn(MIN_ZOOM, MAX_ZOOM)
        NativeBridge.setZoom(zoom)
    }

    private val pollStatus = object : Runnable {
        override fun run() {
            val running = NativeBridge.isRunning()
            if (running) engineHasStarted = true

            if (engineHasStarted && !running) {
                // The engine loop exited: either the user backed out or it hit
                // a fatal error while loading.
                finishWithEngineError()
                return
            }

            if (running) {
                val status = NativeBridge.status()
                playPauseButton.setImageResource(
                    if (status.paused) android.R.drawable.ic_media_play
                    else android.R.drawable.ic_media_pause
                )
                if (!userIsSeeking) {
                    seekBar.progress = (status.progress * SEEK_RESOLUTION).toInt()
                }
                timeLabel.text = getString(
                    R.string.viewer_time,
                    formatFrames(status.currentFrame),
                    formatFrames(status.endFrame),
                )
            } else {
                // Still starting up: loading the mpqs takes a moment.
                val error = NativeBridge.lastError()
                if (error.isNotEmpty()) {
                    finishWithEngineError()
                    return
                }
            }

            handler.postDelayed(this, POLL_INTERVAL_MS)
        }
    }

    private fun finishWithEngineError() {
        handler.removeCallbacks(pollStatus)
        val error = NativeBridge.lastError()
        if (error.isNotEmpty()) {
            Toast.makeText(this, getString(R.string.viewer_error, error), Toast.LENGTH_LONG).show()
        }
        finish()
    }

    private fun formatSpeed(multiplier: Float): String =
        if (multiplier < 1f) String.format(Locale.US, "%.2fx", multiplier)
        else String.format(Locale.US, "%.0fx", multiplier)

    /** BW simulates one frame every 42ms at normal speed. */
    private fun formatFrames(frames: Int): String {
        val totalSeconds = frames.toLong() * FRAME_MILLIS / 1000
        return String.format(Locale.US, "%d:%02d", totalSeconds / 60, totalSeconds % 60)
    }

    companion object {
        private const val EXTRA_REPLAY_PATH = "com.openbw.replays.REPLAY_PATH"

        private const val POLL_INTERVAL_MS = 250L
        private const val SEEK_RESOLUTION = 1000
        private const val FRAME_MILLIS = 42L

        private const val MIN_ZOOM = 0.5f
        private const val MAX_ZOOM = 8f

        private val SPEEDS = floatArrayOf(0.25f, 0.5f, 1f, 2f, 4f, 8f, 16f)
        private const val DEFAULT_SPEED_INDEX = 2

        fun intent(context: Context, replay: File): Intent =
            Intent(context, ViewerActivity::class.java)
                .putExtra(EXTRA_REPLAY_PATH, replay.absolutePath)
    }
}
