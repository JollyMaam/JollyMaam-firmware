@file:Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
package dev.cyclone.padtool

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.text.TextUtils
import android.view.View
import android.view.WindowManager
import android.widget.*
import java.io.ByteArrayOutputStream

/** Small native forms; protocol, drafts and USB operations do not live in widgets. */
class MainActivity : Activity() {
    private lateinit var model: AppModel
    private lateinit var root: LinearLayout
    private lateinit var status: TextView
    private var liveText: TextView? = null
    private var pageScroll: PageScrollView? = null
    private var scrollKey = ""
    private var scrollReady = false
    private val handler = Handler(Looper.getMainLooper())
    private val permissionAction get() = "$packageName.USB_PERMISSION"
    private val polling = object : Runnable {
        override fun run() { model.poll(); handler.postDelayed(this, if (model.live == "input") 100 else 300) }
    }
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == permissionAction) {
                val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                if (device != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) && model.usb.hasPermission(device) && !model.busy) model.load(device)
                else { model.message = "USB access was not granted"; render() }
            } else if (intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED) {
                val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE)
                if (device?.deviceId == model.selected?.deviceId) {
                    model.selected = null; model.live = null; model.loaderMode = false
                    model.loaderPid = null; model.pendingImage = null
                    model.message = "USB device disconnected. Pending edits are retained."
                    render()
                }
                if (!model.busy) model.refreshDevices()
            } else if (!model.busy) model.refreshDevices()
        }
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        model = lastNonConfigurationInstance as? AppModel ?: AppModel(applicationContext)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, IntentFilter(permissionAction), Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(receiver, IntentFilter(permissionAction))
        val filter = IntentFilter(UsbManager.ACTION_USB_DEVICE_ATTACHED).apply { addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED) else registerReceiver(receiver, filter)
        model.changed = ::render; model.progressChanged = ::updateProgress
        render(); model.refreshDevices()
    }
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent); setIntent(intent)
        if (!model.busy) {
            model.rescan()
        }
    }
    override fun onResume() {
        super.onResume(); model.changed = ::render; model.progressChanged = ::updateProgress
        handler.post(polling); render()
    }
    override fun onPause() {
        handler.removeCallbacks(polling); model.live = null
        model.changed = null; model.progressChanged = null
        super.onPause()
    }
    override fun onDestroy() { unregisterReceiver(receiver); if (!isChangingConfigurations) model.close(); super.onDestroy() }
    override fun onRetainNonConfigurationInstance(): Any = model
    private fun dp(n: Int) = (n * resources.displayMetrics.density).toInt()
    private fun column() = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
    private fun label(text: String, size: Float = 16f) = TextView(this).apply {
        this.text = text; textSize = size; setPadding(dp(4), dp(6), dp(4), dp(6))
    }
    private fun button(parent: LinearLayout, text: String, enabled: Boolean = true, action: () -> Unit): Button = Button(this).apply {
        this.text = text; isAllCaps = false; isEnabled = enabled && !model.busy
        setOnClickListener { if (!model.busy) action() }; parent.addView(this)
    }
    private fun row(parent: LinearLayout): LinearLayout {
        val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        parent.addView(HorizontalScrollView(this).apply { addView(line) })
        return line
    }
    private fun updateProgress() {
        if (!::status.isInitialized) return
        status.text = statusLine(); status.contentDescription = statusDetails()
        liveText?.text = model.console
    }
    private fun targetLabel(target: Int) = Protocol.targetNames[target].split('-').joinToString(" ") { it.replaceFirstChar(Char::uppercaseChar) }
    private fun pageName(): String = when {
        model.page == -2 || !model.hasDevice -> "Devices"
        model.loaderMode -> "ROM loader"
        model.page >= 0 -> targetLabel(model.page)
        model.page == -3 -> "Firmware console"
        model.page == -4 -> "Live input"
        model.simulation != null -> "Simulated pad"
        model.session?.identity?.board == 1 -> "Dongle"
        else -> "Pad"
    }
    private fun scope() = model.page.takeIf { it >= 0 }
    private fun statusLine(): String = buildList {
        add(pageName())
        if (model.simulation != null && model.page != -1) add("Simulation")
        model.session?.let { s ->
            if (s.fields.isNotEmpty()) add("${s.drafts.edits.count { scope() == null || it.key.first == scope() }} pending")
            if (s.needsRefresh) add("Reopen device")
        }
        add((if (model.busy) "… " else "") + model.message)
        if (model.loaderMode) {
            add("Product ${model.loaderPid ?: "unread"}")
            add(if (model.loaderFile.isFile) "RAM helper ready" else "Select RAM helper")
        } else if (model.hasDevice) model.session?.status?.takeIf { it.isNotBlank() }?.let(::add)
    }.joinToString(" · ").replace(Regex("\\s+"), " ")
    private fun statusDetails() = statusLine() + (model.session?.identity?.let { "\nBuild ${it.build} · protocol ${it.protocol}\n${it.flashId}" } ?: "")
    private fun back() {
        model.live = null
        model.page = if (model.page != -1 && model.page != -2 && model.hasDevice) -1 else -2
        render()
    }
    private fun render() {
        if (scrollReady) pageScroll?.let { model.scrollPositions[scrollKey] = it.scrollY }
        root = column().apply { setPadding(dp(12), 0, dp(12), 0) }
        root.setOnApplyWindowInsetsListener { v, insets ->
            val top = insets.systemWindowInsetTop; val bottom = insets.systemWindowInsetBottom
            v.setPadding(dp(12), top, dp(12), bottom); insets
        }
        setContentView(root)
        val toolbar = row(root)
        toolbar.tag = "toolbar"
        applyBar(toolbar, model.session, scope())
        button(toolbar, "Rescan") { model.rescan() }
        if (model.page != -2 && model.hasDevice) button(toolbar, "Back") { back() }
        for (i in 0 until toolbar.childCount) (toolbar.getChildAt(i) as? Button)?.apply {
            minWidth = 0; minimumWidth = 0; minHeight = dp(40); minimumHeight = dp(40)
            setPadding(dp(8), 0, dp(8), 0); textSize = 14f
        }
        status = label(statusLine(), 13f).apply {
            tag = "status"; setSingleLine(true); ellipsize = TextUtils.TruncateAt.END
            contentDescription = statusDetails()
            setOnLongClickListener {
                AlertDialog.Builder(this@MainActivity).setTitle(pageName()).setMessage(statusDetails()).setPositiveButton("OK", null).show()
                true
            }
        }
        root.addView(status)
        if (model.busy) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val content = column()
        val key = if (model.page == -2 || !model.hasDevice) "devices" else "${model.session?.identity?.flashId ?: model.selected?.deviceName}:${model.page}"
        var restoring = true
        val scroll = PageScrollView(this).apply {
            tag = "page"; addView(content)
            setOnScrollChangeListener { _, _, y, _, _ -> if (!restoring) model.scrollPositions[key] = y }
        }
        root.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        pageScroll = scroll; scrollKey = key; scrollReady = false
        scroll.requestFocus()
        scroll.addOnLayoutChangeListener(object : View.OnLayoutChangeListener {
            override fun onLayoutChange(view: View, left: Int, top: Int, right: Int, bottom: Int, oldLeft: Int, oldTop: Int, oldRight: Int, oldBottom: Int) {
                if (right <= left || bottom <= top) return
                scroll.removeOnLayoutChangeListener(this)
                if (pageScroll === scroll) {
                    scroll.scrollTo(0, model.scrollPositions[key] ?: 0)
                    restoring = false; scrollReady = true
                }
            }
        })
        liveText = null
        when {
            model.page == -2 || !model.hasDevice -> devicePage(content)
            model.loaderMode -> loaderPage(content)
            model.session == null -> content.addView(label("Select a tool device to read settings."))
            model.page == -3 || model.page == -4 -> livePage(content)
            model.page >= 0 -> settingsPage(content, model.session!!, model.page)
            else -> overview(content, model.session!!)
        }
    }
    private fun choose(device: UsbDevice) {
        if (model.usb.hasPermission(device)) model.load(device)
        else {
            val intent = Intent(permissionAction).setPackage(packageName)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
            model.usb.requestPermission(device, PendingIntent.getBroadcast(this, device.deviceId, intent, flags))
        }
    }
    private fun devicePage(parent: LinearLayout) {
        if (BuildConfig.DEBUG) button(parent, "Simulated pad (no USB)") { model.loadSimulation() }
        parent.addView(label("USB tool mode is required. On the pad, end the active gamepad session first. Bluetooth configuration is not available."))
        for (device in model.devices) button(parent, "${device.productName ?: "USB device"} · %04x:%04x\n${device.deviceName}".format(device.vendorId, device.productId)) { choose(device) }
        if (model.devices.isEmpty()) parent.addView(label("No tool devices found. Use a USB data cable and a phone with USB host support."))
        for (s in model.sessions.values.filter { it.drafts.edits.isNotEmpty() && (!model.hasDevice || it !== model.session) }) {
            parent.addView(label("Retained edits: ${s.identity.flashId.take(12)} · ${s.drafts.edits.size} pending"))
            button(parent, "Discard those edits") { s.drafts.discard(); model.saveDrafts(s); render() }
        }
    }
    private fun applyBar(bar: LinearLayout, s: PadSession?, target: Int?) {
        val count = s?.drafts?.edits?.count { target == null || it.key.first == target } ?: 0
        button(bar, "Apply", count > 0 && s?.needsRefresh == false && model.hasDevice && !model.loaderMode) { model.apply(target) }
        button(bar, "Discard", count > 0) { s!!.drafts.discard(target); model.saveDrafts(s); render() }
        button(bar, "Defaults", s?.fields?.isNotEmpty() == true && !model.loaderMode) {
            if (s == null) return@button
            val targets = if (target == null) Protocol.targets(s.fields) else listOf(target)
            for (t in targets) s.drafts.edits.remove(t to 0xfe)
            val edits = targets.flatMap { t -> s.fields.filter { it.kind == Protocol.kind(t) }.map { Edit(t, it.id, ByteArray(0)) } }
            model.stage(s, edits)
        }
    }
    private fun overview(parent: LinearLayout, s: PadSession) {
        val id = s.identity
        if (id.board == 0) {
            for (t in Protocol.targets(s.fields)) button(parent, targetLabel(t) + if (s.drafts.edits.any { it.key.first == t }) " *" else "") { model.page = t; render() }
            button(parent, "Back up saved settings…") { document("backup-settings", true, "cyclone-settings.json", "application/json") }
            button(parent, "Restore settings…", s.drafts.edits.isEmpty()) { document("restore-settings", false, mime = "application/json") }
            button(parent, "Live input") { model.page = -4; model.console = ""; model.live = "input"; render() }
            button(parent, "LED demos") { model.run("Reading LED demos") {
                val names = model.withPad { pad, _ -> buildList {
                    for (i in 0..254) {
                        val name = pad.command(0x19, i.toLong(), 128).cString()
                        if (name.isEmpty()) break
                        add(name)
                    }
                } }
                handler.post { if (!isFinishing && !isDestroyed) AlertDialog.Builder(this).setTitle("LED demo (30 seconds)").setItems((names + "Stop").toTypedArray()) { _, i -> action("LED demo", 0x19, if (i == names.size) 255 else i or 0x100, 128) }.show() }
            } }
            button(parent, "Test rumble") { rumbleDialog() }
        } else {
            button(parent, "Pair dongle") { action("Starting dongle pairing", 0x17) }
            button(parent, "Stay in tool mode until power loss") { action("Enabling tool mode", 0x31) }
            button(parent, "Connect to address…") { textDialog("Target address", "AA:BB:CC:DD:EE:FF", "", { text ->
                val raw = unhex(text.replace(":", "")); require(raw.size == 6) { "Expected six address bytes" }
                model.run("Connecting dongle") { model.transport { tr ->
                    require(Protocol(tr).identity() == s.identity) { "Device changed" }
                    tr.transfer(byteArrayOf(0xf5.toByte(), 0x30) + raw)
                } }
            }) }
        }
        button(parent, "Firmware console") { model.page = -3; model.live = "console"; render() }
        button(parent, "Restart device", s.drafts.edits.isEmpty()) { action("Restarting", 0x10) }
        if (model.simulation != null) {
            button(parent, "Reset simulated pad") { model.loadSimulation(reset = true) }
            button(parent, "Disconnect simulated pad") { model.disconnectSimulation() }
        }
        button(parent, "Enter flash loader", s.drafts.edits.isEmpty() && model.simulation == null) {
            confirm("Enter flash loader", "The device will reconnect as UBOOT. Select it from Devices and grant USB access again.") { action("Entering flash loader", 0x11) }
        }
    }
    private fun settingsPage(parent: LinearLayout, s: PadSession, target: Int) {
        if (target < 8) {
            val actions = row(parent)
            button(actions, "Connect", s.drafts.edits.none { it.key.first == target }) { action("Connecting slot", 0x13, target) }
            button(actions, "Pair", s.drafts.edits.none { it.key.first == target }) { action("Pairing slot", 0x17, target) }
            val bond = s.fields.firstOrNull { it.kind == 0 && it.name == "bond" }
            val paired = bond?.let { s.values[target]?.get(it.id)?.bytes?.any { b -> b != 0.toByte() } } == true
            button(actions, if (target to 0xfe in s.drafts.edits) "Undo Unpair" else "Unpair", paired) {
                val key = target to 0xfe
                if (key in s.drafts.edits) s.drafts.edits.remove(key) else s.drafts.edits[key] = ByteArray(0)
                model.saveDrafts(s); render()
            }
        }
        if (target == 9) {
            parent.addView(label("Leave sticks and triggers untouched for 2.5 seconds, then circle both sticks and fully press both triggers. A/B saves; Home cancels."))
            button(parent, "Calibrate sensors", s.drafts.edits.isEmpty()) { action("Starting calibration", 0x18) }
        }
        if (target >= 10) {
            val files = row(parent)
            button(files, "Export preset…") { document("export-preset", true, "${Protocol.targetNames[target]}.json", "application/json") }
            button(files, "Import preset…") { document("import-preset", false, mime = "application/json") }
        }
        val fields = s.fields.filter { it.kind == Protocol.kind(target) }
        val advanced = column().apply { tag = "developer-fields"; visibility = if (model.developer) View.VISIBLE else View.GONE }
        val mapField = fields.firstOrNull { it.name == "map" && it.kind == 3 }
        val rulesField = fields.firstOrNull { it.name == "rules" && it.kind == 3 }
        fun raw(field: Field) = s.drafts.edits[target to field.id]?.let {
            if (it.isEmpty()) s.defaults.getValue(target).getValue(field.id).bytes else it
        } ?: s.values.getValue(target).getValue(field.id).bytes
        for (field in fields) {
            if (field == rulesField && mapField != null) continue
            if (field == mapField && rulesField != null) {
                parent.addView(RemapEditorView(this, raw(field), raw(rulesField),
                    s.values.getValue(target).getValue(field.id).bytes, s.values.getValue(target).getValue(rulesField.id).bytes,
                    s.defaults.getValue(target).getValue(field.id).bytes, s.defaults.getValue(target).getValue(rulesField.id).bytes,
                    !model.busy) { map, rules ->
                    val edits = listOf(field to map, rulesField to rules).map { (f, bytes) ->
                        val saved = s.values.getValue(target).getValue(f.id)
                        Edit(target, f.id, if (!saved.explicit && bytes.contentEquals(saved.bytes)) ByteArray(0) else bytes)
                    }
                    model.stage(s, edits)
                })
                continue
            }
            val value = s.values[target]?.get(field.id) ?: continue
            val draft = s.drafts.edits[target to field.id]
            val raw = if (draft == null) value.bytes else if (draft.isEmpty()) s.defaults.getValue(target).getValue(field.id).bytes else draft
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL }
            val default = s.defaults[target]?.get(field.id)?.bytes?.let { runCatching { field.show(it) }.getOrDefault(it.hex()) }
            val helpText = field.help + "\n\nDefault: ${default ?: "unavailable"}" + if (field.type == 1 || field.type == 5) "\nRange: 0–${field.max}" else ""
            val help = View.OnLongClickListener {
                AlertDialog.Builder(this).setTitle(field.name.replace('_', ' ')).setMessage(helpText).setPositiveButton("OK", null).show()
                true
            }
            line.setOnLongClickListener(help)
            Button(this).apply {
                text = if (draft != null) "*" else if (value.explicit) "✓" else "—"
                contentDescription = if (draft != null) "Discard ${field.name} edit" else "Default ${field.name}"
                setOnLongClickListener(help)
                isEnabled = !model.busy && (draft != null || value.explicit)
                layoutParams = LinearLayout.LayoutParams(dp(40), dp(48)); minWidth = 0; minimumWidth = 0; setPadding(0, 0, 0, 0)
                setOnClickListener {
                    if (draft != null) s.drafts.edits.remove(target to field.id) else s.drafts.edits[target to field.id] = ByteArray(0)
                    model.saveDrafts(s); render()
                }; line.addView(this)
            }
            line.addView(label(field.name.replace('_', ' ') + ":", 14f).apply {
                tag = "label-${field.name}"; setOnLongClickListener(help)
            }, LinearLayout.LayoutParams(dp(120), -2))
            val text = runCatching { field.show(raw) }.getOrDefault(raw.hex())
            if (field.type == 2 && raw.size == 3) line.addView(View(this).apply {
                setBackgroundColor(Color.rgb(raw.u8(0), raw.u8(1), raw.u8(2)))
                layoutParams = LinearLayout.LayoutParams(dp(24), dp(36)).apply { setMargins(0, dp(8), dp(4), 0) }
            })
            Button(this).apply {
                this.text = text; tag = "field-${field.name}"; contentDescription = "${field.name.replace('_', ' ')}: $text"
                isAllCaps = false; gravity = android.view.Gravity.START or android.view.Gravity.CENTER_VERTICAL
                isEnabled = !model.busy; layoutParams = LinearLayout.LayoutParams(0, -2, 1f)
                setOnLongClickListener(help)
                setOnClickListener { editField(s, target, field, raw) }; line.addView(this)
            }
            (if (field.developer) advanced else parent).addView(line)
        }
        val developerCount = advanced.childCount
        if (developerCount > 0) {
            parent.addView(CheckBox(this).apply {
                tag = "developer-toggle"; text = "Developer fields ($developerCount)"; isChecked = model.developer; isEnabled = !model.busy
                setOnCheckedChangeListener { _, checked ->
                    model.developer = checked; advanced.visibility = if (checked) View.VISIBLE else View.GONE
                }
            })
            parent.addView(advanced)
        }
    }
    private fun editField(s: PadSession, target: Int, field: Field, raw: ByteArray) {
        val title = field.name.replace('_', ' ')
        if (field.type == 0) {
            AlertDialog.Builder(this).setTitle(title).setSingleChoiceItems(field.names.toTypedArray(), raw.u8(0)) { dialog, i ->
                model.stage(s, listOf(Edit(target, field.id, byteArrayOf(i.toByte())))); dialog.dismiss()
            }.setNegativeButton("Cancel", null).show(); return
        }
        if (field.name.endsWith("curve_points")) { curveDialog(s, target, field, raw); return }
        if (field.kind == 3 && field.name == "map") { mapDialog(s, target, field, raw); return }
        val help = field.help + if (field.name == "rules") "\n\nINPUT TRIGGER[:MS] EFFECT OUT+OUT[@HZ]; …\nExample: r4 held turbo_modifier a+b@15\nTriggers: held, held_for:300, pressed, released, tapped:200, double_tapped:300\nEffects: hold, turbo, nothing; tap, toggle_hold, toggle_turbo, turbo_modifier, toggle_turbo_modifier. Up to 16 rules." else ""
        textDialog(title, help, field.show(raw), { text -> model.stage(s, listOf(Edit(target, field.id, field.parse(text)))) }, field.type == 1 || field.type == 5)
    }
    private fun textDialog(title: String, help: String, initial: String, save: (String) -> Unit, numeric: Boolean = false) {
        val content = column().apply { setPadding(dp(16), 0, dp(16), 0) }
        content.addView(label(help, 14f))
        val editor = EditText(this).apply {
            setText(initial); setSelectAllOnFocus(true)
            inputType = if (numeric) InputType.TYPE_CLASS_NUMBER else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            minLines = 1; maxLines = 8
        }
        content.addView(editor)
        val dialog = AlertDialog.Builder(this).setTitle(title).setView(ScrollView(this).apply { addView(content) })
            .setPositiveButton("Stage", null).setNegativeButton("Cancel", null).create()
        dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            try { save(editor.text.toString()); dialog.dismiss() } catch (e: Exception) { editor.error = e.message }
        } }
        dialog.show()
    }
    private fun mapDialog(s: PadSession, target: Int, field: Field, raw: ByteArray) {
        val bytes = raw.copyOf()
        val content = column()
        val choices = (Remap.keys.filter { it.isNotEmpty() } + "none").toTypedArray()
        for (i in Remap.keys.indices.filter { Remap.keys[it].isNotEmpty() }) {
            val line = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            line.addView(label(Remap.keys[i].uppercase()), LinearLayout.LayoutParams(dp(88), -2))
            line.addView(Spinner(this).apply {
                adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, choices)
                val n = bytes.u8(i); val to = if (n == 255) "none" else if (n in 1..20 && Remap.keys[n - 1].isNotEmpty()) Remap.keys[n - 1] else Remap.keys[i]
                setSelection(choices.indexOf(to))
                onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                    override fun onNothingSelected(parent: AdapterView<*>?) {}
                    override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                        val to = choices[position]; bytes[i] = (if (to == "none") 255 else if (to == Remap.keys[i]) 0 else Remap.keys.indexOf(to) + 1).toByte()
                    }
                }
            }); content.addView(line)
        }
        AlertDialog.Builder(this).setTitle("Key mappings").setView(ScrollView(this).apply { addView(content) })
            .setPositiveButton("Stage") { _, _ -> model.stage(s, listOf(Edit(target, field.id, bytes))) }.setNegativeButton("Cancel", null).show()
    }
    private fun curveDialog(s: PadSession, target: Int, field: Field, raw: ByteArray) {
        val content = column().apply { setPadding(dp(16), 0, dp(16), 0) }
        val editor = EditText(this).apply { setText(field.show(raw)); inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE }
        val graph = CurveView(this, raw.copyOf()).apply { layoutParams = LinearLayout.LayoutParams(-1, dp(220)); changed = { editor.setText(field.show(it)) } }
        content.addView(graph)
        val bar = row(content)
        var linear = false
        for (name in listOf("linear", "concave", "s-curve")) button(bar, name) { linear = name == "linear"; graph.points = Remap.curve(name, field.size); graph.invalidate(); editor.setText(field.show(graph.points)) }
        content.addView(label("Drag a point or enter ${field.size} nondecreasing values (0–255).", 14f)); content.addView(editor)
        val dialog = AlertDialog.Builder(this).setTitle(field.name.replace('_', ' ')).setView(content).setPositiveButton("Stage", null).setNegativeButton("Cancel", null).create()
        dialog.setOnShowListener { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            try {
                val bytes = field.parse(editor.text.toString())
                val enabled = s.fields.first { it.kind == 3 && it.name == field.name.replace("points", "enabled") }
                val edits = listOf(Edit(target, field.id, bytes), Edit(target, enabled.id, byteArrayOf(if (linear && bytes.contentEquals(Remap.curve("linear", field.size))) 0 else 1)))
                model.stage(s, edits); dialog.dismiss()
            } catch (e: Exception) { editor.error = e.message }
        } }; dialog.show()
    }
    private fun action(label: String, command: Int, arg: Int = 0, length: Int = 0) {
        model.run(label) { model.withPad { pad, s -> pad.command(command, arg.toLong(), length); if (command !in listOf(0x10, 0x11)) s.status = AppModel.status(pad.command(1, length = 16)) } }
    }
    private fun livePage(parent: LinearLayout) {
        button(parent, if (model.live == null) "Start" else "Pause") { model.live = if (model.live == null) (if (model.page == -3) "console" else "input") else null; render() }
        button(parent, "Clear") { model.console = ""; render() }
        liveText = label(model.console, 14f).apply { typeface = android.graphics.Typeface.MONOSPACE; setTextIsSelectable(true) }
        parent.addView(liveText)
    }
    private fun rumbleDialog() {
        val content = column(); val values = IntArray(2)
        for ((i, name) in listOf("Left", "Right").withIndex()) {
            val text = label("$name: 0%"); content.addView(text)
            content.addView(SeekBar(this).apply { max = 100; setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onStartTrackingTouch(s: SeekBar?) {}; override fun onStopTrackingTouch(s: SeekBar?) {}
                override fun onProgressChanged(s: SeekBar?, progress: Int, fromUser: Boolean) { values[i] = progress; text.text = "$name: $progress%" }
            }) })
        }
        AlertDialog.Builder(this).setTitle("Rumble test · 1 second").setView(content).setPositiveButton("Run") { _, _ ->
            val arg = values[0] * 255 / 100 or ((values[1] * 255 / 100) shl 8) or (1000 shl 16)
            action("Testing rumble", 0x1a, arg)
        }.setNegativeButton("Cancel", null).show()
    }
    private fun loaderPage(parent: LinearLayout) {
        button(parent, "Select RAM helper…") { document("loader", false) }
        button(parent, "Read product ID", model.loaderFile.isFile) { model.run("Inspecting loader") { loader { model.loaderPid = runCatching { it.pid() }.getOrDefault("unavailable") } } }
        button(parent, "Back up full flash…", model.loaderFile.isFile) { document("backup-flash", true, "cyclone-flash.bin") }
        button(parent, "Flash firmware…", model.loaderFile.isFile) { document("flash", false) }
        if (model.pendingImage != null) button(parent, "Continue checked firmware…") { flashConfirmation() }
        parent.addView(label("Flashing accepts a prepared firmware.img or UFW. Product, capacity and chip key are checked before erasing, and written bytes are verified. Settings in erased sectors are replaced by the image.\n\nAfter a backup, reconnect USB to leave loader mode."))
    }
    private fun <T> loader(block: (Br23Loader) -> T): T = model.transport { tr ->
        Br23Loader(tr, model::progress).let { it.start(model.loaderFile.readBytes()); block(it) }
    }
    private fun document(action: String, create: Boolean, name: String = "", mime: String = "application/octet-stream") {
        model.documentAction = action; model.documentTarget = model.page
        model.documentIdentity = model.session?.identity?.flashId ?: model.selected?.deviceName
        val intent = Intent(if (create) Intent.ACTION_CREATE_DOCUMENT else Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE); type = mime
            if (create) putExtra(Intent.EXTRA_TITLE, name)
        }
        startActivityForResult(intent, 100)
    }
    private fun readFile(uri: Uri, limit: Int): ByteArray = contentResolver.openInputStream(uri)?.use { stream ->
        val result = ByteArrayOutputStream(); val buffer = ByteArray(8192)
        while (true) {
            val n = stream.read(buffer); if (n == -1) break
            require(result.size() + n <= limit) { "Selected file exceeds $limit bytes" }; result.write(buffer, 0, n)
        }; result.toByteArray()
    } ?: error("Cannot open file")
    private fun writeFile(uri: Uri, bytes: ByteArray) {
        contentResolver.openOutputStream(uri, "wt")?.use { it.write(bytes); it.flush() } ?: error("Cannot write backup")
    }
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 100) return
        if (resultCode != RESULT_OK || data?.data == null) { model.pendingImage = null; return }
        val uri = data.data!!; val action = model.documentAction; val target = model.documentTarget
        val expected = model.documentIdentity
        if (expected != (model.session?.identity?.flashId ?: model.selected?.deviceName)) { model.message = "Selected device changed; choose the file again"; render(); return }
        when (action) {
            "loader" -> model.run("Saving RAM helper") { val bytes = readFile(uri, 0x20000); require(bytes.isNotEmpty()); model.loaderFile.writeBytes(bytes) }
            "backup-settings" -> model.run("Backing up saved settings") { model.withPad { pad, s ->
                val values = Protocol.targets(s.fields).associateWith { pad.read(it) }
                writeFile(uri, SettingsFiles.backup(s.identity, s.fields, values).toByteArray())
            } }
            "restore-settings", "import-preset" -> model.run("Staging imported settings") {
                val s = model.session ?: error("No pad selected"); val text = readFile(uri, 1024 * 1024).toString(Charsets.UTF_8)
                val edits = if (action == "restore-settings") SettingsFiles.restore(text, s.fields, s.values) else SettingsFiles.importPreset(text, s.fields, target)
                handler.post { if (model.session === s) {
                    s.drafts.stageImport(s.fields, s.values, edits, action == "restore-settings")
                    model.saveDrafts(s)
                } }
            }
            "export-preset" -> model.run("Exporting remap preset") {
                val s = model.session ?: error("No pad selected")
                fun value(name: String): ByteArray {
                    val f = s.fields.first { it.kind == 3 && it.name == name }
                    return s.drafts.edits[target to f.id]?.let { if (it.isEmpty()) s.defaults.getValue(target).getValue(f.id).bytes else it } ?: s.values.getValue(target).getValue(f.id).bytes
                }
                writeFile(uri, SettingsFiles.exportPreset(value("name").cString(), value("map"), value("rules")).toByteArray())
            }
            "backup-flash" -> model.run("Backing up full flash") { loader { writeFile(uri, it.read(0, it.flashSize, "Backing up flash")) } }
            "flash" -> model.run("Checking firmware image") {
                val image = FirmwareImage.unwrap(readFile(uri, 16 * 1024 * 1024))
                loader { it.prepare(image) } // Complete validation before the user sees the final flash action.
                model.pendingImage = image
                model.flashDeviceId = model.selected?.deviceId
                handler.post { flashConfirmation() }
            }
            "preflash-backup" -> performFlash(uri)
        }
    }
    private fun flashConfirmation() {
        if (isFinishing || isDestroyed) return
        val image = model.pendingImage ?: return
        val pid = FirmwareImage.identity(image).first
        AlertDialog.Builder(this).setTitle("Flash $pid?")
            .setMessage("${image.size} bytes. Save a full pre-flash backup? The app verifies the written bytes. Keep USB connected until finished.")
            .setPositiveButton("Back up, then flash") { _, _ -> document("preflash-backup", true, "cyclone-before-flash.bin") }
            .setNeutralButton("Flash without backup") { _, _ -> performFlash(null) }
            .setNegativeButton("Cancel") { _, _ -> model.pendingImage = null }.show()
    }
    private fun performFlash(backup: Uri?) {
        val image = model.pendingImage ?: return
        model.pendingImage = null
        model.run("Flashing firmware") {
            require(model.selected?.deviceId == model.flashDeviceId) { "Selected loader changed; choose the firmware again" }
            loader { loader ->
                val prepared = loader.prepare(image)
                if (backup != null) writeFile(backup, loader.read(0, loader.flashSize, "Backing up flash"))
                loader.flash(prepared)
            }
        }
    }
    private fun confirm(title: String, text: String, action: () -> Unit) {
        AlertDialog.Builder(this).setTitle(title).setMessage(text).setPositiveButton("Continue") { _, _ -> action() }.setNegativeButton("Cancel", null).show()
    }
    override fun onBackPressed() {
        if (model.busy) { model.message = "Wait for the USB operation to finish"; updateProgress(); return }
        if (model.page != -2 && model.hasDevice) { back(); return }
        if (model.session?.drafts?.edits?.isNotEmpty() == true) confirm("Leave padtool?", "Pending edits are kept on this phone and restored when this firmware is read again.") { finish() }
        else super.onBackPressed()
    }
}
