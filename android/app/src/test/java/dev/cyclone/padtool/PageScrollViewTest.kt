package dev.cyclone.padtool

import android.content.Context
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class PageScrollViewTest {
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
        scroll.scrollTo(0, 0)
        layout(400)
        layout(380)
        assertEquals(0, scroll.scrollY)
        assertSame(scroll, scroll.findFocus())
    }
}
