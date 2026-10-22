package dev.cyclone.padtool

import android.widget.TextView
import android.view.View
import android.view.ViewGroup
import android.hardware.usb.UsbDevice
import android.widget.Button
import android.widget.ScrollView
import android.widget.LinearLayout
import android.widget.CheckBox
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
        assertTrue(descendants(activity.window.decorView).filterIsInstance<Button>().any { it.text == "Refresh" })
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
        activity.window.decorView.findViewWithTag<Button>("field-type").performClick()
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
        val toolbar = activity.window.decorView.findViewWithTag<LinearLayout>("toolbar")
        assertEquals(toolbar.width, buttons.first { it.text == "Back" }.right)
        assertEquals(listOf("Refresh", "Apply", "Discard", "Back"), buttons.take(4).map { it.text.toString() })
        assertTrue(buttons.any { it.text == "Slot A" })
        assertTrue(buttons.any { it.text == "Remap Base" })
        assertFalse(buttons.any { it.text in listOf("Overview", "Devices", "Rescan", "?") })
        val status = activity.window.decorView.findViewWithTag<TextView>("status")
        assertEquals(1, status.maxLines)
        val height = status.height
        m.progress("A very long operation status that used to wrap across several lines", 50, 100)
        shadowOf(Looper.getMainLooper()).idle(); layout(activity)
        assertEquals(height, status.height)
        buttons.first { it.text == "Slot A" }.performClick()
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Back" }.performClick()
        assertEquals(-1, m.page)
        descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Refresh" }.performClick()
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
        val editor = activity.window.decorView.findViewWithTag<Button>("field-name")
        val count = m.session!!.drafts.edits.size
        assertTrue(editor.performLongClick())
        val dialog = ShadowAlertDialog.getLatestAlertDialog()
        assertEquals(name.help + "\n\nDefault: " + name.show(m.session!!.defaults.getValue(11).getValue(name.id).bytes), dialog.findViewById<TextView>(android.R.id.message).text.toString())
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
    @Test @Config(sdk = [28, 35]) fun developerToggleExpandsInPlaceAndLabelsArePlainWithDefaultsOnlyOnLongPress() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get(); val m = model(activity)
        chooseSimulation(activity)
        m.page = 0; m.changed!!.invoke(); layout(activity)
        val page = activity.window.decorView.findViewWithTag<PageScrollView>("page")
        val advanced = activity.window.decorView.findViewWithTag<LinearLayout>("developer-fields")
        val toggle = activity.window.decorView.findViewWithTag<CheckBox>("developer-toggle")
        assertEquals(View.GONE, advanced.visibility)
        assertTrue(advanced.childCount > 0)
        toggle.performClick(); layout(activity)
        assertTrue(m.developer); assertTrue(toggle.isChecked)
        assertSame(page, activity.window.decorView.findViewWithTag<PageScrollView>("page"))
        assertEquals(View.VISIBLE, advanced.visibility)
        assertTrue(activity.window.decorView.findViewWithTag<Button>("field-report_hz").isShown)
        val name = activity.window.decorView.findViewWithTag<TextView>("label-type")
        assertEquals("type:", name.text.toString()); assertFalse(name.isClickable)
        val value = activity.window.decorView.findViewWithTag<Button>("field-type")
        val f = m.session!!.fields.first { it.kind == 0 && it.name == "type" }
        assertEquals(f.show(m.session!!.values.getValue(0).getValue(f.id).bytes), value.text.toString())
        assertFalse(texts(activity.window.decorView).any { it.startsWith("Default:") })
        name.performLongClick()
        val help = ShadowAlertDialog.getLatestAlertDialog()
        assertTrue(help.findViewById<TextView>(android.R.id.message).text.contains("Default: " + f.show(m.session!!.defaults.getValue(0).getValue(f.id).bytes)))
        help.dismiss()
        controller.recreate(); layout(controller.get())
        assertTrue(controller.get().window.decorView.findViewWithTag<CheckBox>("developer-toggle").isChecked)
        controller.get().window.decorView.findViewWithTag<CheckBox>("developer-toggle").performClick()
        assertEquals(View.GONE, controller.get().window.decorView.findViewWithTag<LinearLayout>("developer-fields").visibility)
        controller.pause().stop().destroy()
    }
    private fun pick(label: String) {
        val dialog = ShadowAlertDialog.getLatestAlertDialog()
        val adapter = dialog.listView.adapter
        val index = (0 until adapter.count).first { adapter.getItem(it).toString() == label }
        dialog.listView.performItemClick(null, index, index.toLong())
        shadowOf(Looper.getMainLooper()).idle()
    }
    @Test @Config(sdk = [28, 35]) fun toolbarScopesDeviceAndGlobalChangesAndWarnsBeforeGlobalActions() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get(); val m = model(activity)
        chooseSimulation(activity)
        val s = m.session!!
        val f = s.fields.first { it.kind == 0 && it.name == "primary_colour" }
        m.stage(s, listOf(Edit(0, f.id, f.parse("9")), Edit(1, f.id, f.parse("9"))))
        val other = PadSession(Identity(1, 0, "test", "other-device"), s.fields).apply { values = s.values; defaults = s.defaults }
        m.sessions[other.identity.flashId] = other
        m.stage(other, listOf(Edit(0, f.id, f.parse("9"))))
        fun click(text: String) = descendants(activity.window.decorView).filterIsInstance<Button>().first { it.text == text }.performClick()
        val page = activity.window.decorView.findViewWithTag<PageScrollView>("page")
        val content = page.getChildAt(0) as LinearLayout
        val divider = content.findViewWithTag<View>("actions-divider")
        assertTrue(content.indexOfChild(divider) > 0)
        assertTrue(content.indexOfChild(divider) < content.indexOfChild(descendants(content).filterIsInstance<Button>().first { it.text == "Back up saved settings…" }))
        // Per-pad Discard/Apply affect both slots and leave another pad alone.
        click("Discard")
        assertTrue(s.drafts.edits.isEmpty()); assertEquals(1, other.drafts.edits.size)
        m.stage(s, listOf(Edit(0, f.id, f.parse("9")), Edit(1, f.id, f.parse("9"))))
        click("Apply"); waitForWorker(m)
        assertTrue(s.drafts.edits.isEmpty()); assertEquals(1, other.drafts.edits.size)
        assertEquals(9, s.values.getValue(0).getValue(f.id).bytes.u8(0))
        assertEquals(9, s.values.getValue(1).getValue(f.id).bytes.u8(0))
        m.stage(s, listOf(Edit(0, f.id, f.parse("8"))))
        click("Back"); layout(activity)
        assertEquals(-2, m.page)
        assertFalse(texts(activity.window.decorView).any { it.contains("USB tool mode is required") })
        assertTrue(activity.window.decorView.findViewWithTag<TextView>("status").text.contains("2 pending · 2 devices"))
        click("Apply")
        var dialog = ShadowAlertDialog.getLatestAlertDialog()
        assertTrue(dialog.findViewById<TextView>(android.R.id.message).text.contains("2 devices"))
        dialog.getButton(android.app.AlertDialog.BUTTON_NEGATIVE).performClick(); shadowOf(Looper.getMainLooper()).idle()
        assertEquals(1, s.drafts.edits.size); assertEquals(1, other.drafts.edits.size)
        click("Discard")
        dialog = ShadowAlertDialog.getLatestAlertDialog()
        assertTrue(dialog.findViewById<TextView>(android.R.id.message).text.contains("2 devices"))
        dialog.getButton(android.app.AlertDialog.BUTTON_POSITIVE).performClick(); shadowOf(Looper.getMainLooper()).idle()
        assertTrue(s.drafts.edits.isEmpty()); assertTrue(other.drafts.edits.isEmpty())
        m.stage(s, listOf(Edit(0, f.id, f.parse("8")), Edit(1, f.id, f.parse("8"))))
        click("Apply")
        ShadowAlertDialog.getLatestAlertDialog().getButton(android.app.AlertDialog.BUTTON_POSITIVE).performClick()
        shadowOf(Looper.getMainLooper()).idle(); waitForWorker(m)
        assertEquals(-2, m.page); assertTrue(s.drafts.edits.isEmpty())
        assertEquals(8, s.values.getValue(0).getValue(f.id).bytes.u8(0))
        assertEquals(8, s.values.getValue(1).getValue(f.id).bytes.u8(0))
        controller.pause().stop().destroy()
    }
    @Test @Config(sdk = [28, 35]) fun combinedRemapEditsMappingsAndStructuredRulesAndDiscardsPerKey() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get(); val m = model(activity)
        chooseSimulation(activity)
        m.page = 18; m.changed!!.invoke(); layout(activity)
        val s = m.session!!
        val map = s.fields.first { it.kind == 3 && it.name == "map" }
        val rules = s.fields.first { it.kind == 3 && it.name == "rules" }
        fun button(tag: String) = activity.window.decorView.findViewWithTag<Button>(tag)
        assertNull(activity.window.decorView.findViewWithTag<Button>("field-map"))
        assertNull(activity.window.decorView.findViewWithTag<Button>("field-rules"))
        button("remap-map-a").performClick(); pick("B")
        assertEquals(2, s.drafts.edits.getValue(18 to map.id).u8(0))
        assertFalse(s.drafts.edits.containsKey(18 to rules.id))
        button("remap-status-a").performClick()
        assertTrue(s.drafts.edits.isEmpty())
        button("remap-map-a").performClick(); pick("B")
        button("remap-add-r4").performClick()
        val rule = ShadowAlertDialog.getLatestAlertDialog()
        rule.window!!.decorView.findViewWithTag<Button>("rule-trigger").performClick(); pick("is tapped under")
        assertEquals("tap", rule.window!!.decorView.findViewWithTag<Button>("rule-effect").text.toString())
        rule.window!!.decorView.findViewWithTag<Button>("rule-trigger").performClick(); pick("is held")
        rule.window!!.decorView.findViewWithTag<Button>("rule-effect").performClick(); pick("turbo modifier")
        rule.window!!.decorView.findViewWithTag<Button>("rule-outputs").performClick()
        val outputs = ShadowAlertDialog.getLatestAlertDialog()
        outputs.listView.performItemClick(null, 0, 0); outputs.listView.performItemClick(null, 1, 1)
        outputs.getButton(android.app.AlertDialog.BUTTON_POSITIVE).performClick()
        rule.window!!.decorView.findViewWithTag<Button>("rule-rate").performClick(); pick("10 Hz")
        rule.getButton(android.app.AlertDialog.BUTTON_POSITIVE).performClick()
        assertEquals("r4 held turbo_modifier a+b@10", Remap.showRules(s.drafts.edits.getValue(18 to rules.id)))
        assertEquals(2, s.drafts.edits.getValue(18 to map.id).u8(0))
        button("remap-status-r4").performClick()
        assertFalse(s.drafts.edits.containsKey(18 to rules.id))
        assertTrue(s.drafts.edits.containsKey(18 to map.id))
        controller.recreate(); layout(controller.get())
        assertEquals("B", controller.get().window.decorView.findViewWithTag<Button>("remap-map-a").text.toString())
        controller.get().window.decorView.findViewWithTag<Button>("remap-status-a").performClick()
        assertTrue(s.drafts.edits.isEmpty())
        controller.pause().stop().destroy()
    }
    @Test @Config(sdk = [35]) fun remapPresetKeepsOtherMappingsAndRuleLimitAndDeletionWork() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get(); val m = model(activity)
        chooseSimulation(activity)
        m.page = 18; m.changed!!.invoke(); layout(activity)
        val s = m.session!!
        val map = s.fields.first { it.kind == 3 && it.name == "map" }
        val rules = s.fields.first { it.kind == 3 && it.name == "rules" }
        m.stage(s, listOf(Edit(18, map.id, Remap.parseMap("l4=l1"))))
        activity.window.decorView.findViewWithTag<Button>("remap-preset").performClick(); pick("Swap A/B")
        assertEquals("a=b b=a l4=l1", Remap.showMap(s.drafts.edits.getValue(18 to map.id)))
        val before = s.drafts.edits.mapValues { it.value.hex() }
        activity.window.decorView.findViewWithTag<Button>("remap-add-r4").performClick()
        ShadowAlertDialog.getLatestAlertDialog().getButton(android.app.AlertDialog.BUTTON_NEGATIVE).performClick()
        shadowOf(Looper.getMainLooper()).idle()
        assertEquals(before, s.drafts.edits.mapValues { it.value.hex() })
        m.stage(s, listOf(Edit(18, rules.id, Remap.encodeRules(List(16) { Remap.Rule("r4", outputs = listOf("a")) }))))
        assertEquals("Rule entries: 16 of 16 used", activity.window.decorView.findViewWithTag<TextView>("rule-count").text.toString())
        assertFalse(activity.window.decorView.findViewWithTag<Button>("remap-add-a").isEnabled)
        activity.window.decorView.findViewWithTag<Button>("remap-delete-0").performClick()
        assertEquals(15, Remap.decodeRules(s.drafts.edits.getValue(18 to rules.id)).size)
        assertTrue(activity.window.decorView.findViewWithTag<Button>("remap-add-a").isEnabled)
        // Restoring one key must respect the limit when other keys use all entries.
        s.values = s.values + (18 to (s.values.getValue(18) + (rules.id to Value(Remap.encodeRules(listOf(Remap.Rule("a", outputs = listOf("b")))), true))))
        m.stage(s, listOf(Edit(18, rules.id, Remap.encodeRules(List(16) { Remap.Rule("r4", outputs = listOf("a")) }))))
        activity.window.decorView.findViewWithTag<Button>("remap-status-a").performClick()
        assertTrue(ShadowAlertDialog.getLatestAlertDialog().findViewById<TextView>(android.R.id.message).text.contains("exceed 16 rules"))
        assertEquals(16, Remap.decodeRules(s.drafts.edits.getValue(18 to rules.id)).size)
        ShadowAlertDialog.getLatestAlertDialog().dismiss()
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
        val edit = activity.window.decorView.findViewWithTag<Button>("field-map")
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
