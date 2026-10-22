package dev.cyclone.padtool

import android.widget.TextView
import android.view.View
import android.view.ViewGroup
import android.hardware.usb.UsbDevice
import android.widget.Button
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
        assertTrue(texts(activity.window.decorView).any { it == "Cyclone padtool" })
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
    @Test @Config(sdk = [35]) fun simulatedLaunchRendersEveryPageAndAppliesWithoutUsb() {
        val controller = Robolectric.buildActivity(MainActivity::class.java, Intent().putExtra("simulate", true)).setup()
        val activity = controller.get(); val m = model(activity)
        waitForWorker(m)
        assertNotNull("Simulation failed: ${m.message}", m.simulation)
        assertNull(m.selected)
        assertTrue(texts(activity.window.decorView).contains("SIMULATION · No USB device is accessed"))
        val s = m.session!!
        for (t in Protocol.targets(s.fields)) {
            m.page = t; m.changed!!.invoke()
            assertTrue(texts(activity.window.decorView).contains(Protocol.targetNames[t].replace('-', ' ')))
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
