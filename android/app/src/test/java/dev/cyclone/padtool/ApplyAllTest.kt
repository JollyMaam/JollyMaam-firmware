package dev.cyclone.padtool

import android.os.Looper
import java.io.IOException
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class ApplyAllTest {
    private class Device(private val id: Int) : Scsi {
        private val sim = SimulatedPad(javaClass.getResourceAsStream("/simulated-pad.json")!!.bufferedReader().use { it.readText() })
        var offline = false
        var loseCommitReply = false
        var writes = 0
        var commits = 0
        val protocol = Protocol(this)
        override fun transfer(cdb: ByteArray, length: Int, out: ByteArray?, timeout: Int, rawLength: Boolean): ByteArray {
            if (offline) throw IOException("Device $id is disconnected")
            if (cdb.u8(1) == 0x22) writes++
            if (cdb.u8(1) == 0x23) commits++
            val result = sim.transfer(cdb, length, out, timeout, rawLength)
            if (cdb.u8(1) == 0) result[24] = id.toByte()
            if (cdb.u8(1) == 0x23 && loseCommitReply) throw IOException("Commit reply lost")
            return result
        }
    }
    private fun await(model: AppModel) {
        val deadline = System.nanoTime() + 5_000_000_000L
        while (model.busy && System.nanoTime() < deadline) { Thread.sleep(10); shadowOf(Looper.getMainLooper()).idle() }
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse("Worker timed out", model.busy)
    }
    private fun session(model: AppModel, device: Device): PadSession {
        val p = device.protocol
        return PadSession(p.identity(), p.fields()).also { s ->
            model.read(p, s); model.sessions[s.identity.flashId] = s
            val field = s.fields.first { it.kind == 0 && it.name == "primary_colour" }
            model.stage(s, listOf(Edit(0, field.id, field.parse("9"))))
        }
    }
    private fun batch(model: AppModel, sessions: List<PadSession>, devices: List<Device>) {
        model.applySessions(sessions) { s, action -> action(devices[sessions.indexOf(s)].protocol) }
        await(model)
    }
    @Test fun appliesEachDeviceIndependentlyAndClearsOnlyConfirmedDrafts() {
        val m = AppModel(RuntimeEnvironment.getApplication())
        try {
            val devices = listOf(Device(1), Device(2)); val sessions = devices.map { session(m, it) }
            batch(m, sessions, devices)
            assertTrue(sessions.all { it.drafts.edits.isEmpty() && !it.needsRefresh })
            assertTrue(devices.all { it.commits == 1 })
            for (s in sessions) {
                val f = s.fields.first { it.kind == 0 && it.name == "primary_colour" }
                assertEquals(9, s.values.getValue(0).getValue(f.id).bytes.u8(0))
            }
        } finally { m.close() }
    }
    @Test fun disconnectedDeviceStopsTheBatchBeforeAnyWrites() {
        val m = AppModel(RuntimeEnvironment.getApplication())
        try {
            val devices = listOf(Device(1), Device(2)); val sessions = devices.map { session(m, it) }
            devices[1].offline = true
            batch(m, sessions, devices)
            assertTrue(devices.all { it.writes == 0 && it.commits == 0 })
            assertTrue(sessions.all { it.drafts.edits.size == 1 && !it.needsRefresh })
            assertTrue(m.message.contains("disconnected"))
        } finally { m.close() }
    }
    @Test fun lostReplyKeepsFailedDevicesDraftsAndReportsPartialCompletion() {
        val m = AppModel(RuntimeEnvironment.getApplication())
        try {
            val devices = listOf(Device(1), Device(2)); val sessions = devices.map { session(m, it) }
            devices[1].loseCommitReply = true
            batch(m, sessions, devices)
            assertTrue(sessions[0].drafts.edits.isEmpty()); assertFalse(sessions[0].needsRefresh)
            assertEquals(1, sessions[1].drafts.edits.size); assertTrue(sessions[1].needsRefresh)
            assertTrue(m.message.contains("1 of 2 devices committed"))
        } finally { m.close() }
    }
    @Test fun oversizedDeviceStopsTheBatchBeforeAnyWrites() {
        val m = AppModel(RuntimeEnvironment.getApplication())
        try {
            val devices = listOf(Device(1), Device(2)); val sessions = devices.map { session(m, it) }
            val rules = sessions[1].fields.first { it.kind == 3 && it.name == "rules" }
            m.stage(sessions[1], (10..18).map { Edit(it, rules.id, Remap.parseRules("r4 held hold south")) })
            batch(m, sessions, devices)
            assertTrue(devices.all { it.writes == 0 && it.commits == 0 })
            assertTrue(sessions.all { it.drafts.edits.isNotEmpty() })
            assertTrue(m.message.contains("apply one page at a time"))
        } finally { m.close() }
    }
}
