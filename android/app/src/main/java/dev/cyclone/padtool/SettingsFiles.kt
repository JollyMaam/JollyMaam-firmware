package dev.cyclone.padtool

import org.json.JSONObject

object SettingsFiles {
    fun backup(identity: Identity, fields: List<Field>, values: Map<Int, Map<Int, Value>>): String {
        val targets = JSONObject()
        for (t in Protocol.targets(fields)) {
            val row = JSONObject()
            for (f in fields.filter { it.kind == Protocol.kind(t) }) {
                val v = values[t]?.get(f.id) ?: continue
                row.put(f.name, JSONObject().put("value", f.show(v.bytes)).put("set", v.explicit))
            }
            targets.put(Protocol.targetNames[t], row)
        }
        return JSONObject().put("format", 1).put("build", identity.build).put("flash_id", identity.flashId)
            .put("notes", JSONObject()).put("targets", targets).toString(1)
    }
    /** Validate the entire file before staging anything; missing targets are untouched. */
    fun restore(text: String, fields: List<Field>, values: Map<Int, Map<Int, Value>>): List<Edit> {
        val file = JSONObject(text)
        require(file.getInt("format") == 1) { "Not a padtool format 1 settings backup" }
        val targets = file.getJSONObject("targets")
        val out = mutableListOf<Edit>()
        for (name in targets.keys()) {
            val t = Protocol.targetNames.indexOf(name)
            require(t in Protocol.targets(fields)) { "Backup target is not available: $name" }
            val row = targets.getJSONObject(name)
            val fs = fields.filter { it.kind == Protocol.kind(t) }
            for (fieldName in row.keys()) require(fs.any { it.name == fieldName }) { "Unknown backup field $name/$fieldName" }
            for (f in fs) {
                if (f.id !in values[t].orEmpty()) continue
                val v = row.optJSONObject(f.name)
                // Desktop whole-target resets retain pairing absent from a backup.
                if (v == null && f.name in listOf("address", "bond")) continue
                val raw = if (v != null && v.getBoolean("set")) f.parse(v.getString("value")) else ByteArray(0)
                out += Edit(t, f.id, raw)
            }
        }
        return out
    }
    fun exportPreset(name: String, map: ByteArray, rules: ByteArray) = JSONObject().put("format", 1)
        .put("name", name).put("map", Remap.showMap(map)).put("rules", Remap.showRules(rules)).toString(1)
    fun importPreset(text: String, fields: List<Field>, target: Int): List<Edit> {
        val data = JSONObject(text)
        require(data.getInt("format") == 1) { "Not a remap preset" }
        return listOf("name" to data.optString("name", ""), "map" to data.optString("map", "identity"), "rules" to data.optString("rules", "none")).map { (name, text) ->
            val f = fields.first { it.kind == 3 && it.name == name }; Edit(target, f.id, f.parse(text))
        }
    }
}
