import { ask, choose, tell } from "./dialogs.js";
import { text as msg, fieldText } from "./i18n.js";
// The page: connect to a pad over Bluetooth in config mode (Home+M held for
// 3 s) or over USB (its tool device), show what it is and its settings, and
// change them.  Screens: home (Load Package and its contents; how to
// connect, or in the Android app the devices, flash.js), package (what the
// package has), device (a pad: what it is, and Configure, Install, Upgrade,
// Backup, Restore), config (its settings) and action (one of its actions).
// Back (and the browser's or phone's back) goes back a screen.  In a
// browser the device page holds the link (its settings stay staged going
// back and forth); in the app it is flash.js's, and Configure links.
import { bluetooth, usb, android, mock } from "./environment.js";
import { setupPads, setIcon } from "./flash.js";
import { Pad, TARGETS, T, PALETTE, PALETTE_TARGET, REMAP_TARGET, BASE_TARGET, MACROS_TARGET, SLOT_NAMES, kindOf, targetsFor,
  showValue, parseValue, editable, slotLabel, remapPart, macrosOf, remapOf } from "./pad.js";
import { $, h, nameMenu } from "./dom.js";
import { remapEditor, curveEditor, macroEditor } from "./editors.js";
import { decodeMacros, parseMacros } from "./remap.js";
import { writeTests, updaterTrip } from "./linktest.js";
import { openPackage, findFile, checkUpdateImage } from "./package.js";
import { intoUpdater, sendImage, finish } from "./update.js";

const STATUS_EVERY_MS = 5000;    // also keeps config mode open (it ends a minute after the last command)
// Developer extras (the link tests): the page opened with ?dev.
const DEV = new URLSearchParams(location.search).has("dev");

let quickMacroReady = false;
let pad = null, poll = null, image = null, layout = null;

// In the Android app (a WebView, without Web Bluetooth or WebHID) the app
// supplies the links, and a flasher for the boot loaders: there home
// watches USB and takes a pad where it belongs (flash.js).
const pads = android ? setupPads(android.flasher, android.bluetooth, {
  openSettings: (device) => connect(usb, device),
  openBle: (address) => connect(bluetooth, address),
  enterDevice: () => enter("device"),
  enterAction: () => enter("action"),
  upgradeBle,
}) : null;

// The links a link is one of: to find the same pad again after a restart.
const linksOf = (link) => (link.kind === "hid" ? usb : bluetooth);

// ---- screens ----

let screen = "home", updating = false, leaving = false;

const SCREENS = ["home", "package", "device", "config", "action"];
let depth = 0;                   // screens above home in the history

function show(name) {
  for (const s of SCREENS) $(s).hidden = s !== name;
  $("back").hidden = name === "home";
  document.querySelector("header h1").hidden = name !== "home";   // room for Back, Discard and Save
  screen = name;
  bar();
  if (name === "home") pads?.home();
  if (name === "device") pads ? pads.device() : showDevice();
  if (name === "package") showContents();
}

// A screen: one history entry each, so back goes back a screen.
function enter(name) {
  history.pushState({ screen: name, depth: ++depth }, "");
  show(name);
}

// Back a screen (Back), as the back key does.
function back() {
  if (screen !== "home") history.back();
}

// A pad gone: back to the device page in the app (its list goes on), home
// in a browser (the link was the device page's); nothing left to ask about.
function home(force = false) {
  if (screen === "home") return;
  leaving = force;
  if (pads && screen === "config") history.back();
  else history.go(-depth);
}

// Leaving a screen for another: what it has running stops, unless it must
// not.  In a browser the link lives from the device page down.
function leave(to) {
  const force = leaving;
  leaving = false;
  if (updating) return tell(msg("ui.app.wait_for_the_update_to_finish")), false;
  if (pads && (screen === "device" || screen === "action")) return pads.leave() || (tell(msg("ui.app.wait_for_it_to_finish")), false);
  if (!pad && !edits.size) return true;
  if (!pads && screen === "package") return true;
  if (!pads && SCREENS.indexOf(to) >= SCREENS.indexOf("device")) {   // still this pad's; the edits go
    if (edits.size) render();
    return true;
  }
  disconnect();
  return true;
}

addEventListener("popstate", (e) => {
  const to = e.state?.screen ?? "home";
  if (to === screen) return;
  if (!leaving && !updating && edits.size) {          // stay until asked
    history.pushState({ screen, depth }, "");
    if (!asking) unsaved(e.state?.depth ?? 0);
    return;
  }
  if (!leave(to)) return history.pushState({ screen, depth }, "");
  depth = e.state?.depth ?? 0;
  show(to);
});

// Back with unsaved changes: save them, drop them, or stay (as padtool asks).
let asking = false;
async function unsaved(target) {
  asking = true;
  const choice = await choose(msg("ui.page.unsaved_changes"),
    [["save", msg("ui.page.save")], ["discard", msg("ui.page.discard")], ["stay", msg("ui.page.stay")]]).finally(() => { asking = false; });
  if (choice === "save") await save();
  else if (choice !== "discard") return;
  if (choice === "save" && edits.size) return;        // refused or failed: still here
  leaving = true;
  history.go(target - depth);
}

// A target as people read it: "Slot B (south)", "Remap base", "Palette".
function targetName(t) {
  if (t >= MACROS_TARGET) return `${targetName(remapOf(t))} macros`;
  if (t < 8) return `Slot ${slotLabel(layout, t)}`;
  if (t === BASE_TARGET) return msg("ui.app.remap_base");
  if (t >= REMAP_TARGET) return `Remap ${slotLabel(layout, t - REMAP_TARGET)}`;
  return TARGETS[t][0].toUpperCase() + TARGETS[t].slice(1);
}

// What happened, out of the way: something that failed or blocks a dialog
// (problem), a short note after something is done a toast at the bottom
// that fades (toast).  Progress shows where it happens.
const problem = (message) => mock ? mock.notice(message) : tell(message);
let toastTimer = null;
function toast(message) {
  $("toast").textContent = message;
  $("toast").hidden = !message;
  clearTimeout(toastTimer);
  if (mock?.review) return;
  toastTimer = setTimeout(() => { $("toast").hidden = true; }, 4000);
}

const spinner = () => h("span", { class: "spinner", "aria-hidden": "true" });

// What the pad is; id null: the box in place, a spinner where the board goes.
function showIdentity(id, link) {
  const hid = link.kind === "hid";
  const rows = [[msg("ui.app.board"), id ? id.boardName || "?" : spinner()],
    [msg("ui.app.mode"), id && (id.updater ? msg("ui.app.updater") : hid ? msg("ui.app.app") : msg("ui.app.config_mode"))],
    [msg("ui.app.product"), id?.product], [msg("ui.app.build"), id?.build], [msg("ui.app.flash_id"), id?.flashId], [msg("ui.app.protocol"), id && String(id.protocol)],
    [hid ? msg("ui.app.usb") : msg("ui.app.bluetooth"), link.name]];
  $("identity").replaceChildren(h("dl", { "aria-busy": id ? null : "true" },
    rows.map(([k, v]) => [h("dt", {}, k), h("dd", { class: k === msg("ui.app.flash_id") ? "mono" : null }, v ?? "")])));
}

function showStatus(s) {
  const battery = [s.percent !== null ? `${s.percent}%` : null, s.mv !== null ? `${s.mv} mV` : null,
    s.charging, s.low ? msg("ui.app.low") : null].filter(Boolean).join(", ");
  $("status").textContent = [`state ${s.state}`, s.slot ? `slot ${slotLabel(layout, SLOT_NAMES.indexOf(s.slot))}` : null,
    battery ? `battery ${battery}` : null, `up ${s.uptime.toFixed(0)} s`,
    s.ephemeral ? msg("ui.app.ephemeral") : null].filter(Boolean).join(" · ");
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
const SIDES = { left: msg("ui.app.left_stick"), right: msg("ui.app.right_stick"), l2: msg("ui.app.l2_trigger"), r2: msg("ui.app.r2_trigger") };
const BOXED = { maps_to: msg("ui.app.maps_to"), x_axis: msg("ui.app.invert_x_axis"), y_axis: msg("ui.app.invert_y_axis"), press: msg("ui.app.digital_at"),
  mode: msg("ui.app.analog_mode"), curve_points: msg("ui.app.curve") };
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
    side, label: fieldText(f, "label", (side && BOXED[rest]) || sentence(rest)),
    checkbox: !!side && (rest === "x_axis" || rest === "y_axis") && f.type === T.ENUM && f.names.length === 2,
    nice: !!side && f.type === T.ENUM, unit: rest === "press" ? " %" : "",
  };
}

function editor(t, f, shown, how) {
  let e;
  if (f.type === T.ENUM) {
    e = h("select", {}, f.names.filter(Boolean).map((n) => h("option", { value: n }, fieldText(f, `options.${n}`, how.nice ? sentence(n) : n))));
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
  // The map and rules are the keys editor's (it writes their text here);
  // a curve's on/off follows its points.
  const merged = part === "map" || part === "rules" || part === "macros" || (f.kind === 3 && f.name.endsWith("_curve_enabled"));
  const tr = h("tr", { class: [merged ? "merged" : null, v.set ? "set" : null].filter(Boolean).join(" ") || null, "data-key": key });
  const how = look(t, f);
  let cell, input = null;
  if (editable(f)) {
    cell = editor(t, f, shown, how);
    input = cell.matches("input, select") ? cell : cell.querySelector("input, select");
    if (part === "map" || part === "rules" || part === "macros") input.classList.add("wide");
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
  const shownDef = f.type === T.ENUM && def !== null
    ? fieldText(f, `options.${def}`, how.nice ? sentence(def) : def) : def;
  const help = fieldText(f, "help", f.help);
  const reset = h("button", { type: "button", class: "small" }, msg("ui.app.set_to_default"));
  reset.addEventListener("click", () => {
    reset.closest("details")?.removeAttribute("open");
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
  // The name opens its menu: help, the default, back to it.
  const menu = nameMenu(how.label, help ? h("p", {}, help) : null,
    h("p", { class: "muted" }, v.set ? msg("ui.app.saved_on_the_pad") : msg("ui.app.the_firmware_s_default")),
    def !== null ? h("p", {}, msg("ui.app.default"), h("strong", {}, shownDef === "" ? "(empty)" : shownDef)) : null,
    editable(f) ? reset : null, h("p", { class: "muted mono" }, f.name));
  // A curve takes the whole row, under its name.
  if (part === "curve") tr.append(h("td", { colspan: 2 }, menu, cell));
  else tr.append(h("th", { scope: "row" }, menu), h("td", {}, cell));
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
  // The remap slot's macros: their own target, edited here under the rules.
  const mt = kindOf(t) === 3 ? macrosOf(t) : null, mvals = mt !== null ? values.get(mt) : null;
  const mfield = mvals?.has("macros") ? table.find((f) => f.kind === 4 && f.name === "macros") : null;
  const mrow = mfield ? row(mt, mfield, new Map()) : null;
  const macroNames = () => {
    try { return decodeMacros(parseMacros(mrow.input.value, 4096)).map((m) => m.name); } catch { return []; }
  };
  let keys = null, macros = null;
  if (map && rules && byName.get("map").input && byName.get("rules").input) {
    const raw = (m) => m?.has("map") && m.has("rules") ? { map: m.get("map").value, rules: m.get("rules").value } : null;
    keys = remapEditor({
      map: byName.get("map").input, rules: byName.get("rules").input, name: byName.get("name")?.input,
      rulesField: rules, saved: raw(vals), defaults: raw(defaults.get(t)), layout, file: TARGETS[t],
      resetAll: () => { byName.get("map").reset.click(); byName.get("rules").reset.click(); },
      macroNames: mrow ? macroNames : null,
    });
  }
  if (mrow?.input) {
    const m = mvals.get("macros");
    macros = macroEditor({
      input: mrow.input, field: mfield, saved: m.value, defaults: defaults.get(mt)?.get("macros")?.value,
      inherited: !m.set, base: t === BASE_TARGET, layout,
      changed: () => keys?.redraw(), reset: () => mrow.reset.click(),
      quickMacro: () => pad.quickMacro(),
      quickMacroAvailable: () => quickMacroReady,
    });
  }
  // The rest, then a box per stick and trigger (padgui's grouping), then
  // the developer fields under a fold.
  const general = [], boxes = new Map(), dev = [];
  fields.forEach((f, i) => {
    const side = sideOf(t, f);
    if (f.dev) return dev.push(rows[i]);
    if (!side) return general.push(rows[i]);
    if (!boxes.has(side)) boxes.set(side, []);
    boxes.get(side).push(rows[i]);
  });
  const settings = (trs) => h("table", { class: "settings" }, h("colgroup", {}, h("col", { class: "c-name" }), h("col")),
    h("tbody", {}, trs));
  return h("details", { open, "data-target": t },
    h("summary", {}, targetName(t), h("span", { class: "muted mono" }, ` ${TARGETS[t]}`)),
    rows.length ? settings(general) : h("p", { class: "muted" }, msg("ui.app.nothing_here")),
    keys,
    macros, mrow ? h("table", { hidden: true }, h("tbody", {}, mrow)) : null,
    [...boxes].map(([side, trs]) => h("section", { class: "box" }, h("h3", {}, SIDES[side]), settings(trs))),
    dev.length ? h("details", { class: "devfold", open: devOpen.has(t) },
      h("summary", {}, msg("ui.app.developer_fields"), h("span", { class: "muted" }, msg("ui.app.for_measuring_and_testing"))), settings(dev)) : null);
}

// Developer folds left open, by target.
const devOpen = new Set();

function render() {
  const open = new Set([...document.querySelectorAll("#settings > details[open]")].map((d) => Number(d.dataset.target)));
  devOpen.clear();
  for (const d of document.querySelectorAll("#settings details.devfold[open]")) devOpen.add(Number(d.closest("[data-target]").dataset.target));
  edits.clear();
  // A target the pad does not keep (the U2CW's last four slots) reads
  // with no values: not shown.
  // Macros go with their remap slot.
  $("settings").replaceChildren(...[...values].filter(([t, v]) => v.size && kindOf(t) !== 4)
    .map(([t]) => targetSection(t, open.has(t))));
  bar();
}

function bar() {
  const n = edits.size, bad = [...edits.values()].filter((e) => e.error);
  $("savebar").hidden = !n || screen !== "config";
  document.body.classList.toggle("unsaved", !!n);
  $("savecount").textContent = bad.length ? bad[0].error : `${n} change${n === 1 ? "" : "s"}`;
  $("savecount").classList.toggle("error", !!bad.length);
  $("save").disabled = !!bad.length;
}

async function readTarget(t) {
  values.set(t, await pad.read(table, t));
  defaults.set(t, await pad.read(table, t, true));
}

async function save() {
  const items = [...edits.values()];
  const renew = items.filter((e) => e.f.kind === 0 && (e.f.name === "radio" || e.f.name === "radio_type"));
  if (renew.length && !ask(`Changing a slot's Bluetooth type, or moving it between Bluetooth and the dongle, renews its address and clears its pairing (${renew.map((e) => TARGETS[e.target]).join(", ")}). Save?`)) return;
  $("save").disabled = $("undo").disabled = true;
  try {
    $("save").textContent = msg("ui.app.saving");
    await pad.write(items);
    // A base's change shows in every remap slot that takes it from there.
    const reread = new Set(items.map((e) => e.target));
    for (const t of [...reread]) {
      if (t === BASE_TARGET || t === macrosOf(BASE_TARGET)) for (const u of values.keys()) if (kindOf(u) === kindOf(t)) reread.add(u);
    }
    for (const t of reread) await readTarget(t);
    render();
    const s = await pad.status();
    showStatus(s);
    toast(`Saved${s.ephemeral ? msg("ui.app.ephemeral_until_the_next_boot") : ""}; it applies on the next connect.`);
  } catch (error) {
    // Which settings the pad refused, by name (nothing was saved).
    const named = (error.refused || []).map((r) => `${targetName(r.target)}, ${look(r.target, r.f).label}: ${r.why}`);
    problem(named.length ? `The pad refused ${named.join("; ")}. Nothing was saved.` : String(error.message || error));
    for (const r of error.refused || []) {
      const tr = document.querySelector(`#settings tr[data-key="${r.target}:${r.id}"]`);
      tr?.classList.add("invalid");
      if (tr) tr.title = `the pad: ${r.why}`;
    }
  } finally {
    $("save").textContent = msg("ui.app.save");
    $("save").disabled = false;
    $("undo").disabled = false;
    bar();
  }
}

// One page talks to a pad at a time: two windows of this page would each
// take the other's answers (one link, one subscription in the browser).
let releaseLock = null;

function takeLock() {
  if (mock || !navigator.locks) return Promise.resolve(true);
  return new Promise((resolve) => {
    navigator.locks.request("jollymaam-pad-link", { ifAvailable: true }, (lock) => {
      resolve(!!lock);
      return lock ? new Promise((r) => { releaseLock = r; }) : undefined;
    });
  });
}

function dropLock() {
  releaseLock?.();
  releaseLock = null;
}

// links: bluetooth or usb.  Over USB several programs share the pad (each
// takes its own answers), so no lock.  device: the one to take (the
// Android app's USB device name), else the user chooses.
// to: the screen to open (the mock's controls go straight to the settings).
async function connect(links, device = null, to = null) {
  if (pads) pads.leave();
  disconnect();
  if (links === bluetooth && !(await takeLock())) {
    return problem(msg("ui.app.the_pad_settings_page_is_already_connected_in_another_tab_or_window_use_that_one") +
      msg("ui.app.or_disconnect_there_first"));
  }
  let link;
  try {
    link = await links.choose(device);
  } catch (error) {
    console.info(`link: choosing failed: ${error.name}: ${error.message}${error.step ? ` (at ${error.step})` : ""}`);
    if (error.name !== "NotFoundError") problem(String(error.message || error));
    dropLock();
    if (screen === "device") pads?.device();   // watching USB again
    return;
  }
  // The app's device page was flash.js's: its settings next.  A browser's
  // is this link's.
  enter(pads ? "config" : "device");
  if (to === "config" && screen !== "config") enter("config");
  showIdentity(null, link);
  showDevice();
  // Right after a disconnect a new link can come up and then fail its
  // first write ("GATT Error Unknown", Chrome on Linux) while BlueZ is
  // still ending the old one: then a fresh link, for up to 8 s.
  const end = Date.now() + 8000;
  for (let p = new Pad(link); ; ) {
    try {
      await p.identify();
      return attach(p);
    } catch (error) {
      console.info(`link: the first command failed: ${error.name}: ${error.message}` +
        (Date.now() > end ? msg("ui.app.giving_up") : msg("ui.app.a_fresh_link")));
      p.onDisconnect = null;     // not closed: on Linux that ends the link for every program on it
      if (Date.now() > end) {
        home(true);
        return problem(String(error.message || error));
      }
      await new Promise((r) => setTimeout(r, 500));
      try {
        p = new Pad(await links.reconnect(link, Math.max(1, (end - Date.now()) / 1000)));
      } catch (again) {
        home(true);
        return problem(String(again.message || again));
      }
    }
  }
}

// A pad on a new link (from the chooser, or found again after an update):
// what it is, its status and settings.
async function attach(p) {
  pad = p;
  pad.onDisconnect = () => {
    home(true);
    toast(p.usb ? msg("ui.app.the_pad_went_away_unplugged_or_it_restarted")
      : msg("ui.app.the_pad_went_away_config_mode_ends_a_minute_after_the_last_command_or_on_home"));
  };
  for (const b of document.querySelectorAll("#tests .needs-pad")) b.hidden = false;
  try {
    const id = pad.id || await pad.identify();
    showIdentity(id, pad.link);
    await pickImage();
    showDevice();
    if (id.updater) return;      // the Mode row says so; Firmware update is what it takes
    layout = await p.layout().catch(() => null);
    const status = await p.status();
    await refreshQuickMacro(p);
    if (pad !== p) return;
    showStatus(status);
    poll = setInterval(async () => {
      try { showStatus(await p.status()); await refreshQuickMacro(p); } catch { /* the disconnect handler says it */ }
    }, STATUS_EVERY_MS);
    await loadSettings();
    $("tests").hidden = !!pads || !DEV;
  } catch (error) {
    if (pad !== p) return;       // Disconnect: nothing to say
    $("settings").replaceChildren(h("p", { class: "error", role: "alert", id: "settings-error" }, String(error.message || error)));
  }
}

// Where the settings will be, while they are read: one greyed fold, its
// spinner, and how many pages are read once that is known.  Built once,
// so the spinner keeps turning.
async function loadSettings() {
  const progress = h("span", { class: "progress", role: "status" });
  $("settings").replaceChildren(h("div", { class: "pending", "aria-busy": "true" },
    h("span", { class: "tri" }, "▸"), "Settings", spinner(), progress));
  table = await pad.fields();
  if (!table.length) {          // a device without settings (a dongle)
    values = new Map();
    defaults = new Map();
    return $("settings").replaceChildren(h("p", { class: "muted" }, msg("ui.app.this_device_has_no_settings")));
  }
  const targets = targetsFor(table);
  values = new Map();
  defaults = new Map();
  for (const [i, t] of targets.entries()) {
    progress.textContent = `${i}/${targets.length} pages loaded`;
    await readTarget(t);
  }
  render();
}

function stop() {
  clearInterval(poll);
  poll = null;
  dropLock();
}

async function refreshQuickMacro(p) {
  const available = await p.hasQuickMacro().catch(() => false);
  if (pad !== p) return;
  quickMacroReady = available;
  for (const button of document.querySelectorAll(".quick-macro-import")) button.disabled = !available || button.dataset.importing === "true";
}

function disconnect() {
  quickMacroReady = false;
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
  image = null;
  upgradeNote = "";
}

// ---- firmware update ----

let pkg = null;

// A firmware package for both the browser's Upgrade and the flasher:
// the one that comes with the page (firmware.zip beside it: the Android
// app has one), or one loaded from home (Load package) until unloaded.
let bundled = null, loaded = null, pkgName = null, pkgError = "";

async function setPackage(name, bytes) {
  pkg = null;
  pkgName = name;
  pkgError = "";
  try {
    pkg = await openPackage(bytes);
  } catch (error) {
    pkgError = `${name}: ${error.message || error}`;
  }
  await pickImage();
  if (screen === "device" && !pads) showDevice();
  if (screen === "package") showContents();
  showLoaded();
  await pads?.setPackage(name, pkg ? bytes : null);
}

const BUNDLED = msg("ui.app.firmware_zip_bundled");

// The home card: Load Package, and which one is in use; its i button opens
// the package page (contents, and Unload while one is loaded).
function showLoaded() {
  $("loadpkginfo").textContent = pkg ? pkgName : pkgError || msg("ui.page.no_package_loaded");
  $("loadpkginfo").classList.toggle("error", !pkg);
  $("loadpkg").classList.toggle("missing", !pkg);      // red: nothing to install or upgrade with
  $("pkginfo").disabled = !pkg;
  $("pkginfo").classList.toggle("missing", !pkg);
  $("pkgunload").hidden = !loaded;
}

// The package page: one line a file, as the manifest has them.
function showContents() {
  $("pkgname").textContent = pkg ? pkgName : pkgError || msg("ui.app.no_package");
  $("pkgcontents").textContent = pkg ? pkg.files.map((f) =>
    `${f.name.padEnd(10)} ${f.role.padEnd(8)} ${f.path.padEnd(28)} ${String(f.size).padStart(8)}  ${f.sha256}`).join("\n") : "";
}

async function loadPackage(file) {
  let bytes;
  try {
    bytes = new Uint8Array(await file.arrayBuffer());
    await openPackage(bytes);
  } catch (error) {
    return tell(`${file.name} is not a firmware package: ${error.message || error}`);
  }
  loaded = file.name;
  await setPackage(file.name, bytes);
  showLoaded();                  // the card says which
}

async function unloadPackage() {
  loaded = null;
  if (bundled) await setPackage(BUNDLED, bundled);
  else {
    pkg = null;
    pkgName = null;
    await pads?.setPackage(null, null);
    await pickImage();
    if (screen === "package") showContents();
  }
  showLoaded();
}

async function bundledPackage() {
  if (mock) {
    bundled = mock.packageBytes;
    if (bundled) await setPackage("Mock firmware package", bundled);
    showLoaded();
    return;
  }
  try {
    const r = await fetch("firmware.zip");
    if (r.ok) {
      bundled = new Uint8Array(await r.arrayBuffer());
      if (!loaded) await setPackage(BUNDLED, bundled);
    }
  } catch { /* none: the user loads one */ }
  showLoaded();
}

// Why the browser's Upgrade is off, or what it would write.
let upgradeNote = "";
const fwSay = (text) => { upgradeNote = text; };

// The browser's device page: the linked pad, what it can do here.
function showDevice() {
  if (pads) return;
  const id = pad?.id;
  $("devname").textContent = id ? id.boardName || id.product : pad ? msg("ui.app.connecting") : msg("ui.app.not_connected");
  $("devstate").textContent = !id ? "" : `${id.updater ? msg("ui.app.updater") : `JollyMaam ${id.build}`} · ${pad.usb ? msg("ui.app.usb") : msg("ui.app.bluetooth")}`;
  setIcon($("devicon"), pad?.usb ? "usb" : "ble");
  $("devabout").textContent = id ? [id.product, pad.link.name].filter(Boolean).join(" · ") : "";
  $("doconfigure").disabled = !id || id.updater;
  $("doupgrade").disabled = !image;
  for (const b of ["doinstall", "dobackup", "dorestore"]) $(b).disabled = true;
  $("devnote").textContent = [image ? "" : upgradeNote, id ? msg("ui.app.install_backup_restore_elsewhere") : ""].filter(Boolean).join(" ");
}

// The connected pad's update.bin from the chosen package.
async function pickImage() {
  image = null;
  if (!pkg || !pad?.id) return fwSay(pkg || !pad?.id ? "" : msg("ui.app.no_package_for_upgrade"));
  const file = findFile(pkg, pad.id.product, "update");
  if (!file) return fwSay(`The package has no update for this pad (${pad.id.product}).`, true);
  try {
    const data = await pkg.read(file);
    const wrong = checkUpdateImage(data, pad.id.product);
    if (wrong) return fwSay(wrong, true);
    image = data;
    fwSay(`${file.path}: ${data.length} bytes, SHA-256 ${file.sha256.slice(0, 12)}…`);
  } catch (error) {
    fwSay(String(error.message || error), true);
  }
}

function fwLog(line) {
  $("flashlog").textContent += line + "\n";
}

// The browser's Upgrade page: what it writes, and the button.
function openUpgrade() {
  if (!image) return;
  $("actiontitle").textContent = msg("ui.page.upgrade");
  $("actiondevice").textContent = `${$("devname").textContent} · ${$("devstate").textContent}`;
  const go = h("button", { type: "button", id: "update" }, msg("ui.page.upgrade"));
  go.addEventListener("click", update);
  $("actionbody").replaceChildren(h("p", { id: "fwinfo", class: "muted" }, upgradeNote), h("div", { class: "actions" }, go));
  $("actionresult").textContent = "";
  $("actionresult").classList.remove("error");
  $("padjob").hidden = true;
  $("flashlog").textContent = "";
  enter("action");
}

function result(text, error = false) {
  $("actionresult").textContent = text;
  $("actionresult").classList.toggle("error", error);
  if (error) $("flashdetails").open = true;
}

async function update() {
  if (!image || !ask(`Write this firmware to ${pad.id.boardName}?`)) return;
  const p = pad, data = image;
  p.onDisconnect = null;         // the update restarts the pad
  stop();
  pad = null;
  updating = true;
  $("update").disabled = true;
  $("flashlog").textContent = "";
  result("");
  $("padjob").hidden = false;
  $("padstep").textContent = msg("ui.app.updating_keep_this_page_open_and_the_pad_on");
  $("flashprogress").value = 0;
  let up = null, written = false;
  try {
    up = await intoUpdater(p, linksOf(p.link), fwLog);
    await sendImage(up, data, fwLog, (done, total) => { $("flashprogress").value = done / total; });
    written = true;
    const fresh = await finish(up, linksOf(up.link), fwLog);
    $("padjob").hidden = true;
    $("settings").replaceChildren();
    updating = false;
    result(msg("ui.flash.upgraded_the_pad_starts_it"));
    await attach(fresh);
  } catch (error) {
    updating = false;
    fwLog(`${written ? msg("ui.app.not_found_again") : "failed"}: ${error.message || error}`);
    $("padjob").hidden = true;
    result(`${msg("ui.page.upgrade")}: ${error.message || error}`, true);
    if (written) problem(p.usb ? msg("ui.app.the_new_firmware_is_written_and_started_but_did_not_come_back_on_usb_go_back_and_connect_again")
      : msg("ui.app.the_new_firmware_is_written_and_started_but_did_not_come_back_in_config_mode") +
      msg("ui.app.an_older_updater_does_not_ask_for_it_go_back_hold_home_m_for_3_s_and_connect_again"));
    else problem(`The update failed. The pad stays in its updater: go Back and connect to it (${p.usb ? msg("ui.app.over_usb") :
      msg("ui.app.it_advertises_as_update")}) and update again, or press Home to go back to its app if that is still whole.`);
    up?.close();
  }
}

// The device page's Upgrade of a pad in config mode over Bluetooth (the
// app's): the package's update.bin for it, through its updater.
async function upgradeBle(address, log, progress) {
  const p = new Pad(await bluetooth.choose(address));
  try {
    const id = await p.identify();
    const file = pkg && findFile(pkg, id.product, "update");
    if (!file) throw new Error(`the package has no update for ${id.product}`);
    const data = await pkg.read(file);
    const wrong = checkUpdateImage(data, id.product);
    if (wrong) throw new Error(wrong);
    const up = await intoUpdater(p, bluetooth, log);
    await sendImage(up, data, log, progress);
    (await finish(up, bluetooth, log)).close();
  } catch (error) {
    p.close();
    throw error;
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

$("connect").addEventListener("click", () => connect(bluetooth));
$("connectusb").addEventListener("click", () => connect(usb));
$("back").addEventListener("click", back);
$("pkginfo").addEventListener("click", () => enter("package"));
$("doconfigure").addEventListener("click", () => (pads ? pads.configure() : enter("config")));
$("doinstall").addEventListener("click", () => pads?.install());
$("dobackup").addEventListener("click", () => pads?.backup());
$("dorestore").addEventListener("click", () => pads?.restore());
$("doupgrade").addEventListener("click", () => (pads ? pads.upgrade() : openUpgrade()));
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
  await updaterTrip(p, linksOf(p.link), testLog);
  disconnect();
  $("tests").hidden = false;
  for (const b of document.querySelectorAll("#tests .needs-pad")) b.hidden = true;
  testLog(`The updater round trip ended the link; go Back and connect again${p.usb ? "" : msg("ui.app.home_m")} for more.`);
}));
$("package").addEventListener("change", (e) => {
  const file = e.target.files[0];
  e.target.value = "";
  if (file) loadPackage(file);
});
$("loadpkg").addEventListener("click", () => $("package").click());
$("pkgload").addEventListener("click", () => $("package").click());
$("pkgunload").addEventListener("click", unloadPackage);
$("save").addEventListener("click", save);
$("undo").addEventListener("click", render);
// What this browser or phone lacks: on the choice itself.
function unavailable(id, why) {
  $(id).disabled = true;
  $(id).querySelector("span").textContent = why;
}
if (!bluetooth.available()) unavailable("connect", android ? msg("ui.app.this_phone_has_no_bluetooth")
  : msg("ui.app.this_browser_has_no_web_bluetooth_use_chrome_or_edge"));
if (!usb.available()) unavailable("connectusb", msg("ui.app.this_browser_has_no_webhid_use_chrome_or_edge"));
// The app lists the devices itself (USB, and Bluetooth with its switch on);
// a browser needs the user to choose a pad.
$("homelist").hidden = !pads;
for (const id of ["connect", "connectusb"]) $(id).hidden = !!pads;
history.replaceState(null, "");    // a reload starts at home
show("home");
bundledPackage();
mock?.controls({ usb: () => connect(usb, null, "config"), bluetooth: () => connect(bluetooth, null, "config"), save, loadPackage, unloadPackage, update });
