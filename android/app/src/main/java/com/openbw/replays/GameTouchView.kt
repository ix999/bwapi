package com.openbw.replays

import android.content.Context
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View

/**
 * Takes every touch over the game surface and turns it into an explicit engine
 * command.
 *
 * The app owns all interaction: this view sits on top of SDL's surface and
 * consumes the events, so openbw never sees a mouse and its built-in
 * drag-select, minimap clicking and replay slider are unreachable. Only the
 * app's own controls, which sit above this view, take touches of their own.
 *
 * - drag: pans, and the map tracks the finger rather than moving away from it
 * - pinch: zooms about the centre of the screen
 * - tap: selects the unit underneath
 * - long press: follows that unit; again on the same unit releases it
 */
class GameTouchView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    /** Current magnification, mirrored here so pinch can scale relative to it. */
    var zoom: Float = DEFAULT_ZOOM
        set(value) {
            field = value.coerceIn(MIN_ZOOM, MAX_ZOOM)
        }

    /** Notified when a gesture changes the zoom, so the host can show it. */
    var onZoomChanged: ((Float) -> Unit)? = null

    /** Notified after a long press, so the host can report follow state. */
    var onFollowToggled: (() -> Unit)? = null

    private val scaleDetector = ScaleGestureDetector(
        context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(detector: ScaleGestureDetector): Boolean {
                zoom *= detector.scaleFactor
                NativeBridge.setZoom(zoom)
                onZoomChanged?.invoke(zoom)
                return true
            }
        },
    )

    private val gestureDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent): Boolean = true

            override fun onSingleTapUp(e: MotionEvent): Boolean {
                NativeBridge.selectAt(e.x.toInt(), e.y.toInt())
                return true
            }

            override fun onLongPress(e: MotionEvent) {
                NativeBridge.toggleFollowAt(e.x.toInt(), e.y.toInt())
                onFollowToggled?.invoke()
            }

            override fun onScroll(
                e1: MotionEvent?,
                e2: MotionEvent,
                distanceX: Float,
                distanceY: Float,
            ): Boolean {
                // Ignore one-finger panning mid-pinch: the detector reports its
                // own focal movement and the two would fight.
                if (scaleDetector.isInProgress) return true
                // distance is the movement of the finger negated already, which
                // is the direction the camera must move for the map to follow
                // the finger.
                NativeBridge.pan(distanceX.toInt(), distanceY.toInt())
                return true
            }
        },
    )

    init {
        isClickable = true
        isFocusable = false
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        // Both detectors see every event: pinch needs the secondary pointers,
        // and the gesture detector needs a continuous stream to time long press.
        scaleDetector.onTouchEvent(event)
        gestureDetector.onTouchEvent(event)
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    companion object {
        // Below 1 would mean an SDL surface larger than the display: more
        // pixels for the software renderer and no visible benefit.
        const val MIN_ZOOM = 1f
        const val MAX_ZOOM = 8f
        const val DEFAULT_ZOOM = 2f
    }
}
