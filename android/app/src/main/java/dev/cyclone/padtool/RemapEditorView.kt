package dev.cyclone.padtool

import android.app.AlertDialog
import android.annotation.SuppressLint
import android.content.Context
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

/** The map and rules are one per-key table, like padgui's RemapEditor. */
@SuppressLint("ViewConstructor") // Built from the selected session, never inflated from XML.
class RemapEditorView(context: Context, private val map: ByteArray, rulesRaw: ByteArray,
    private val savedMap: ByteArray, savedRules: ByteArray,
    private val defaultMap: ByteArray, defaultRules: ByteArray,
    private val editable: Boolean, private val stage: (ByteArray, ByteArray) -> Unit
) : LinearLayout(context) {
    private val rules = Remap.decodeRules(rulesRaw)
    private val original = Remap.decodeRules(savedRules)
    private val defaults = Remap.decodeRules(defaultRules)
    private val order = listOf("south", "east", "west", "north", "up", "down", "left", "right", "l1", "r1", "l4", "r4", "l3", "r3", "minus", "plus", "home", "m", "capture")
    private val triggerLabels = listOf("is held", "is held for", "is pressed", "is released", "is tapped under", "is double-tapped within")
    private val turbo = setOf("turbo", "toggle_turbo", "turbo_modifier", "toggle_turbo_modifier")
    private val timed = setOf("held_for", "tapped", "double_tapped")
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()
    private fun keyLabel(key: String) = if (key in listOf("m", "l1", "r1", "l4", "r4", "l3", "r3")) key.uppercase() else key.replaceFirstChar(Char::uppercaseChar)
    private fun text(parent: LinearLayout, value: String) = TextView(context).apply {
        text = value; textSize = 14f; gravity = Gravity.CENTER_VERTICAL; setPadding(dp(4), dp(4), dp(4), dp(4)); parent.addView(this)
    }
    private fun row(parent: LinearLayout) = LinearLayout(context).also { it.gravity = Gravity.CENTER_VERTICAL; parent.addView(it) }
    private fun button(parent: LinearLayout, value: String, tagValue: String, enabled: Boolean = editable, weight: Boolean = false, action: () -> Unit) = Button(context).apply {
        text = value; tag = tagValue; isAllCaps = false; isEnabled = enabled
        minWidth = 0; minimumWidth = 0; setPadding(dp(8), 0, dp(8), 0); textSize = 14f
        if (weight) layoutParams = LayoutParams(0, -2, 1f)
        setOnClickListener { action() }; parent.addView(this)
    }
    private fun mapping(bytes: ByteArray, key: String): String {
        val n = bytes.u8(Remap.keys.indexOf(key))
        return if (n == 255) "Nothing" else if (n in 1..20 && Remap.keys[n - 1].isNotEmpty()) Remap.keys[n - 1] else key
    }
    private fun state(key: String, bytes: ByteArray, list: List<Remap.Rule>) = mapping(bytes, key) to list.filter { it.input == key }
    private fun setMapping(bytes: ByteArray, key: String, to: String) {
        bytes[Remap.keys.indexOf(key)] = (if (to == "Nothing") 255 else if (to == key) 0 else Remap.keys.indexOf(to) + 1).toByte()
    }
    private fun choose(title: String, labels: List<String>, selected: Int, pick: (Int) -> Unit) {
        AlertDialog.Builder(context).setTitle(title).setSingleChoiceItems(labels.toTypedArray(), selected) { dialog, which ->
            dialog.dismiss(); pick(which)
        }.setNegativeButton("Cancel", null).show()
    }
    private fun summary(rule: Remap.Rule): String = buildString {
        append("When ${keyLabel(rule.input)} ${triggerLabels[Remap.triggers.indexOf(rule.trigger) - 1]}")
        if (rule.trigger in timed) append(" ${rule.ms} ms")
        append(": ${rule.effect.replace('_', ' ')}")
        if (rule.effect != "nothing") append(" ${rule.outputs.joinToString(" + ", transform = ::keyLabel).ifEmpty { "Choose keys…" }}")
        if (rule.effect in turbo) append(if (rule.rate == 0) " at every report" else " at ${rule.rate} Hz")
    }
    init {
        orientation = VERTICAL; tag = "remap-editor"
        text(this, "Button mappings")
        text(this, "While held, each key sends its mapping. Rules add actions to that mapping.")
        text(this, "Rule entries: ${rules.size} of 16 used").tag = "rule-count"
        button(this, "Select preset…", "remap-preset") {
            val names = listOf("swap-faces", "swap-south-east", "swap-west-north", "capture-off")
            choose("Mapping preset", listOf("Swap faces (south/east, west/north)", "Swap south/east", "Swap west/north", "Capture off"), -1) { i ->
                val preset = Remap.parseMap(names[i]); val updated = map.copyOf()
                for (key in order) if (mapping(preset, key) != key) setMapping(updated, key, mapping(preset, key))
                stage(updated, Remap.encodeRules(rules))
            }
        }
        val labels = order.map(::keyLabel)
        val namePaint = TextView(context).apply { textSize = 14f }.paint
        val valuePaint = Button(context).apply { textSize = 14f }.paint
        val nameWidth = maxOf(dp(64), kotlin.math.ceil(labels.maxOf { namePaint.measureText(it) }.toDouble()).toInt() + dp(8))
        val valueWidth = maxOf(dp(80), kotlin.math.ceil((labels + "Nothing").maxOf { valuePaint.measureText(it) }.toDouble()).toInt() + dp(16))
        for (key in order) {
            val current = state(key, map, rules)
            val dirty = current != state(key, savedMap, original)
            val custom = current != state(key, defaultMap, defaults)
            val line = row(this)
            button(line, if (dirty) "*" else if (custom) "✓" else "—", "remap-status-$key", editable && (dirty || custom)) {
                val sourceMap = if (dirty) savedMap else defaultMap
                val sourceRules = if (dirty) original else defaults
                val updated = map.copyOf(); setMapping(updated, key, mapping(sourceMap, key))
                val restored = rules.filter { it.input != key } + sourceRules.filter { it.input == key }
                if (restored.size > 16) {
                    AlertDialog.Builder(context).setTitle("Rule limit").setMessage("Restoring this key would exceed 16 rules. Remove another rule first.").setPositiveButton("OK", null).show()
                } else stage(updated, Remap.encodeRules(restored))
            }.apply {
                layoutParams = LayoutParams(dp(40), dp(48))
                contentDescription = "${if (dirty) "Discard" else "Default"} ${keyLabel(key)} mapping and rules"
            }
            val name = text(line, keyLabel(key)).apply { layoutParams = LayoutParams(nameWidth, -2) }
            text(line, "maps to")
            val to = mapping(map, key)
            val value = button(line, if (to == "Nothing") to else keyLabel(to), "remap-map-$key") {
                val choices = order + "Nothing"
                choose("${keyLabel(key)} maps to", choices.map { if (it == "Nothing") it else keyLabel(it) }, choices.indexOf(to)) { i ->
                    val updated = map.copyOf(); setMapping(updated, key, choices[i]); stage(updated, Remap.encodeRules(rules))
                }
            }.apply { layoutParams = LayoutParams(valueWidth, -2, 1f) }
            val help = "While held, ${keyLabel(key)} sends this mapping. Rules add to it.\n\nDefault: " +
                mapping(defaultMap, key).let { if (it == "Nothing") it else keyLabel(it) } +
                defaults.filter { it.input == key }.joinToString("\n", prefix = "\n") { summary(it) }
            val showHelp = View.OnLongClickListener {
                AlertDialog.Builder(context).setTitle(keyLabel(key)).setMessage(help).setPositiveButton("OK", null).show(); true
            }
            value.setOnLongClickListener(showHelp); name.setOnLongClickListener(showHelp)
            button(line, "+ Rule", "remap-add-$key", editable && rules.size < 16) { editRule(Remap.Rule(key), null) }
            for ((index, rule) in rules.withIndex().filter { it.value.input == key }) {
                val ruleLine = row(this).apply { setPadding(dp(36), 0, 0, 0) }
                button(ruleLine, summary(rule), "remap-rule-$index", weight = true) { editRule(rule, index) }.setOnLongClickListener(showHelp)
                button(ruleLine, "×", "remap-delete-$index") { stage(map.copyOf(), Remap.encodeRules(rules.filterIndexed { n, _ -> n != index })) }
                    .contentDescription = "Delete rule for ${keyLabel(key)}"
            }
        }
    }
    private fun editRule(initial: Remap.Rule, index: Int?) {
        var rule = initial
        val body = LinearLayout(context).apply { orientation = VERTICAL; setPadding(dp(12), 0, dp(12), 0) }
        fun number(title: String, initialValue: Int, range: IntRange, save: (Int) -> Unit) {
            val editor = EditText(context).apply { inputType = InputType.TYPE_CLASS_NUMBER; setText(initialValue.toString()); selectAll() }
            val dialog = AlertDialog.Builder(context).setTitle(title).setView(editor).setPositiveButton("OK", null).setNegativeButton("Cancel", null).create()
            dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val n = editor.text.toString().toIntOrNull()
                if (n == null || n !in range) editor.error = "Expected ${range.first}–${range.last}"
                else { dialog.dismiss(); save(n) }
            } }; dialog.show()
        }
        fun draw() {
            body.removeAllViews()
            val whenRow = row(body); text(whenRow, "When ${keyLabel(rule.input)}")
            button(whenRow, triggerLabels[Remap.triggers.indexOf(rule.trigger) - 1], "rule-trigger", weight = true) {
                choose("Trigger", triggerLabels, Remap.triggers.indexOf(rule.trigger) - 1) { i ->
                    val trigger = Remap.triggers[i + 1]; val allowed = Remap.effectsFor(trigger)
                    val twins = mapOf("hold" to "tap", "tap" to "hold", "toggle_hold" to "hold", "turbo" to "toggle_turbo", "toggle_turbo" to "turbo", "turbo_modifier" to "toggle_turbo_modifier", "toggle_turbo_modifier" to "turbo_modifier")
                    val effect = rule.effect.takeIf { it in allowed } ?: twins[rule.effect]?.takeIf { it in allowed } ?: allowed.first()
                    val ms = if (trigger == "tapped" && rule.trigger !in timed) 200 else rule.ms.takeIf { it in 10..2550 } ?: 300
                    rule = rule.copy(trigger = trigger, effect = effect, ms = ms); draw()
                }
            }
            if (rule.trigger in timed) button(body, "${rule.ms} ms", "rule-time") {
                number("Time in milliseconds (10–2550)", rule.ms, 10..2550) { rule = rule.copy(ms = kotlin.math.round(it / 10.0).toInt() * 10); draw() }
            }
            val effectRow = row(body); text(effectRow, "Effect:")
            button(effectRow, rule.effect.replace('_', ' '), "rule-effect", weight = true) {
                val allowed = Remap.effectsFor(rule.trigger)
                choose("Effect", allowed.map { it.replace('_', ' ') }, allowed.indexOf(rule.effect)) { rule = rule.copy(effect = allowed[it]); draw() }
            }
            if (rule.effect != "nothing") {
                text(body, if (rule.effect.endsWith("modifier")) "Pulse what these pressed keys send:" else "Send these keys:")
                button(body, rule.outputs.joinToString(" + ", transform = ::keyLabel).ifEmpty { "Choose keys…" }, "rule-outputs") {
                    val chosen = rule.outputs.toMutableSet()
                    AlertDialog.Builder(context).setTitle("Output keys").setMultiChoiceItems(order.map(::keyLabel).toTypedArray(), order.map { it in chosen }.toBooleanArray()) { _, which, checked ->
                        if (checked) chosen.add(order[which]) else chosen.remove(order[which])
                    }.setPositiveButton("Done") { _, _ -> rule = rule.copy(outputs = order.filter { it in chosen }); draw() }.setNegativeButton("Cancel", null).show()
                }
            }
            if (rule.effect in turbo) button(body, if (rule.rate == 0) "Every report (max)" else "${rule.rate} Hz", "rule-rate") {
                val rates = (1..30).toList() + 0
                choose("Turbo rate", rates.map { if (it == 0) "Every report (max)" else "$it Hz" }, rates.indexOf(rule.rate)) { rule = rule.copy(rate = rates[it]); draw() }
            }
        }
        draw()
        val dialog = AlertDialog.Builder(context).setTitle("${keyLabel(initial.input)} rule")
            .setView(PageScrollView(context).apply { addView(body) }).setPositiveButton("Stage", null).setNegativeButton("Cancel", null).create()
        dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            val updated = rules.toMutableList()
            if (index == null) updated.add(rule) else updated[index] = rule
            val raw = Remap.encodeRules(updated)
            dialog.dismiss(); stage(map.copyOf(), raw)
        } }; dialog.show()
    }
}
