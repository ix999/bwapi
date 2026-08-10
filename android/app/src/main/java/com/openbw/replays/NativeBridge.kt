package com.openbw.replays

/**
 * Control surface for the running replay engine.
 *
 * The engine runs on SDL's own thread. Everything here is safe to call from the
 * UI thread: commands are queued and applied by the engine on its next frame,
 * and [status] reads a snapshot the engine publishes rather than blocking on
 * one being rendered. When no replay is playing, commands are dropped and
 * [status] reports [Status.finished].
 *
 * The library is loaded by SDLActivity before SDL_main runs, so these must not
 * be called before [ViewerActivity] has started.
 */
object NativeBridge {

    data class Status(
        val currentFrame: Int,
        val endFrame: Int,
        val paused: Boolean,
        val speed: Float,
        val replayLoaded: Boolean,
        val finished: Boolean,
        val zoom: Float,
    ) {
        /** Playback position in 0..1, or 0 when the replay length is unknown. */
        val progress: Float
            get() = if (endFrame > 0) (currentFrame.toFloat() / endFrame).coerceIn(0f, 1f) else 0f
    }

    fun status(): Status {
        val v = nativeGetStatus() ?: return Status(0, 0, false, 1f, false, true, 1f)
        return Status(
            currentFrame = v[0],
            endFrame = v[1],
            paused = v[2] != 0,
            speed = v[3] / 1000f,
            replayLoaded = v[4] != 0,
            finished = v[5] != 0,
            zoom = v[6] / 1000f,
        )
    }

    fun isRunning(): Boolean = nativeIsRunning()

    fun setPaused(paused: Boolean) = nativeSetPaused(paused)

    fun togglePaused() = nativeTogglePaused()

    fun setSpeed(multiplier: Float) = nativeSetSpeed(multiplier)

    fun seekFraction(fraction: Float) = nativeSeekFraction(fraction)

    fun seekFrame(frame: Int) = nativeSeekFrame(frame)

    fun pan(dx: Int, dy: Int) = nativePan(dx, dy)

    fun setZoom(zoom: Float) = nativeSetZoom(zoom)

    fun quit() = nativeQuit()

    /** Last fatal engine error, or an empty string. Survives the engine exiting. */
    fun lastError(): String = nativeGetError() ?: ""

    fun mapName(): String = nativeGetMapName() ?: ""

    fun playerNames(): List<String> = nativeGetPlayerNames()?.toList() ?: emptyList()

    @JvmStatic private external fun nativeIsRunning(): Boolean
    @JvmStatic private external fun nativeGetStatus(): IntArray?
    @JvmStatic private external fun nativeSetPaused(paused: Boolean)
    @JvmStatic private external fun nativeTogglePaused()
    @JvmStatic private external fun nativeSetSpeed(multiplier: Float)
    @JvmStatic private external fun nativeSeekFraction(fraction: Float)
    @JvmStatic private external fun nativeSeekFrame(frame: Int)
    @JvmStatic private external fun nativePan(dx: Int, dy: Int)
    @JvmStatic private external fun nativeSetZoom(zoom: Float)
    @JvmStatic private external fun nativeQuit()
    @JvmStatic private external fun nativeGetError(): String?
    @JvmStatic private external fun nativeGetMapName(): String?
    @JvmStatic private external fun nativeGetPlayerNames(): Array<String>?
}
