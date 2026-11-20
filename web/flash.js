import { ask, tell } from "./dialogs.js";
import { text as msg } from "./i18n.js";
// Devices and firmware, in the Android app only (link-android.js:
// libjollymaam over JNI, the app's Bluetooth scan).
//
// Home lists every pad on USB (ours, an updater, a boot loader, stock
// firmware) and, while the Bluetooth switch is on, pads in config mode.
// Tapping one opens its device page: what it is, and Configure, Install,
// Upgrade, Backup and Restore, each enabled when the pad and the package
// allow it.  Configure opens its settings (app.openSettings, app.openBle);
// the rest open an action page.  Install backs the whole flash up first
// unless told not to, then writes our updater and firmware through the
// vendor boot loader, in one loader session.
import { $, h, svg } from "./dom.js";

const OURS = 0, BITDO = 1, JIELI = 2, STOCK = 3;
const ACTION = { none: 0, install: 1, restore: 2, reboot: 3 };
// libjollymaam's progress words, as steps.
const STEPS = { "reading": msg("ui.flash.backing_up_the_flash"), "writing": msg("ui.flash.writing"), "writing and reading back": msg("ui.flash.writing_and_checking"),
  "erasing, writing and reading back": msg("ui.flash.writing_and_checking"), "upgrading": msg("ui.flash.upgrading") };
const BLE_KEY = "jollymaam-bluetooth";
// A pad over Bluetooth not heard for this long has left config mode (it
// advertises every 20-30 ms while in it).
const BLE_GONE_MS = 3000;

// The card's transport icon (Material Symbols' bluetooth and usb paths).
const ICONS = {
  ble: "M440-80v-304L256-200l-56-56 224-224-224-224 56-56 184 184v-304h40l228 228-172 172 172 172L480-80h-40Zm80-496 76-76-76-74v150Zm0 342 76-74-76-76v150Z",
  usb: "M480-80q-33 0-56.5-23.5T400-160q0-21 11-39t29-29v-92H320q-33 0-56.5-23.5T240-400v-92q-18-9-29-27t-11-41q0-33 23.5-56.5T280-640q33 0 56.5 23.5T360-560q0 23-11 41t-29 27v92h120v-320h-80l120-160 120 160h-80v320h120v-80h-40v-160h160v160h-40v80q0 33-23.5 56.5T640-320H520v92q19 10 29.5 28t10.5 40q0 33-23.5 56.5T480-80Z",
};
function icon(kind) {
  const s = svg("svg", { class: "transport", viewBox: "0 -960 960 960", "aria-label": kind === "ble" ? msg("ui.flash.bluetooth") : msg("ui.flash.usb") });
  s.append(svg("path", { d: ICONS[kind] }));
  return s;
}
// The device page's icon (an svg with one path).
export function setIcon(element, kind) {
  element.querySelector("path").setAttribute("d", ICONS[kind]);
}

// What a device is, in a few words.
function stateOf(dev) {
  if (dev.ble) return msg("ui.flash.bluetooth_config_mode");
  const id = dev.id;
  if (!id) return "…";
  if (id.error) return id.error;
  if (id.kind === OURS) return id.updater ? msg("ui.flash.in_its_updater") : `JollyMaam ${id.build}`;
  if (id.kind === JIELI) return `Jieli ${id.chip} boot loader`;
  if (id.kind === BITDO) return msg("ui.flash.8bitdo_boot_loader");
  return `Stock firmware${id.mode ? `, ${id.mode}` : ""}`;
}
const nameOf = (dev) => dev.ble ? dev.ble.name : dev.id?.name || (dev.id?.kind === JIELI ? msg("ui.flash.a_pad") : dev.d.name);

export function setupPads(flasher, bluetooth, app) {
  const bleKey = bluetooth.storageKey ?? BLE_KEY;
  let targets = null, timer = null, scanning = false, busy = false;
  let where = "off";            // "home", "device", "off"
  let selected = null;          // the device page's device: its key
  const ids = new Map();        // USB: what each device is (probed once while it stays)
  let usbFound = [];            // USB: [{key, d, id}]
  const bleFound = new Map();   // Bluetooth: key -> {key, ble: {name, id}, heard: ms}
  let stopScan = null;
  let bleOn = true;             // on unless switched off
  try { bleOn = localStorage.getItem(bleKey) !== "0"; } catch { /* no storage: on */ }
  let blePermission = null;     // null: not answered yet; then true or false

  const log = (line) => { $("flashlog").textContent += line + "\n"; };
  function button(content, click, cls = null) {
    const b = h("button", { type: "button", class: cls }, content);
    b.addEventListener("click", click);
    return b;
  }
  const errorText = (e) => String(e?.message || e);
  const devices = () => [...usbFound, ...bleFound.values()];
  const current = () => devices().find((x) => x.key === selected) ?? null;

  // ---- watching: USB every second, Bluetooth while the switch is on ----

  // Pads over Bluetooth gone quiet: out of config mode.
  function dropQuiet() {
    const now = Date.now();
    let dropped = false;
    for (const [k, v] of bleFound) if (now - v.heard > BLE_GONE_MS) dropped = bleFound.delete(k);
    if (dropped) draw();
  }

  async function scanUsb() {
    dropQuiet();
    if (busy || scanning || where === "off") return;
    let found;
    try {
      scanning = true;
      found = await flasher.scan();
    } catch {
      found = [];
    } finally {
      scanning = false;
    }
    if (busy || where === "off") return;
    for (const k of ids.keys()) if (!found.some((d) => d.device === k)) ids.delete(k);
    usbFound = found.map((d) => ({ key: d.device, d, id: ids.get(d.device) }));
    draw();
    for (const dev of usbFound) {
      if (dev.id) continue;
      try {
        ids.set(dev.d.device, await flasher.probe(dev.d.device));
      } catch (error) {
        ids.set(dev.d.device, { error: errorText(error) });
      }
      dev.id = ids.get(dev.d.device);
      if (where === "off") return;
      draw();
    }
  }

  // Asked once per run of the app: denied, the switch stays off and disabled.
  let asking = null;
  function allowed() {
    asking ??= bluetooth.permission().then((yes) => !!yes, () => false).then((yes) => {
      blePermission = yes;
      for (const s of document.querySelectorAll("input.bleswitch")) {
        s.disabled = !yes;
        if (!yes) s.checked = false;
      }
      for (const help of document.querySelectorAll(".ble-permission-help")) help.hidden = yes;
      return yes;
    });
    return asking;
  }

  async function bleScan(on) {
    if (on && !(await allowed())) on = false;
    if (on && !stopScan && where !== "off") {
      bleFound.clear();
      stopScan = bluetooth.scan((v) => {
        const key = `ble:${v.id}`;
        const known = bleFound.has(key);
        bleFound.set(key, { key, ble: v, heard: Date.now() });
        if (!known) draw();
      }, (why) => { $("blenote").textContent = why; });
    } else if (!on && stopScan) {
      stopScan();
      stopScan = null;
      bleFound.clear();
    }
  }

  function setBle(on) {
    bleOn = on;
    try { localStorage.setItem(bleKey, on ? "1" : "0"); } catch { /* not kept */ }
    for (const s of document.querySelectorAll("input.bleswitch")) s.checked = on;
    $("blenote").textContent = "";
    bleScan(on).then(draw);
  }
  for (const s of document.querySelectorAll("input.bleswitch")) s.addEventListener("change", () => setBle(s.checked));

  function watch(screen) {
    where = screen;
    shown = "";
    for (const s of document.querySelectorAll("input.bleswitch")) s.checked = bleOn && blePermission !== false;
    bleScan(bleOn).then(draw);
    scanUsb();
    timer ??= setInterval(scanUsb, 1000);
    draw();
  }

  // ---- the device lists ----

  let shown = "";
  function draw() {
    if (where === "off") return;
    const all = devices();
    const ble = bleOn && blePermission !== false;
    const key = JSON.stringify([where, selected, all.map((x) => [x.key, stateOf(x), nameOf(x), can(x).why]), !!targets, ble]);
    if (key === shown) return;
    shown = key;
    if (where === "device") return page();
    const hint = ble && !bleFound.size ? msg("ui.flash.hold_home_m_for_3_s_to_show_a_pad_here_over_bluetooth") : "";
    $("homedevices").replaceChildren(...all.map((dev) => button([h("div", {}, h("strong", {}, nameOf(dev)), h("span", {}, stateOf(dev))),
      icon(dev.ble ? "ble" : "usb")], () => tap(dev), "choice device")),
    ...(all.length ? [] : [h("p", { class: "muted" }, msg("ui.flash.plug_in_a_pad_over_usb"))]),
    ...(hint ? [h("p", { class: "muted" }, hint)] : []));
  }

  function tap(dev) {
    if (!dev.ble && !dev.id) return;          // still being asked what it is
    selected = dev.key;
    app.enterDevice();
  }

  // The device page: what the pad is, and what it can do.
  function page() {
    const dev = current();
    const c = can(dev);
    $("devname").textContent = dev ? nameOf(dev) : msg("ui.flash.device_gone");
    $("devstate").textContent = dev ? stateOf(dev) : msg("ui.flash.device_gone_about");
    setIcon($("devicon"), dev?.ble ? "ble" : "usb");
    $("devicon").hidden = !dev;
    $("devabout").textContent = dev ? about(dev) : "";
    $("doconfigure").disabled = !c.configure;
    $("doinstall").disabled = !c.install;
    $("doupgrade").disabled = !c.upgrade;
    $("dobackup").disabled = !c.backup;
    $("dorestore").disabled = !c.restore;
    $("devnote").textContent = c.why;
  }

  // A line about it: how it is connected, its product, what the package has for it.
  function about(dev) {
    if (dev.ble) return msg("ui.flash.about_ble");
    const id = dev.id ?? {};
    const parts = [msg("ui.flash.about_usb", { name: dev.d.name })];
    if (id.product) parts.push(msg("ui.flash.about_product", { product: id.product }));
    if (id.chip) parts.push(msg("ui.flash.about_chip", { chip: id.chip }));
    if (targets) {
      const fits = id.kind === JIELI ? targets.some((x) => x.chip === id.chip) : !!forProduct(id.product);
      parts.push(fits ? msg("ui.flash.about_package_has_it") : msg("ui.flash.about_package_lacks_it"));
    } else parts.push(msg("ui.flash.about_no_package"));
    return parts.join(" · ");
  }

  // ---- the actions ----

  const forProduct = (p) => targets?.find((t) => t.product === p) ?? null;

  // What the selected device can do, and why not.
  function can(dev) {
    const none = { configure: false, install: false, backup: false, upgrade: false, restore: false, why: "" };
    if (!dev) return none;
    if (dev.ble) return { ...none, configure: true, upgrade: !!targets, why: targets ? "" : msg("ui.flash.no_package") };
    const id = dev.id;
    if (!id || id.error) return { ...none, why: id?.error ?? "" };
    const fits = id.kind === JIELI ? (targets ?? []).some((x) => x.chip === id.chip) : !!forProduct(id.product);
    const reachable = id.kind !== STOCK || id.canEnter;
    const flash = !!targets && reachable;
    return {
      configure: id.kind === OURS && !id.updater,
      install: flash && fits, backup: flash, restore: flash,
      upgrade: id.kind === OURS && !!forProduct(id.product),
      why: !targets ? msg("ui.flash.no_package") : !reachable ? msg("ui.flash.manual_bootloader")
        : !fits ? msg("ui.flash.package_missing_device", { device: id.product || id.chip }) : "",
    };
  }

  // An action page for the selected device.
  function open(title, body) {
    const dev = current();
    if (!dev) return null;
    $("actiontitle").textContent = title;
    $("actiondevice").textContent = `${nameOf(dev)} · ${stateOf(dev)}`;
    $("actionbody").replaceChildren(...body);
    $("actionresult").textContent = "";
    $("actionresult").classList.remove("error");
    $("padjob").hidden = true;
    $("flashlog").textContent = "";
    app.enterAction();
    return dev;
  }

  const toggle = (label, on) => {
    const box = h("input", { type: "checkbox", class: "switch", checked: on });
    return [h("label", { class: "check" }, box, ` ${label}`), box];
  };

  function configure() {
    const dev = current();
    if (!dev) return;
    if (dev.ble) return app.openBle(dev.ble.id);
    if (dev.id?.kind === OURS && !dev.id.updater) app.openSettings(dev.d.device);
  }
  function install() {
    const [backupRow, backup] = toggle(msg("ui.flash.back_up_first"), true);
    const dev = open(msg("ui.flash.install"), [h("p", {}, msg("ui.flash.this_wipes_all_settings_and_the_flash")), backupRow,
      h("div", { class: "actions" }, button(msg("ui.flash.install"), () => {
        if (ask(msg("ui.flash.install_jollymaam_this_wipes_all_settings_and_the_flash"))) {
          session(dev, msg("ui.flash.install"), { backup: backup.checked, action: ACTION.install }, msg("ui.flash.installed_the_pad_starts_jollymaam"));
        }
      }))]);
  }
  function backup() {
    const dev = open(msg("ui.flash.backup"), [h("div", { class: "actions" }, button(msg("ui.flash.back_up"), () =>
      session(dev, msg("ui.flash.backup"), { backup: true, action: ACTION.none }, msg("ui.flash.backed_up_the_pad_is_in_its_boot_loader"))))]);
  }
  function startUpgrade() {
    const dev = open(msg("ui.flash.upgrade"), [h("div", { class: "actions" }, button(msg("ui.flash.upgrade"), () => upgrade(dev)))]);
  }
  function restore() {
    // A backup of this pad or a vendor image (a raw write of a whole
    // backup is the command line's: jollymaam install --raw).
    const file = h("input", { type: "file", hidden: true });
    const dev = open(msg("ui.flash.restore"), [h("div", { class: "actions" }, button(msg("ui.flash.choose_a_file"), () => file.click()), file)]);
    file.addEventListener("change", async () => {
      const f = file.files[0];
      file.value = "";
      if (!f || !ask(`Write ${f.name} to the pad?`)) return;
      session(dev, msg("ui.flash.restore"), { action: ACTION.restore, image: new Uint8Array(await f.arrayBuffer()) }, msg("ui.flash.restored_the_pad_starts_it"));
    });
  }

  // ---- jobs ----

  function showProgress(what, done, total) {
    $("padstep").textContent = `${STEPS[what] ?? what}…`;
    $("flashprogress").value = total ? done / total : 0;
  }

  async function job(what, work, after) {
    busy = true;
    for (const b of document.querySelectorAll("#action button, #action input")) b.disabled = true;
    $("actionresult").textContent = "";
    $("actionresult").classList.remove("error");
    $("flashlog").textContent = "";
    $("padjob").hidden = false;
    $("padstep").textContent = "Starting…";
    $("flashprogress").value = 0;
    const t0 = performance.now();
    try {
      const r = await work();
      $("actionresult").textContent = `${after} ${((performance.now() - t0) / 1000).toFixed(0)} s.${r?.backup ? ` Backup: ${r.backup}.` : ""}`;
    } catch (error) {
      $("actionresult").textContent = `${what} failed: ${errorText(error)}`;
      $("actionresult").classList.add("error");
      $("flashdetails").open = true;
    } finally {
      $("padjob").hidden = true;
      busy = false;
      ids.clear();              // whatever is on USB now is asked again
    }
  }

  function session(dev, what, { backup = false, action, image = null, raw = false }, after) {
    const id = dev.id;
    return job(what, () => flasher.session(dev.d.device, { uid: id.uid, backup, label: id.product || `jieli-${id.chip}`,
      action, image, raw }, showProgress, log), after);
  }

  function upgrade(dev) {
    if (dev.ble) {
      return job(msg("ui.flash.upgrade"), () => app.upgradeBle(dev.ble.id, log, (done, total) => showProgress("upgrading", done, total)),
        msg("ui.flash.upgraded_the_pad_starts_it"));
    }
    const id = dev.id;
    return job(msg("ui.flash.upgrade"), () => flasher.update(dev.d.device, id.product, id.uid, showProgress, log), msg("ui.flash.upgraded_the_pad_starts_it"));
  }

  // ---- the package ----

  async function setPackage(name, bytes) {
    targets = null;
    try {
      if (bytes) targets = await flasher.package(bytes);
    } catch { /* app.js said why */ }
    $("padhow").replaceChildren(...(targets ? [h("ul", {}, targets.map((t) =>
      h("li", {}, h("strong", {}, t.name), t.byHand ? `: ${t.byHand}.` : msg("ui.flash.plug_it_in"))))] : []));
    shown = "";
    draw();
  }

  return {
    setPackage,
    configure, install, backup, restore, upgrade: startUpgrade,
    home() { selected = null; watch("home"); },
    device() { watch("device"); },
    // Another screen; false while a job runs (stay).
    leave() {
      if (busy) return false;
      where = "off";
      bleScan(false);
      clearInterval(timer);
      timer = null;
      return true;
    },
  };
}
