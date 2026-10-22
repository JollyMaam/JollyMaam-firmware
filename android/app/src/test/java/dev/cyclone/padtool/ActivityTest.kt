package dev.cyclone.padtool

import android.widget.TextView
import android.view.View
import android.view.ViewGroup
import android.hardware.usb.UsbDevice
import android.widget.Button
import android.widget.ScrollView
import android.widget.LinearLayout
import org.robolectric.shadows.ShadowAlertDialog
import org.robolectric.Shadows.shadowOf
import android.os.Looper
import android.content.Intent
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28])
class ActivityTest {
    private fun texts(view: View): List<String> = (if (view is TextView) listOf(view.text.toString()) else emptyList()) +
        if (view is ViewGroup) (0 until view.childCount).flatMap { texts(view.getChildAt(it)) } else emptyList()
    @Test fun launchesWithoutUsbAndSurvivesRecreation() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get()
        assertEquals(-2, model(activity).page)
        assertTrue(descendants(activity.window.decorView).filterIsInstance<Button>().any { it.text == "Rescan" })
        assertTrue(texts(activity.window.decorView).any { it.contains("No tool devices") })
        controller.recreate()
        assertTrue(texts(controller.get().window.decorView).any { it.contains("No tool devices") })
        controller.pause().stop().destroy()
    }
    @Test @Config(sdk = [35]) fun launchesOnAndroid15() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        assertTrue(texts(controller.get().window.decorView).any { it.contains("No tool devices") })
        controller.pause().stop().destroy()
    }
    private fun descendants(view: View): List<View> = listOf(view) + if (view is ViewGroup)
        (0 until view.childCount).flatMap { descendants(view.getChildAt(it)) } else emptyList()
    private fun model(activity: MainActivity) = MainActivity::class.java.getDeclaredField("model").apply { isAccessible = true }.get(activity) as AppModel
    private fun waitForWorker(model: AppModel) {
        val deadline = System.nanoTime() + 5_000_000_000L
        while (model.busy && System.nanoTime() < deadline) { Thread.sleep(10); shadowOf(Looper.getMainLooper()).idle() }
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse("Worker timed out: ${model.message}", model.busy)
    }
    private fun layout(activity: MainActivity) {
        activity.window.decorView.apply {
            measure(View.MeasureSpec.makeMeasureSpec(480, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY))
            layout(0, 0, 480, 800)
        }
        shadowOf(Looper.getMainLooper()).idle()
    }
    private fun chooseSimulation(activity: MainActivity) {
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Simulated pad (no USB)" }.performClick()
        waitForWorker(model(activity))
        layout(activity)
    }
    @Test @Config(sdk = [35]) fun firstStartShowsDevicesThenSimulationRendersEveryPageAndAppliesWithoutUsb() {
        val controller = Robolectric.buildActivity(MainActivity::class.java, Intent().putExtra("simulate", true)).setup()
        val activity = controller.get(); val m = model(activity)
        assertEquals(-2, m.page)
        assertNull(m.simulation)
        chooseSimulation(activity)
        assertNotNull("Simulation failed: ${m.message}", m.simulation)
        assertNull(m.selected)
        assertTrue(activity.window.decorView.findViewWithTag<TextView>("status").text.contains("Simulated pad"))
        val s = m.session!!
        for (t in Protocol.targets(s.fields)) {
            m.page = t; m.changed!!.invoke()
            val title = Protocol.targetNames[t].split('-').joinToString(" ") { it.replaceFirstChar(Char::uppercaseChar) }
            assertTrue(activity.window.decorView.findViewWithTag<TextView>("status").text.contains(title))
        }
        m.page = 0; m.changed!!.invoke()
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text.toString().startsWith("type\n") }.performClick()
        val dialog = ShadowAlertDialog.getLatestAlertDialog()
        dialog.listView.performItemClick(null, 0, 0)
        shadowOf(Looper.getMainLooper()).idle()
        assertTrue(s.drafts.edits.isNotEmpty())
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Apply" }.performClick()
        waitForWorker(m)
        assertFalse("Apply failed: ${m.message}", s.needsRefresh)
        assertTrue(s.drafts.edits.isEmpty())
        val type = s.fields.first { it.kind == 0 && it.name == "type" }
        assertEquals(0, s.values.getValue(0).getValue(type.id).bytes.u8(0))
        assertThrows(IllegalArgumentException::class.java) { m.transport { error("USB must never open") } }
        controller.recreate()
        assertSame(m.simulation, model(controller.get()).simulation)
        controller.pause().stop().destroy()
    }
    @Test fun compactToolbarRescanBackAndCapitalizedTargets() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get(); val m = model(activity)
        chooseSimulation(activity)
        val buttons = descendants(activity.window.decorView).filterIsInstance<Button>()
        assertEquals(listOf("Apply", "Discard", "Defaults", "Rescan", "Back"), buttons.take(5).map { it.text.toString() })
        assertTrue(buttons.any { it.text == "Slot A" })
        assertTrue(buttons.any { it.text == "Remap Base" })
        assertFalse(buttons.any { it.text in listOf("Overview", "Devices", "Refresh", "?") })
        val status = activity.window.decorView.findViewWithTag<TextView>("status")
        assertEquals(1, status.maxLines)
        val height = status.height
        m.progress("A very long operation status that used to wrap across several lines", 50, 100)
        shadowOf(Looper.getMainLooper()).idle(); layout(activity)
        assertEquals(height, status.height)
        buttons.first { it.text == "Slot A" }.performClick()
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Back" }.performClick()
        assertEquals(-1, m.page)
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Rescan" }.performClick()
        assertEquals(-2, m.page)
        assertNull(m.live)
        assertTrue(descendants(activity.window.decorView).filterIsInstance<Button>().any { it.text == "Simulated pad (no USB)" })
        controller.pause().stop().destroy()
    }
    @Test fun fieldLongPressShowsHelpWithoutStagingAndRendersKeepScrollPosition() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get(); val m = model(activity)
        chooseSimulation(activity)
        m.page = 11; m.changed!!.invoke(); layout(activity)
        val name = m.session!!.fields.first { it.kind == 3 && it.name == "name" }
        val editor = descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text.toString().startsWith("name\n") }
        val count = m.session!!.drafts.edits.size
        assertTrue(editor.performLongClick())
        val dialog = ShadowAlertDialog.getLatestAlertDialog()
        assertEquals(name.help, dialog.findViewById<TextView>(android.R.id.message).text.toString())
        assertEquals(count, m.session!!.drafts.edits.size)
        dialog.dismiss()
        val scroll = activity.window.decorView.findViewWithTag<PageScrollView>("page")
        scroll.scrollTo(0, 600)
        assertEquals(600, scroll.scrollY)
        m.stage(m.session!!, listOf(Edit(11, name.id, name.parse("Edited game"))))
        layout(activity)
        assertEquals(600, activity.window.decorView.findViewWithTag<PageScrollView>("page").scrollY)
        controller.recreate(); layout(controller.get())
        assertEquals(600, controller.get().window.decorView.findViewWithTag<PageScrollView>("page").scrollY)
        controller.pause().stop().destroy()
    }
    @Test fun stagesMappingAndRetainsDraftThroughRecreation() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get()
        val modelField = MainActivity::class.java.getDeclaredField("model").apply { isAccessible = true }
        val model = modelField.get(activity) as AppModel
        // A display-only USB identity; all USB communication remains mocked out.
        model.selected = UsbDevice::class.java.getDeclaredConstructor(String::class.java,
            Int::class.javaPrimitiveType, Int::class.javaPrimitiveType, Int::class.javaPrimitiveType,
            Int::class.javaPrimitiveType, Int::class.javaPrimitiveType, String::class.java,
            String::class.java, String::class.java, String::class.java)
            .newInstance("/dev/bus/usb/001/002",19020,16725,0,0,0,"Jieli","Cyclone","1.0","123")
        val map = Field(3,2,3,20,0,false,"map","Key mappings",emptyList())
        val s = PadSession(Identity(1,0,"test","ui-fixture"),listOf(map))
        s.values = mapOf(10 to mapOf(2 to Value(ByteArray(20),false)))
        s.defaults = s.values
        model.session = s; model.sessions[s.identity.flashId] = s; model.page = 10
        model.changed!!.invoke()
        val edit = descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text.toString().startsWith("map\n") }
        edit.performClick()
        val dialog = ShadowAlertDialog.getLatestAlertDialog()
        assertTrue(texts(dialog.window!!.decorView).contains("A"))
        dialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE).performClick()
        shadowOf(Looper.getMainLooper()).idle()
        // Default identity mapping deliberately staged explicitly by the editor.
        assertEquals(1,s.drafts.edits.size)
        controller.recreate()
        val retained = modelField.get(controller.get()) as AppModel
        assertSame(model,retained); assertEquals(1,retained.sessions.getValue("ui-fixture").drafts.edits.size)
        controller.pause().stop().destroy()
    }
}
