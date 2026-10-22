package dev.cyclone.padtool

import android.content.Context
import android.graphics.Rect
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.ScrollView

/** Mouse scrolling must not later snap back to a button left focused below it. */
class PageScrollView(context: Context) : ScrollView(context) {
    private var wheelScrolling = false
    init {
        isFillViewport = true
        isFocusableInTouchMode = true
        descendantFocusability = ViewGroup.FOCUS_BEFORE_DESCENDANTS
    }
    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_SCROLL && event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)) {
            wheelScrolling = true
            requestFocus()
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
