// The page: connect to a pad in config mode (Home+M held for 3 s), show what
// it is and its settings, and change them.
import { links } from "./link-web.js";
import { Pad, TARGETS, T, PALETTE, PALETTE_TARGET, REMAP_TARGET, BASE_TARGET, SLOT_NAMES, kindOf, targetsFor, showValue,
  parseValue, editable, slotLabel, remapPart } from "./pad.js";
import { $, h } from "./dom.js";
import { remapEditor, curveEditor } from "./editors.js";
import { writeTests, updaterTrip } from "./linktest.js";
import { openPackage, findFile, checkUpdateImage } from "./package.js";
import { intoUpdater, sendImage, finish } from "./update.js";

const STATUS_EVERY_MS = 5000;    // also keeps config mode open (it ends a minute after the last command)

let pad = null, poll = null, image = null, layout = null;

// A target as people read it: "Slot B (south)", "Remap base", "Palette".
function targetName(t) {
  if (t < 8) return `Slot ${slotLabel(layout, t)}`;
  if (t === BASE_TARGET) return "Remap base";
  if (t >= REMAP_TARGET) return `Remap ${slotLabel(layout, t - REMAP_TARGET)}`;
  return TARGETS[t][0].toUpperCase() + TARGETS[t].slice(1);
}

function say(message, error = false) {
  $("message").textContent = message;
  $("message").classList.toggle("error", error);
}

function showIdentity(id, link) {
  $("identity").replaceChildren(h("dl", {},
    h("dt", {}, "Board"), h("dd", {}, id.boardName || "?"),
    h("dt", {}, "Mode"), h("dd", {}, id.updater ? "updater" : "config mode"),
    h("dt", {}, "Product"), h("dd", {}, id.product),
    h("dt", {}, "Build"), h("dd", {}, id.build),
    h("dt", {}, "Flash ID"), h("dd", { class: "mono" }, id.flashId),
    h("dt", {}, "Protocol"), h("dd", {}, String(id.protocol)),
    h("dt", {}, "Bluetooth"), h("dd", {}, link.name)));
}

function showStatus(s) {
  const battery = [s.percent !== null ? `${s.percent}%` : null, s.mv !== null ? `${s.mv} mV` : null,
    s.charging, s.low ? "low" : null].filter(Boolean).join(", ");
  $("status").textContent = [`state ${s.state}`, s.slot ? `slot ${slotLabel(layout, SLOT_NAMES.indexOf(s.slot))}` : null,
    battery ? `battery ${battery}` : null, `up ${s.uptime.toFixed(0)} s`,
    s.ephemeral ? "ephemeral" : null].filter(Boolean).join(" · ");
}

// ---- settings ----

// What the pad has: the field table, per target its values and defaults;
// edits: key "target:id" -> {target, id, f, value} or {error}, value empty
// for the default.
let table = null, values = new Map(), defaults = new Map(), edits = new Map();
const keyOf = (t, f) => `${t}:${f.id}`;

function paletteColour(i) {
  const f = table.find((x) => x.kind === 1 && x.id === i + 1);
  const v = f && values.get(PALETTE_TARGET)?.get(f.name);
  return v ? showValue(f, v.value) : "transparent";
}

// ---- how settings read: padgui's names ----

// A remap slot's stick and trigger settings go in a box each.
const SIDES = { left: "Left stick", right: "Right stick", l2: "L2 trigger", r2: "R2 trigger" };
const BOXED = { maps_to: "Maps to", x_axis: "Invert X axis", y_axis: "Invert Y axis", press: "Digital at",
  mode: "Analog mode", curve_points: "Curve" };
const sentence = (s) => s.replace(/_/g, " ").replace(/^./, (c) => c.toUpperCase());

function sideOf(t, f) {
  if (t < REMAP_TARGET || f.dev) return null;
  const side = f.name.split("_")[0];
  return SIDES[side] && f.name !== side ? side : null;
}

// {label, side, checkbox, nice (enum choices capitalised), unit} for a field.
function look(t, f) {
  const side = sideOf(t, f), rest = side ? f.name.slice(side.length + 1) : f.name;
  return {
    side, label: (side && BOXED[rest]) || sentence(rest),
    checkbox: !!side && (rest === "x_axis" || rest === "y_axis") && f.type === T.ENUM && f.names.length === 2,
    nice: !!side && f.type === T.ENUM, unit: rest === "press" ? " %" : "",
  };
}

function editor(t, f, shown, how) {
  let e;
  if (f.type === T.ENUM) {
    e = h("select", {}, f.names.filter(Boolean).map((n) => h("option", { value: n }, how.nice ? sentence(n) : n)));
  } else if (f.type === T.ENTRY) {
    e = h("select", {}, Array.from({ length: PALETTE }, (_, i) => h("option", { value: String(i) }, String(i))));
  } else if (f.type === T.RGB) {
    e = h("input", { type: "color" });
  } else if (f.type === T.NUMBER) {
    e = h("input", { type: "number", min: 0, max: f.max || null, step: 1, inputmode: "numeric" });
  } else {
    e = h("input", { type: "text", spellcheck: "false", autocomplete: "off" });
  }
  e.value = shown;
  e.setAttribute("aria-label", `${targetName(t)} ${how.side ? SIDES[how.side] + " " : ""}${how.label}`);
  if (how.checkbox) {           // normal/inverted as a tick; the select stays the value
    const box = h("input", { type: "checkbox", "aria-label": e.getAttribute("aria-label") });
    const show = () => { box.checked = e.value === f.names[1]; };
    box.addEventListener("change", () => { e.value = f.names[box.checked ? 1 : 0]; e.dispatchEvent(new Event("change")); });
    e.addEventListener("change", show);
    e.hidden = true;
    show();
    return h("span", {}, e, box);
  }
  if (f.type === T.NUMBER) return h("span", { class: "number" }, e, h("span", { class: "muted" }, ` 0–${f.max || 2 ** (8 * Math.min(f.size, 4)) - 1}${how.unit}`));
  if (f.type !== T.ENTRY) return e;
  const swatch = h("span", { class: "swatch" });
  const paint = () => { swatch.style.background = paletteColour(Number(e.value)); };
  e.addEventListener("change", paint);
  paint();
  return h("span", { class: "entry" }, e, swatch);
}

// A setting's row.  rows: the target's rows by field name, for the editors
// that span fields (a curve's points and its on/off).
function row(t, f, rows) {
  const v = values.get(t).get(f.name), d = defaults.get(t)?.get(f.name);
  const shown = showValue(f, v.value), def = d ? showValue(f, d.value) : null;
  const key = keyOf(t, f), part = remapPart(f);
  // The map and rules have the keys editor (their text is for developers);
  // a curve's on/off follows its points.
  const hidden = part === "map" || part === "rules" ? "dev" : f.kind === 3 && f.name.endsWith("_curve_enabled") ? "merged" : null;
  const tr = h("tr", { class: [f.dev ? "dev" : null, hidden].filter(Boolean).join(" ") || null, "data-key": key });
  const mark = h("td", { class: "mark", title: v.set ? "saved on the pad" : "firmware default" }, v.set ? "✓" : "");
  const how = look(t, f);
  let cell, input = null;
  if (editable(f)) {
    cell = editor(t, f, shown, how);
    input = cell.matches("input, select") ? cell : cell.querySelector("input, select");
    if (part === "map" || part === "rules") input.classList.add("wide");
    if (part === "curve") {
      input.classList.add("wide");
      cell = curveEditor(input, f.size, () => rows.get(f.name.replace("_points", "_enabled"))?.input);
    }
    const changed = () => {
      tr.classList.remove("reset");
      edit(t, f, input.value, shown, tr);
    };
    input.addEventListener(input.tagName === "SELECT" || input.type === "color" ? "change" : "input", changed);
  } else {
    cell = h("span", { class: "mono" }, shown);
  }
  const shownDef = def !== null && how.nice ? sentence(def) : def;
  const reset = h("button", { type: "button", class: "small", title: `back to the firmware default${def !== null ? ` (${shownDef})` : ""}`,
    hidden: !v.set }, "default");
  reset.addEventListener("click", () => {
    if (!v.set) {               // at its default already: just drop the edit
      if (input) input.value = shown, input.dispatchEvent(new Event("change"));
      edits.delete(key);
      tr.classList.remove("changed", "invalid", "reset");
      if (part === "curve") rows.get(f.name.replace("_points", "_enabled"))?.reset.click();
      return bar();
    }
    if (input && def !== null) input.value = def, input.dispatchEvent(new Event("change"));
    if (part === "curve") rows.get(f.name.replace("_points", "_enabled"))?.reset.click();   // they go together
    edits.set(key, { target: t, id: f.id, f, value: new Uint8Array(0) });
    tr.classList.remove("invalid");
    tr.classList.add("changed", "reset");
    tr.title = "";
    bar();
  });
  tr.append(...[h("th", { scope: "row", title: [f.name, f.help].filter(Boolean).join(": ") }, how.label),
    // The curve takes the default's column too (its default is a preset).
    h("td", { colspan: part === "curve" ? 2 : null }, cell),
    part === "curve" ? null : h("td", { class: "muted" }, v.set && def !== null && shown !== def ? shownDef : ""),
    mark, h("td", {}, reset)].filter(Boolean));
  Object.assign(tr, { input, reset });
  return tr;
}

function edit(t, f, text, shown, tr) {
  const key = keyOf(t, f);
  tr.classList.remove("invalid");
  tr.title = "";
  if (text === shown) {
    edits.delete(key);
    tr.classList.remove("changed");
  } else {
    try {
      edits.set(key, { target: t, id: f.id, f, value: parseValue(f, text) });
      tr.classList.add("changed");
    } catch (error) {
      edits.set(key, { error: error.message });
      tr.classList.add("invalid");
      tr.title = error.message;
    }
  }
  bar();
}

function targetSection(t, open) {
  const vals = values.get(t), byName = new Map();
  const fields = table.filter((f) => f.kind === kindOf(t) && vals.has(f.name));
  const rows = fields.map((f) => {
    const tr = row(t, f, byName);
    byName.set(f.name, tr);
    return tr;
  });
  const map = fields.find((f) => remapPart(f) === "map"), rules = fields.find((f) => remapPart(f) === "rules");
  let keys = null;
  if (map && rules && byName.get("map").input && byName.get("rules").input) {
    const raw = (m) => m?.has("map") && m.has("rules") ? { map: m.get("map").value, rules: m.get("rules").value } : null;
    keys = remapEditor({
      map: byName.get("map").input, rules: byName.get("rules").input, name: byName.get("name")?.input,
      rulesField: rules, saved: raw(vals), defaults: raw(defaults.get(t)), layout, file: TARGETS[t],
      resetAll: () => { byName.get("map").reset.click(); byName.get("rules").reset.click(); },
    });
  }
  // The rest, then a box per stick and trigger (padgui's grouping).
  const general = [], boxes = new Map();
  fields.forEach((f, i) => {
    const side = sideOf(t, f);
    if (!side) return general.push(rows[i]);
    if (!boxes.has(side)) boxes.set(side, []);
    boxes.get(side).push(rows[i]);
  });
  const settings = (trs) => h("table", { class: "settings" }, h("colgroup", {}, h("col", { class: "c-name" }), h("col"),
    h("col", { class: "c-def" }), h("col", { class: "c-mark" }), h("col", { class: "c-reset" })), h("tbody", {}, trs));
  return h("details", { open, "data-target": t },
    h("summary", {}, targetName(t), h("span", { class: "muted mono" }, ` ${TARGETS[t]}`)),
    rows.length ? settings(general) : h("p", { class: "muted" }, "Nothing here."),
    keys,
    [...boxes].map(([side, trs]) => h("section", { class: "box" }, h("h3", {}, SIDES[side]), settings(trs))));
}

function render() {
  const open = new Set([...document.querySelectorAll("#settings details[open]")].map((d) => Number(d.dataset.target)));
  edits.clear();
  $("settings").replaceChildren(...[...values.keys()].map((t) => targetSection(t, open.has(t))));
  bar();
}

function bar() {
  const n = edits.size, bad = [...edits.values()].filter((e) => e.error);
  $("savebar").hidden = !n;
  document.body.classList.toggle("unsaved", !!n);
  $("savecount").textContent = bad.length ? bad[0].error : `${n} unsaved change${n === 1 ? "" : "s"}`;
  $("savecount").classList.toggle("error", !!bad.length);
  $("save").disabled = !!bad.length;
}

async function readTarget(t) {
  values.set(t, await pad.read(table, t));
  defaults.set(t, await pad.read(table, t, true));
}

async function save() {
  const items = [...edits.values()];
  const renew = items.filter((e) => e.f.kind === 0 && e.f.name === "type");
  if (renew.length && !confirm(`Changing a slot's type renews its address and clears its pairing (${renew.map((e) => TARGETS[e.target]).join(", ")}). Save?`)) return;
  $("save").disabled = $("undo").disabled = true;
  try {
    say("Saving…");
    await pad.write(items);
    for (const t of new Set(items.map((e) => e.target))) await readTarget(t);
    render();
    const s = await pad.status();
    showStatus(s);
    say(`Saved${s.ephemeral ? " (ephemeral: until the next boot)" : ""}; it applies on the next connect.`);
  } catch (error) {
    say(String(error.message || error), true);
    for (const r of error.refused || []) {
      const tr = document.querySelector(`#settings tr[data-key="${r.target}:${r.id}"]`);
      tr?.classList.add("invalid");
      if (tr) tr.title = `the pad: ${r.why}`;
    }
  } finally {
    $("save").disabled = false;
    $("undo").disabled = false;
    bar();
  }
}

async function connect() {
  disconnect();
  say("Choose your pad (hold Home+M for 3 s to put it in config mode).");
  let link;
  try {
    link = await links.choose();
  } catch (error) {
    say(error.name === "NotFoundError" ? "No pad chosen." : String(error.message || error), error.name !== "NotFoundError");
    return;
  }
  await attach(new Pad(link));
}

// A pad on a new link (from the chooser, or found again after an update):
// what it is, its status and settings.
async function attach(p) {
  pad = p;
  pad.onDisconnect = () => { stop(); say("The pad went away (config mode ends a minute after the last command, or on Home)."); };
  $("connect").hidden = true;
  $("disconnect").hidden = false;
  for (const b of document.querySelectorAll("#tests .needs-pad")) b.hidden = false;
  try {
    say("Asking the pad what it is…");
    const id = pad.id || await pad.identify();
    showIdentity(id, pad.link);
    $("firmware").hidden = false;
    pickImage();
    if (id.updater) {
      say("The pad is in its updater.");
      return;
    }
    layout = await pad.layout().catch(() => null);
    showStatus(await pad.status());
    poll = setInterval(async () => {
      try { showStatus(await pad.status()); } catch { /* the disconnect handler says it */ }
    }, STATUS_EVERY_MS);
    say("Reading its settings…");
    table = await pad.fields();
    values = new Map();
    defaults = new Map();
    for (const t of targetsFor(table)) await readTarget(t);
    render();
    say(`${table.length} settings in ${values.size} groups. Changes apply on the next connect.`);
    $("tests").hidden = false;
  } catch (error) {
    say(String(error.message || error), true);
  }
}

function stop() {
  clearInterval(poll);
  poll = null;
  $("connect").hidden = false;
  $("disconnect").hidden = true;
}

function disconnect() {
  stop();
  if (pad) {
    pad.onDisconnect = null;
    pad.close();
    pad = null;
  }
  $("identity").replaceChildren();
  $("status").textContent = "";
  $("settings").replaceChildren();
  edits.clear();
  bar();
  $("tests").hidden = true;
  $("firmware").hidden = true;
}

// ---- firmware update ----

let pkg = null;

function fwSay(text, error = false) {
  $("fwinfo").textContent = text;
  $("fwinfo").classList.toggle("error", error);
}

// The connected pad's update.bin from the chosen package.
async function pickImage() {
  image = null;
  $("update").disabled = true;
  if (!pkg || !pad?.id) return fwSay(pkg ? "" : "Choose a firmware package (firmware.zip).");
  const file = findFile(pkg, pad.id.product, "update");
  if (!file) return fwSay(`The package has no update for this pad (${pad.id.product}).`, true);
  try {
    const data = await pkg.read(file);
    const wrong = checkUpdateImage(data, pad.id.product);
    if (wrong) return fwSay(wrong, true);
    image = data;
    fwSay(`${file.path}: ${data.length} bytes, SHA-256 ${file.sha256.slice(0, 12)}…`);
    $("update").disabled = false;
  } catch (error) {
    fwSay(String(error.message || error), true);
  }
}

function fwLog(line) {
  $("fwlog").textContent += line + "\n";
}

async function update() {
  if (!image || !confirm(`Write this firmware to ${pad.id.boardName}?`)) return;
  const p = pad, data = image;
  p.onDisconnect = null;         // the update restarts the pad
  stop();
  pad = null;
  $("connect").hidden = true;
  $("update").disabled = true;
  $("fwlog").textContent = "";
  $("fwprogress").hidden = false;
  $("fwprogress").value = 0;
  say("Updating: keep this page open and the pad on.");
  let up = null, written = false;
  try {
    up = await intoUpdater(p, links, fwLog);
    await sendImage(up, data, fwLog, (done, total) => { $("fwprogress").value = done / total; });
    written = true;
    const fresh = await finish(up, links, fwLog);
    $("fwprogress").hidden = true;
    $("settings").replaceChildren();
    await attach(fresh);
  } catch (error) {
    fwLog(`${written ? "not found again" : "failed"}: ${error.message || error}`);
    $("fwprogress").hidden = true;
    if (written) say("The new firmware is written and started, but did not come back in config mode " +
      "(an older updater does not ask for it): hold Home+M for 3 s, then Connect.");
    else say("The update failed. The pad stays in its updater: Connect to it (it advertises as \"… Update\") and update again, or press Home to go back to its app if that is still whole.", true);
    up?.close();
    $("connect").hidden = false;
  }
}

function testLog(line) {
  $("testlog").textContent += line + "\n";
}

async function runTest(test) {
  for (const b of document.querySelectorAll("#tests button")) b.disabled = true;
  try {
    await test();
  } catch (error) {
    testLog(`error: ${error.message || error}`);
  } finally {
    for (const b of document.querySelectorAll("#tests button")) b.disabled = false;
  }
}

$("connect").addEventListener("click", connect);
$("disconnect").addEventListener("click", () => { disconnect(); say("Disconnected."); });
$("writetest").addEventListener("click", () => runTest(async () => {
  testLog(`-- write sizes, ${new Date().toLocaleTimeString()}`);
  await writeTests(pad, testLog);
}));
$("updatertest").addEventListener("click", () => runTest(async () => {
  testLog(`-- updater round trip, ${new Date().toLocaleTimeString()}`);
  const p = pad;
  p.onDisconnect = null;         // expected: the trip ends the link
  stop();
  pad = null;
  await updaterTrip(p, links, testLog);
  disconnect();
  $("tests").hidden = false;
  for (const b of document.querySelectorAll("#tests .needs-pad")) b.hidden = true;
  say("The updater round trip ended the link; Connect again (Home+M) for more.");
}));
$("package").addEventListener("change", async (e) => {
  pkg = null;
  const f = e.target.files[0];
  if (!f) return pickImage();
  try {
    pkg = await openPackage(new Uint8Array(await f.arrayBuffer()));
  } catch (error) {
    return fwSay(`${f.name}: ${error.message || error}`, true);
  }
  pickImage();
});
$("update").addEventListener("click", update);
$("save").addEventListener("click", save);
$("undo").addEventListener("click", render);
$("dev").addEventListener("change", (e) => document.body.classList.toggle("show-dev", e.target.checked));
if (!links.available()) {
  $("connect").disabled = true;
  say("This browser has no Web Bluetooth: use Chrome or Edge.", true);
}
