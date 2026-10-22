package dev.cyclone.padtool

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View
import kotlin.math.roundToInt

class CurveView @JvmOverloads constructor(context: Context, var points: ByteArray = Remap.curve("linear")) : View(context) {
    var changed: ((ByteArray) -> Unit)? = null
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val margin = 24 * resources.displayMetrics.density
    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width - 2*margin; val h = height - 2*margin
        paint.color = Color.GRAY; paint.strokeWidth = 2f
        canvas.drawLine(margin, margin+h, margin+w, margin, paint)
        canvas.drawLine(margin, margin+h, margin+w, margin+h, paint)
        canvas.drawLine(margin, margin, margin, margin+h, paint)
        paint.color = Color.rgb(128, 203, 196); paint.strokeWidth = 4f
        var x = margin; var y = margin+h
        for (i in 0..points.size) {
            val nextX = margin + w*(i+1)/(points.size+1)
            val nextY = margin + h*(1 - (if (i == points.size) 255 else points.u8(i))/255f)
            canvas.drawLine(x, y, nextX, nextY, paint)
            if (i < points.size) canvas.drawCircle(nextX, nextY, 6*resources.displayMetrics.density, paint)
            x = nextX; y = nextY
        }
    }
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_DOWN || event.action == MotionEvent.ACTION_MOVE) {
            parent.requestDisallowInterceptTouchEvent(true)
            val i = (((event.x - margin)/(width - 2*margin)) * (points.size+1)).roundToInt().minus(1).coerceIn(0, points.size-1)
            val low = if (i == 0) 0 else points.u8(i-1); val high = if (i == points.lastIndex) 255 else points.u8(i+1)
            points[i] = ((1 - (event.y-margin)/(height-2*margin))*255).roundToInt().coerceIn(low, high).toByte()
            invalidate(); changed?.invoke(points.copyOf()); return true
        }
        if (event.action == MotionEvent.ACTION_UP) { performClick(); return true }
        return super.onTouchEvent(event)
    }
    override fun performClick(): Boolean { super.performClick(); return true }
}
