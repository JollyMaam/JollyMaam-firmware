package dev.cyclone.padtool

import org.junit.Assert.*
import org.junit.Test

class SimulatedPadTest {
    private var time = 1000L
    private fun sim(saved: String? = null, persist: (String) -> Unit = {}) = SimulatedPad(
        javaClass.getResourceAsStream("/simulated-pad.json")!!.bufferedReader().use { it.readText() }, saved, persist, { time })
    private fun field(s: SimulatedPad, target: Int, name: String) = s.fields.first { it.kind == Protocol.kind(target) && it.name == name }
    private fun edit(s: SimulatedPad, target: Int, name: String, value: String) = field(s,target,name).let { Edit(target,it.id,it.parse(value)) }
    private fun value(p: Protocol, s: SimulatedPad, target: Int, name: String, defaults: Boolean = false) = p.read(target,defaults).getValue(field(s,target,name).id)

    @Test fun realFirmwareSchemaLoadsEveryPage() {
        val s=sim(); val p=Protocol(s)
        assertEquals(s.identity,p.identity()); assertEquals(70,p.fields().size)
        assertEquals(19,Protocol.targets(s.fields).size)
        for (t in Protocol.targets(s.fields)) for (f in s.fields.filter { it.kind == Protocol.kind(t) }) {
            assertEquals(f.size,p.read(t).getValue(f.id).bytes.size)
            assertEquals(f.size,p.read(t,true).getValue(f.id).bytes.size)
        }
        assertEquals("Demo game",value(p,s,11,"name").bytes.cString())
        assertTrue(value(p,s,0,"bond").bytes.any { it != 0.toByte() })
    }
    @Test fun collectedWritesDoNotApplyUntilCommitAndDropCancels() {
        val s=sim(); val p=Protocol(s); val f=field(s,0,"primary_colour")
        val before=value(p,s,0,f.name).bytes
        p.command(0x22,out=byteArrayOf(0,f.id.toByte(),1,9))
        assertArrayEquals(byteArrayOf(1,0),p.command(0x26,length=256))
        assertArrayEquals(before,value(p,s,0,f.name).bytes)
        p.command(0x24); p.command(0x23)
        assertArrayEquals(before,value(p,s,0,f.name).bytes)
        p.send(s.identity,listOf(edit(s,0,f.name,"9")))
        assertArrayEquals(byteArrayOf(9),value(p,s,0,f.name).bytes)
        assertTrue(value(p,s,0,f.name).explicit)
        p.send(s.identity,listOf(Edit(0,f.id,ByteArray(0))))
        assertArrayEquals(before,value(p,s,0,f.name).bytes)
        assertFalse(value(p,s,0,f.name).explicit)
    }
    @Test fun settingsPersistAcrossSimulatedReconnects() {
        var saved: String?=null
        val s=sim { saved=it }; val p=Protocol(s)
        p.send(s.identity,listOf(edit(s,11,"name","Persisted game"),edit(s,8,"palette1","#123456")))
        val next=sim(saved); val q=Protocol(next)
        assertEquals("Persisted game",value(q,next,11,"name").bytes.cString())
        assertArrayEquals(unhex("123456"),value(q,next,8,"palette1").bytes)
    }
    @Test fun unsetRemapsFollowBaseButKeepTheirOwnNameAndRemapASwap() {
        val s=sim(); val p=Protocol(s)
        val initialA=value(p,s,10,"map").bytes
        p.send(s.identity,listOf(edit(s,18,"map","swap-xy"),edit(s,18,"name","Base name"),edit(s,18,"left_inner_deadzone","7")))
        assertArrayEquals(Remap.parseMap("swap-xy"),value(p,s,12,"map").bytes)
        assertFalse(value(p,s,12,"map").explicit)
        assertArrayEquals(initialA,value(p,s,10,"map").bytes)
        assertNotEquals("Base name",value(p,s,12,"name").bytes.cString())
        assertEquals(7,value(p,s,12,"left_inner_deadzone",true).bytes.u8(0))
    }
    @Test fun unpairAndTypeChangesRenewOnlyTheIntendedSlot() {
        val s=sim(); val p=Protocol(s)
        val old=value(p,s,0,"address").bytes; val other=value(p,s,1,"address").bytes
        p.send(s.identity,listOf(Edit(0,0xfe,ByteArray(0))))
        assertFalse(old.contentEquals(value(p,s,0,"address").bytes))
        assertArrayEquals(other,value(p,s,1,"address").bytes)
        assertTrue(value(p,s,0,"bond").bytes.all { it == 0.toByte() })
        val unpaired=value(p,s,0,"address").bytes
        p.send(s.identity,listOf(edit(s,0,"type","switch")))
        assertFalse(unpaired.contentEquals(value(p,s,0,"address").bytes))
        assertEquals(2,value(p,s,0,"address").bytes.u8(5) and 3)
    }
    @Test fun invalidRangeIsRefusedWithoutChangingSavedValues() {
        val s=sim(); val p=Protocol(s); val f=field(s,0,"report_hz")
        val before=value(p,s,0,f.name).bytes
        p.command(0x22,out=byteArrayOf(0,f.id.toByte(),2)+le32(1001).copyOf(2))
        assertArrayEquals(byteArrayOf(1,3),p.command(0x26,length=256))
        p.command(0x23)
        assertArrayEquals(before,value(p,s,0,f.name).bytes)
    }
    @Test fun animatedInputAndCommandLogsUseNormalWireFormats() {
        val s=sim(); val p=Protocol(s)
        val first=p.command(4,length=36); time+=2500
        val next=p.command(4,length=36)
        assertFalse(first.contentEquals(next)); assertEquals(36,next.size); assertEquals(1,next.u8(0))
        p.command(0x13,3); assertEquals(4,p.command(1,length=16).u8(0))
        p.command(0x1a,123)
        val log=p.command(2,1,512)
        assertEquals(1L,log.u32()); assertEquals(log.size-8,log.u32(4).toInt())
        assertTrue(log.copyOfRange(8,log.size).toString(Charsets.UTF_8).contains("Rumble"))
        assertEquals("Rainbow",p.command(0x19,2,128).cString())
        p.command(0x18); assertTrue(value(p,s,9,"calibration").explicit)
        assertThrows(IllegalStateException::class.java) { p.command(0x11) }
    }
}
