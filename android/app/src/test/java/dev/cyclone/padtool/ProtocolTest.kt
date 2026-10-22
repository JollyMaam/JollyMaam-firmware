package dev.cyclone.padtool

import org.junit.Assert.*
import org.junit.Test
import org.json.JSONObject
import java.io.IOException

class ProtocolTest {
    private fun fixture(name: String) = javaClass.getResourceAsStream("/$name")!!.use { it.readBytes() }
    @Test fun pythonRemapAndCurveCompatibility() {
        val data = JSONObject(fixture("python-vectors.json").toString(Charsets.UTF_8))
        for (kind in listOf("maps", "rules")) {
            val rows = data.getJSONArray(kind)
            repeat(rows.length()) { i ->
                val row = rows.getJSONObject(i)
                val raw = if (kind == "maps") Remap.parseMap(row.getString("input")) else Remap.parseRules(row.getString("input"))
                assertEquals(row.getString("hex"), raw.hex())
                assertEquals(row.getString("text"), if (kind == "maps") Remap.showMap(raw) else Remap.showRules(raw))
                if (kind == "rules") assertArrayEquals(raw, Remap.encodeRules(Remap.decodeRules(raw)))
            }
        }
        val curves = data.getJSONArray("curves")
        repeat(curves.length()) { i -> val row = curves.getJSONObject(i); assertEquals(row.getString("hex"), Remap.curve(row.getString("name")).hex()) }
    }
    @Test fun rejectsBadFieldsAndRules() {
        assertThrows(IllegalArgumentException::class.java) { Remap.parseRules("r4 pressed hold a") }
        assertThrows(IllegalArgumentException::class.java) { Remap.parseRules("r4 held_for:2560 hold a") }
        assertThrows(IllegalArgumentException::class.java) { Remap.parseRules("r4 held turbo a@31") }
        assertThrows(IllegalArgumentException::class.java) { Field.decode(byteArrayOf(1,0,1,0,1,0,0,0,65)) }
        assertThrows(IllegalArgumentException::class.java) { Protocol.decodeValues(byteArrayOf(1,0,1,2,1,8,1), 0) }
        assertThrows(IllegalArgumentException::class.java) { Protocol.decodeValues(byteArrayOf(1,1,0), 0) }
        val f = Field(3, 10, 3, 15, 0, false, "left_curve_points", "", emptyList())
        assertThrows(IllegalArgumentException::class.java) { f.parse("255," + List(14) { "0" }.joinToString(",")) }
        val number = Field(0, 1, 1, 2, 100, false, "timeout", "", emptyList())
        assertThrows(IllegalArgumentException::class.java) { number.parse("101") }
    }
    private val identityBytes = ByteArray(64).apply { "C2PD".toByteArray().copyInto(this); this[4] = 1; this[24] = 42 }
    private class Device(val identity: ByteArray) : Scsi {
        val commands = mutableListOf<Int>()
        val chunks = mutableListOf<ByteArray>()
        var reject = false
        var truncate = false
        var commitFails = false
        var lastCount = 0
        override fun transfer(cdb: ByteArray, length: Int, out: ByteArray?, timeout: Int, rawLength: Boolean): ByteArray {
            val sub = cdb.u8(1); commands += sub
            return when (sub) {
                0 -> identity
                0x22 -> {
                    chunks += out!!
                    var at = 0; lastCount = 0
                    while (at < out.size) { at += 3 + out.u8(at + 2); lastCount++ }
                    ByteArray(0)
                }
                0x26 -> if (truncate) byteArrayOf(lastCount.toByte()) else ByteArray(lastCount + 1).apply { this[0] = lastCount.toByte(); if (reject) this[1] = 3 }
                0x23 -> if (commitFails) throw IOException("disconnected") else ByteArray(0)
                else -> ByteArray(0)
            }
        }
    }
    @Test fun validatesAllWritesBeforeOneCommit() {
        val d = Device(identityBytes); val p = Protocol(d)
        val items = List(4) { Edit(10, it + 1, ByteArray(160)) }
        p.send(Identity.decode(identityBytes), items)
        assertEquals(2, d.chunks.size)
        assertEquals(1, d.commands.count { it == 0x23 })
        assertEquals(listOf(0, 0x24, 0x22, 0x26, 0x22, 0x26, 0x23), d.commands)
    }
    @Test fun refusedOrShortResultDropsWithoutCommit() {
        for (short in listOf(false, true)) {
            val d = Device(identityBytes).apply { truncate = short; reject = !short }
            assertThrows(IllegalArgumentException::class.java) { Protocol(d).send(Identity.decode(identityBytes), listOf(Edit(0, 1, byteArrayOf(1)))) }
            assertFalse(0x23 in d.commands); assertEquals(0x24, d.commands.last())
        }
    }
    @Test fun identityAndCapacityAreCheckedBeforeWriting() {
        val d = Device(identityBytes)
        assertThrows(IllegalArgumentException::class.java) { Protocol(d).send(Identity.decode(identityBytes).copy(flashId = "wrong"), listOf(Edit(0,1,byteArrayOf(1)))) }
        assertEquals(listOf(0), d.commands)
        d.commands.clear()
        assertThrows(IllegalArgumentException::class.java) { Protocol(d).send(Identity.decode(identityBytes), List(7) { Edit(10,it+1,ByteArray(160)) }) }
        assertEquals(listOf(0), d.commands)
    }
    @Test fun ambiguousCommitDoesNotClaimSuccess() {
        val d = Device(identityBytes).apply { commitFails = true }
        var completed = false
        val error = assertThrows(IOException::class.java) { Protocol(d).send(Identity.decode(identityBytes), listOf(Edit(0,0xfe,ByteArray(0)))) { completed = true } }
        assertTrue(error.message!!.contains("refresh")); assertFalse(completed)
    }
    @Test fun draftsRespectScopeAndFirmwareFieldOrder() {
        val type = Field(0,3,0,1,1,false,"type","",listOf("a","b"))
        val addr = Field(0,1,3,6,0,false,"address","",emptyList())
        val d = Drafts()
        d.stage(0,addr,ByteArray(6) { 2 },Value(ByteArray(6),true))
        d.stage(1,type,byteArrayOf(1),Value(byteArrayOf(0),false))
        d.stage(0,type,byteArrayOf(1),Value(byteArrayOf(0),false))
        assertEquals(listOf(3,1), d.items(listOf(type,addr),0).map { it.id })
        d.discard(0); assertEquals(listOf(1), d.items(listOf(type,addr)).map { it.target })
        d.stage(1,type,ByteArray(0),Value(byteArrayOf(0),false)); assertTrue(d.edits.isEmpty())
    }
    @Test fun settingsBackupAndPresetRoundTrip() {
        val f = listOf(Field(3,1,4,24,0,false,"name","",emptyList()), Field(3,2,3,20,0,false,"map","",emptyList()), Field(3,3,3,160,0,false,"rules","",emptyList()))
        val row = f.associate { it.id to Value(it.parse(when(it.name) { "name" -> "Mario Kart"; "map" -> "swap-ab"; else -> "r4 held turbo_modifier a@15" }), true) }
        val values = mapOf(10 to row)
        val restored = SettingsFiles.restore(SettingsFiles.backup(Identity.decode(identityBytes),f,values),f,values)
        assertEquals(3, restored.count { it.target == 10 })
        restored.filter { it.target == 10 }.forEach { assertArrayEquals(row.getValue(it.id).bytes, it.bytes) }
        val text = SettingsFiles.exportPreset("Mario Kart",row.getValue(2).bytes,row.getValue(3).bytes)
        val preset = SettingsFiles.importPreset(text,f,10)
        preset.forEach { assertArrayEquals(row.getValue(it.id).bytes,it.bytes) }
    }
    @Test fun restorePreservesBackupPairingAfterTypeRenewsIdentity() {
        val type = Field(0,3,0,1,1,false,"type","",listOf("old","new"))
        val address = Field(0,1,3,6,0,false,"address","",emptyList())
        val bond = Field(0,2,3,8,0,false,"bond","",emptyList())
        val fields = listOf(type,address,bond)
        val values = mapOf(0 to mapOf(3 to Value(byteArrayOf(0),true),1 to Value(ByteArray(6) { 7 },true),2 to Value(ByteArray(8) { 8 },true)))
        val edits = listOf(Edit(0,3,byteArrayOf(1)),Edit(0,1,ByteArray(6) { 7 }),Edit(0,2,ByteArray(8) { 8 }))
        val drafts = Drafts()
        drafts.stageImport(fields,values,edits,true)
        assertEquals(listOf(3,1,2),drafts.items(fields).map { it.id })
        assertArrayEquals(edits[1].bytes,drafts.items(fields)[1].bytes)
    }
}
