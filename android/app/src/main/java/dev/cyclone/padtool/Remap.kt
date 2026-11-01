package dev.cyclone.padtool

import kotlin.math.round

/** Text and wire formats shared with tools/padtool/padremap.py. */
object Remap {
    data class Rule(val input: String, val trigger: String = "held", val effect: String = "hold",
        val outputs: List<String> = emptyList(), val ms: Int = 300, val rate: Int = 15)
    fun effectsFor(trigger: String) = if (trigger in listOf("held", "held_for"))
        listOf("hold", "turbo", "turbo_modifier", "nothing") else listOf("tap", "toggle_hold", "toggle_turbo", "toggle_turbo_modifier")
    fun decodeRules(raw: ByteArray): List<Rule> = (0 until raw.size / 10).mapNotNull { n ->
        val at = n * 10; val t = raw.u8(at); val k = raw.u8(at + 1); val e = raw.u8(at + 3)
        if (t !in 1..6 || k !in keys.indices || keys[k].isEmpty() || e !in effects.indices) null else
            Rule(keys[k], triggers[t], effects[e], keys.indices.filter { keys[it].isNotEmpty() && raw.u32(at + 4) and (1L shl it) != 0L }.map { keys[it] },
                if (triggers[t] in timed) raw.u8(at + 2) * 10 else 300, if (effects[e] in turbo) raw.u8(at + 8) else 15)
    }
    fun encodeRules(rules: List<Rule>): ByteArray = parseRules(rules.joinToString("; ") { r ->
        "${r.input} ${r.trigger}" + (if (r.trigger in timed) ":${r.ms}" else "") + " ${r.effect}" +
            (if (r.effect != "nothing") " " + r.outputs.joinToString("+").ifEmpty { "-" } else "") +
            (if (r.effect in turbo) "@${if (r.rate == 0) "max" else r.rate}" else "")
    })
    val keys = listOf("a", "b", "capture", "x", "y", "m", "l1", "r1", "l4", "r4", "minus", "plus", "home", "l3", "r3", "", "up", "right", "down", "left")
    val triggers = listOf("", "held", "held_for", "pressed", "released", "tapped", "double_tapped")
    val effects = listOf("hold", "turbo", "nothing", "tap", "toggle_hold", "toggle_turbo", "turbo_modifier", "toggle_turbo_modifier")
    private val timed = mapOf("held_for" to 300, "tapped" to 200, "double_tapped" to 300)
    private val turbo = setOf("turbo", "toggle_turbo", "turbo_modifier", "toggle_turbo_modifier")
    private val presets = mapOf("swap-ab-xy" to "a=b b=a x=y y=x", "swap-ab" to "a=b b=a", "swap-xy" to "x=y y=x", "capture-off" to "capture=none")
    private fun key(text: String): Int {
        val name = mapOf("select" to "minus", "start" to "plus", "screenshot" to "capture").getOrDefault(text.lowercase(), text.lowercase())
        return keys.indexOf(name).also { require(name.isNotEmpty() && it >= 0) { "Unknown key: $text" } }
    }
    fun parseMap(input: String): ByteArray {
        val text = presets.getOrDefault(input, input)
        val raw = ByteArray(20)
        if (text.isBlank() || text == "identity") return raw
        for (part in text.split(Regex("\\s+"))) {
            val pair = part.split('='); require(pair.size == 2) { "Use key=key or key=none" }
            val from = key(pair[0]); val to = if (pair[1].lowercase() == "none") -1 else key(pair[1])
            raw[from] = (if (to == -1) 255 else if (to == from) 0 else to + 1).toByte()
        }
        return raw
    }
    fun showMap(raw: ByteArray): String = keys.indices.filter { keys[it].isNotEmpty() }.mapNotNull { i ->
        val n = if (i < raw.size) raw.u8(i) else 0
        val to = if (n == 255) "none" else if (n in 1..20 && keys[n - 1].isNotEmpty()) keys[n - 1] else keys[i]
        if (to != keys[i]) "${keys[i]}=$to" else null
    }.joinToString(" ").ifEmpty { "identity" }

    fun parseRules(text: String): ByteArray {
        val raw = ByteArray(160)
        if (text.isBlank() || text.trim() == "none") return raw
        val rules = text.split(';').filter { it.isNotBlank() }; require(rules.size <= 16) { "At most 16 rules" }
        rules.forEachIndexed { n, rule ->
            val words = rule.trim().split(Regex("\\s+")); require(words.size in 3..4) { "Use INPUT TRIGGER[:MS] EFFECT [OUT+OUT][@HZ]" }
            val trigger = words[1].substringBefore(':'); val t = triggers.indexOf(trigger)
            val effect = words[2]; val e = effects.indexOf(effect)
            require(t > 0 && e >= 0) { "Unknown trigger or effect" }
            val allowed = if (trigger in listOf("held", "held_for")) setOf("hold", "turbo", "turbo_modifier", "nothing") else setOf("tap", "toggle_hold", "toggle_turbo", "toggle_turbo_modifier")
            require(effect in allowed) { "$trigger cannot use $effect" }
            val ms = if (trigger in timed) words[1].substringAfter(':', timed.getValue(trigger).toString()).toInt().also { require(it in 10..2550) { "Time must be 10–2550 ms" } } else 0
            val outputs = words.getOrElse(3) { "" }.substringBefore('@')
            val hz = if (effect in turbo) words.getOrElse(3) { "" }.substringAfter('@', "15").let { if (it == "max") 0 else it.toInt() }.also { require(it in 0..30) { "Turbo must be 1–30 Hz or max" } } else 0
            var mask = 0L
            for (out in outputs.split('+').filter { it.isNotBlank() && it != "-" }) { val bit = key(out); if (effect != "nothing") mask = mask or (1L shl bit) }
            val at = n * 10
            raw[at] = t.toByte(); raw[at + 1] = key(words[0]).toByte(); raw[at + 2] = round(ms / 10.0).toInt().toByte(); raw[at + 3] = e.toByte()
            le32(mask).copyInto(raw, at + 4); raw[at + 8] = hz.toByte()
        }
        return raw
    }
    fun showRules(raw: ByteArray): String = (0 until raw.size / 10).mapNotNull { n ->
        val at = n * 10; val t = raw.u8(at); val k = raw.u8(at + 1); val e = raw.u8(at + 3)
        if (t !in 1..6 || k !in keys.indices || keys[k].isEmpty() || e !in effects.indices) null else {
            val trigger = triggers[t]; val effect = effects[e]
            val time = if (trigger in timed) ":${raw.u8(at + 2) * 10}" else ""
            val mask = raw.u32(at + 4)
            val outputs = keys.indices.filter { keys[it].isNotEmpty() && mask and (1L shl it) != 0L }.joinToString("+") { keys[it] }.ifEmpty { "-" }
            val rate = if (effect in turbo) "@${if (raw.u8(at + 8) == 0) "max" else raw.u8(at + 8).toString()}" else ""
            "${keys[k]} $trigger$time $effect" + (if (effect != "nothing") " $outputs" else "") + rate
        }
    }.joinToString("; ").ifEmpty { "none" }

    fun curve(name: String, count: Int = 15): ByteArray {
        require(name in listOf("linear", "concave", "s-curve"))
        return ByteArray(count) { i ->
            val x = (i + 1).toDouble() / (count + 1)
            val y = when (name) { "concave" -> x * x; "s-curve" -> if (x <= .5) 2*x*(1-x) else 1-2*(1-x)*x; else -> x }
            round(y * 255).toInt().toByte()
        }
    }
}
