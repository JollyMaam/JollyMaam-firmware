package dev.cyclone.padtool

import android.content.Context
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.os.Looper
import android.widget.Button
import android.widget.LinearLayout
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import java.time.Duration
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28, 35])
class PageScrollViewTest {
    private fun wheel(scroll: PageScrollView, amount: Float) {
        val properties = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE })
        val coordinates = arrayOf(MotionEvent.PointerCoords().apply { x = 100f; y = 100f; setAxisValue(MotionEvent.AXIS_VSCROLL, amount) })
        val time = android.os.SystemClock.uptimeMillis()
        val event = MotionEvent.obtain(time, time, MotionEvent.ACTION_SCROLL, 1, properties, coordinates, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0)
        try { assertTrue(scroll.dispatchGenericMotionEvent(event)) } finally { event.recycle() }
    }
    @Test fun wheelUpCancelsDownwardFlingAndStaysPutBetweenEvents() {
        val context: Context = RuntimeEnvironment.getApplication()
        val scroll = PageScrollView(context)
        scroll.addView(View(context).apply { minimumHeight = 10000 }, android.widget.FrameLayout.LayoutParams(400, 10000))
        scroll.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY))
        scroll.layout(0, 0, 400, 400)
        scroll.scrollTo(0, 2000)
        scroll.fling(8000)
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(32))
        scroll.computeScroll()
        assertTrue("A real fling must be moving before wheel input", scroll.scrollY > 2000)
        val before = scroll.scrollY
        wheel(scroll, 3f)
        val after = scroll.scrollY
        assertTrue("Wheel up must move up", after < before)
        repeat(30) {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(16))
            scroll.computeScroll()
            assertEquals("Old fling must not overwrite wheel position", after, scroll.scrollY)
        }
    }
    @Test fun repeatedWheelDirectionChangesAndLimitsDoNotDriftOrReachChildren() {
        val context: Context = RuntimeEnvironment.getApplication()
        var childWheelEvents = 0
        val scroll = PageScrollView(context)
        scroll.addView(object : View(context) {
            override fun onGenericMotionEvent(event: MotionEvent): Boolean { childWheelEvents++; return true }
        }.apply { minimumHeight = 10000 })
        scroll.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY))
        scroll.layout(0, 0, 400, 400)
        repeat(10) {
            val before = scroll.scrollY
            wheel(scroll, -1f)
            assertTrue(scroll.scrollY > before)
        }
        repeat(10) {
            val before = scroll.scrollY
            wheel(scroll, 1f)
            assertTrue(scroll.scrollY < before)
        }
        assertEquals(0, scroll.scrollY)
        wheel(scroll, 1000f)
        assertEquals(0, scroll.scrollY)
        wheel(scroll, -1000f)
        assertEquals(9600, scroll.scrollY)
        wheel(scroll, -1000f)
        assertEquals(9600, scroll.scrollY)
        wheel(scroll, 1000f)
        repeat(30) {
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(16))
            scroll.computeScroll()
            assertEquals(0, scroll.scrollY)
        }
        assertEquals(0, childWheelEvents)
        // A subsequent touch fling still animates normally.
        val down = MotionEvent.obtain(0, 0, MotionEvent.ACTION_DOWN, 100f, 100f, 0)
        scroll.dispatchTouchEvent(down); down.recycle()
        val cancel = MotionEvent.obtain(0, 0, MotionEvent.ACTION_CANCEL, 100f, 100f, 0)
        scroll.dispatchTouchEvent(cancel); cancel.recycle()
        scroll.fling(4000)
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(32))
        scroll.computeScroll()
        assertTrue(scroll.scrollY > 0)
    }
    @Test fun wheelPositionWinsOverDeferredChildFocusOnLayoutAndResize() {
        val context: Context = RuntimeEnvironment.getApplication()
        val scroll = PageScrollView(context)
        val content = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        repeat(50) { content.addView(Button(context).apply { text = "Row $it" }, LinearLayout.LayoutParams(400, 60)) }
        scroll.addView(content)
        fun layout(height: Int) {
            scroll.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY))
            scroll.layout(0, 0, 400, height)
        }
        layout(400)
        // Leave a focus-reveal request pending while a wheel action moves away.
        content.requestLayout()
        content.getChildAt(49).requestFocus()
        val properties = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE })
        val coordinates = arrayOf(MotionEvent.PointerCoords().apply { setAxisValue(MotionEvent.AXIS_VSCROLL, 100f) })
        val wheel = MotionEvent.obtain(0, 1, MotionEvent.ACTION_SCROLL, 1, properties, coordinates, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0)
        scroll.onGenericMotionEvent(wheel); wheel.recycle()
        assertEquals(0, scroll.scrollY)
        layout(400)
        layout(380)
        assertEquals(0, scroll.scrollY)
        assertSame(scroll, scroll.findFocus())
    }
}
