package dev.cyclone.padtool

import android.content.Context
import android.graphics.Rect
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.ViewConfiguration
import android.widget.ScrollView
import kotlin.math.roundToInt

/** Mouse scrolling must not later snap back to a button left focused below it. */
class PageScrollView(context: Context) : ScrollView(context) {
    private var wheelScrolling = false
    private val wheelFactor = ViewConfiguration.get(context).scaledVerticalScrollFactor
    init {
        isFillViewport = true
        isFocusableInTouchMode = true
        descendantFocusability = ViewGroup.FOCUS_BEFORE_DESCENDANTS
    }
    private fun isVerticalWheel(event: MotionEvent) = event.action == MotionEvent.ACTION_SCROLL &&
        event.isFromSource(InputDevice.SOURCE_CLASS_POINTER) && event.getAxisValue(MotionEvent.AXIS_VSCROLL) != 0f
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        // A row's horizontal scroller or focused button must not consume vertical wheel input.
        return if (isVerticalWheel(event)) onGenericMotionEvent(event) else super.dispatchGenericMotionEvent(event)
    }
    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (isVerticalWheel(event)) {
            wheelScrolling = true
            requestFocus()
            scrollBy(0, -(event.getAxisValue(MotionEvent.AXIS_VSCROLL) * wheelFactor).roundToInt())
            // scrollTo/scrollBy do not cancel ScrollView's OverScroller. Reset it at
            // the new position so an old fling cannot overwrite the wheel movement.
            // Bypass native wheel handling, which can start another differential fling.
            super.fling(0)
            return true
        }
        return super.onGenericMotionEvent(event)
    }
    // ScrollView can defer a child's focus reveal until its next layout/resize.
    // A later wheel action supersedes that request, including the deferred case.
    override fun computeScrollDeltaToGetChildRectOnScreen(rect: Rect): Int =
        if (wheelScrolling) 0 else super.computeScrollDeltaToGetChildRectOnScreen(rect)
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) wheelScrolling = false
        return super.dispatchKeyEvent(event)
    }
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_DOWN) wheelScrolling = false
        return super.dispatchTouchEvent(event)
    }
}
