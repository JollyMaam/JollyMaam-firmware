package dev.cyclone.padtool

import android.content.Intent
import android.hardware.usb.UsbConfiguration
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28])
class FirmwareUpdaterActivityTest {
    private fun views(view: View): List<View> = listOf(view) + if (view is ViewGroup)
        (0 until view.childCount).flatMap { views(view.getChildAt(it)) } else emptyList()
    private fun device(vendor: Int, product: Int): UsbDevice = UsbDevice::class.java.getDeclaredConstructor(String::class.java,
        Int::class.javaPrimitiveType, Int::class.javaPrimitiveType, Int::class.javaPrimitiveType,
        Int::class.javaPrimitiveType, Int::class.javaPrimitiveType, String::class.java,
        String::class.java, String::class.java, String::class.java)
        .newInstance("/dev/bus/usb/001/002", vendor, product, 0, 0, 0, "Test", "Pad", "1.0", "123").apply {
            UsbDevice::class.java.getDeclaredField("mConfigurations").apply { isAccessible = true }.set(this, emptyArray<UsbConfiguration>())
        }
    @Test fun selectsDocumentWithoutOpeningUsb() {
        val controller = Robolectric.buildActivity(FirmwareUpdaterActivity::class.java).setup()
        val activity = controller.get()
        val buttons = views(activity.window.decorView).filterIsInstance<Button>()
        assertTrue(views(activity.window.decorView).filterIsInstance<TextView>().any { it.text.contains("No supported USB") })
        buttons.first { it.text == "Choose firmware ZIP" }.performClick()
        val intent = shadowOf(activity).nextStartedActivityForResult
        assertEquals(Intent.ACTION_OPEN_DOCUMENT, intent.intent.action)
        assertTrue(intent.intent.hasCategory(Intent.CATEGORY_OPENABLE))
        controller.pause().stop().destroy()
    }
    @Test fun requiresPackageBeforeDeviceCanBeSelected() {
        val controller = Robolectric.buildActivity(FirmwareUpdaterActivity::class.java).setup()
        val activity = controller.get()
        shadowOf(activity.getSystemService(UsbManager::class.java)).addOrUpdateUsbDevice(device(0x2dc8, 0x3208), false)
        views(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Rescan USB" }.performClick()
        val button = views(activity.window.decorView).filterIsInstance<Button>().first { it.text.contains("2dc8:3208") }
        assertFalse(button.isEnabled)
        assertEquals(1, AndroidNativeUsb.kind(device(0x2dc8, 0x3208)))
        assertEquals(0, AndroidNativeUsb.kind(device(0x16c0, 0x05df)))
        assertNull(AndroidNativeUsb.kind(device(0x2dc8, 0x310a)))
        controller.pause().stop().destroy()
    }
    @Test fun updaterIsReachableWithoutAConnectedPad() {
        val controller = Robolectric.buildActivity(MainActivity::class.java).setup()
        val activity = controller.get()
        views(activity.window.decorView).filterIsInstance<Button>().first { it.text == "Firmware updater" }.performClick()
        assertEquals(FirmwareUpdaterActivity::class.java.name, shadowOf(activity).nextStartedActivity.component!!.className)
        controller.pause().stop().destroy()
    }
}
