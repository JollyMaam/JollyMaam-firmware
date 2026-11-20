// In-memory devices speaking the same framing as USB and Bluetooth. Nothing
// here opens a browser device chooser or calls the Android bridge.
import { CMD, MORE, T, kindOf, BASE_TARGET, BASE_MACROS_TARGET } from "./pad.js";
import { parseMap, parseRules, parseMacros, encodeMacros, curvePreset } from "./remap.js";
import { sha256 } from "./package.js";
import { useDialogs } from "./dialogs.js";
import { installTour } from "./mock-tour.js";

const enc = (s) => new TextEncoder().encode(s);
const cat = (...parts) => Uint8Array.from(parts.flatMap((p) => Array.from(p)));
const uint = (n, size = 4) => Uint8Array.from({ length: size }, (_, i) => n >>> (8 * i) & 255);
const word = (n) => uint(n, 2);
const string = (s) => cat(enc(s), [0]);
const delay = (ms) => new Promise((r) => setTimeout(r, ms));
const held = () => new Promise(() => {});

export const BOARDS = {
  c2: ["GS_C2_ADC_DEVICE", "Cyclone 2", 0],
  n2: ["AC695X_TRANS", "Nova 2 Lite", 0],
  u2cw: ["U2C_WIRELESS", "Ultimate 2C Wireless", 2],
  u2cb: ["U2C_BLUETOOTH", "Ultimate 2C Bluetooth", 2],
};
export const APP_SCENARIOS = new Set(["stock", "manual", "jieli", "bitdo", "probe_error", "permission", "no_devices"]);

export const SCENARIOS = {
  normal: "Connected controller — edit, save, reset, remap and macros",
  populated: "Saved settings, rules and inherited macros",
  loading: "Slow settings loading",
  unavailable: "Browser lacks USB and Bluetooth support",
  empty: "Dongle — no settings",
  unknown: "Future firmware — unknown setting and choice",
  incompatible: "Unsupported protocol",
  read_error: "Settings read error",
  updater: "Controller in updater",
  no_app: "Updater with no valid application",
  ephemeral: "Settings apply until the next boot",
  no_devices: "No devices found",
  permission: "Bluetooth permission denied",
  probe_error: "USB identification error",
  stock: "Stock firmware — can enter boot loader",
  manual: "Stock firmware — manual boot loader required",
  jieli: "Jieli boot loader",
  bitdo: "8BitDo boot loader",
};

export function makeFields(schema, board, scenario) {
  let fields = schema.filter((f) => !(board.startsWith("u2c") && (f.kind === 1 ||
    ["grips", "players", "connect_colour", "primary_colour", "secondary_colour", "usb_audio"].includes(f.name))));
  fields = fields.map((f) => ({ ...f, help: "", names: [...f.options],
    max: f.type === T.ENUM ? f.options.length - 1 : f.max,
    size: f.name === "rules" && board.startsWith("u2c") ? 40 : f.size }));
  if (scenario === "empty") return [];
  if (scenario === "unknown") fields.push({ kind: 0, id: 254, name: "future_mode", help: "",
    type: T.ENUM, size: 1, max: 2, names: ["off", "automatic", "future_choice"], default: 2 });
  return fields;
}

function defaultValue(f, target) {
  const out = new Uint8Array(f.size);
  if (f.name === "map") return parseMap("");
  if (f.name === "macros") return encodeMacros([], f.size);
  if (f.name.endsWith("_curve_points")) return Uint8Array.from(curvePreset("linear", f.size));
  if (f.type === T.TEXT && target === BASE_TARGET) out.set(enc("Mock base").subarray(0, f.size));
  else if (f.type !== T.BYTES && f.type !== T.TEXT) out.set(uint(f.default || 0, Math.min(f.size, 4)));
  if (f.name === "radio") out[0] = 1;
  return out;
}

export class MockDevice {
  constructor(schema, { board = "c2", scenario = "normal", outcome = "success", speed = 30 } = {}) {
    this.board = BOARDS[board] ? board : "c2";
    this.scenario = scenario;
    this.outcome = outcome;
    this.speed = speed;
    this.fields = makeFields(schema, this.board, scenario);
    this.saved = new Map(); this.staged = new Map(); this.result = [];
    this.updater = scenario === "updater" || scenario === "no_app";
    this.info = { state: 0, written: 0, error: 0, begun: 0 };
    this.links = new Set(); this.state = 4; this.battery = "charging";
    if (scenario === "populated") {
      this.saved.set("18:2", parseMap("south=east east=south"));
      const rules = this.fields.find((f) => f.kind === 3 && f.name === "rules");
      const macros = this.fields.find((f) => f.kind === 4);
      this.saved.set("18:3", parseRules("south pressed tap east", rules.size / 10));
      this.saved.set("27:1", parseMacros("Demo: +south 40 -south", macros.size));
    }
  }
  async gate(kind) {
    if (this.hold !== kind) return;
    this.waiting = kind;
    await new Promise((resolve) => { this.release = () => { this.release = null; this.waiting = null; this.hold = null; resolve(); }; });
  }
  get product() { return BOARDS[this.board][0]; }
  get name() { return BOARDS[this.board][1]; }
  value(f, target, defaults) {
    if (!defaults) {
      const own = this.saved.get(`${target}:${f.id}`);
      if (own) return { value: own.slice(), set: true };
      const base = f.kind === 3 ? BASE_TARGET : f.kind === 4 ? BASE_MACROS_TARGET : null;
      const inherited = base !== null && this.saved.get(`${base}:${f.id}`);
      if (inherited) return { value: inherited.slice(), set: false };
    }
    return { value: defaultValue(f, target), set: false };
  }
  disconnect() { for (const link of this.links) link.lose(); }
  async answer(sub, arg, data) {
    await delay(this.speed);
    if (sub === CMD.FIELDS) await this.gate("fields");
    if (sub === CMD.WRITE) await this.gate("write");
    if (sub === CMD.UPDATE_DATA) await this.gate("update");
    if (sub === CMD.IDENTIFY) {
      const out = new Uint8Array(90);
      out.set(enc("C2PD")); out[4] = this.scenario === "incompatible" ? 99 : 2;
      out[7] = +this.updater; out.set(enc("mock-build"), 8); out.fill(0x12, 24, 40);
      out.set(enc(this.product).subarray(0, 16), 40); out.set(enc(this.name), 56); out.set(word(488), 88);
      return out;
    }
    if (sub === CMD.STATUS) {
      const low = this.battery === "low", charging = this.battery === "charging";
      const full = this.battery === "full", unknown = this.battery === "unknown";
      return cat([this.state, 0, 1, +low], word(low ? 3300 : 3950),
        [unknown ? 0 : 1 | 2 | 4 | 32 | (low ? 64 : 0) | (charging ? 8 | 16 : 0) | (full ? 128 : 0), full ? 100 : low ? 5 : 75],
        uint(120000), [+(this.scenario === "ephemeral"), 0], word(this.saved.size));
    }
    if (sub === CMD.LAYOUT) {
      const face = this.board === "c2" || this.board === "n2" ? ["A", "B", "X", "Y"] : ["B", "A", "Y", "X"];
      const entries = [[0, face[0]], [1, face[1]], [3, face[2]], [4, face[3]], [32, ""], [33, ""]];
      const here = entries.slice(arg);
      return cat([1, 12, entries.length, here.length], ...here.map(([input, label]) =>
        cat([input, input >= 32 ? 4 : 0], enc(label.padEnd(8, "\0")), [0, 0])));
    }
    if (sub === CMD.FIELDS) {
      if (this.scenario === "loading" && !this.guided) await delay(2000);
      if (this.scenario === "read_error") throw new Error("Mock: settings could not be read.");
      const entries = []; let size = 1;
      for (const f of this.fields.slice(arg)) {
        const defs = f.size < 4 ? Array.from({ length: f.kind === 0 ? 8 : 1 }, (_, t) => defaultValue(f, t)) : [];
        const entry = cat([1, f.kind, f.id, f.type, f.size, +f.dev], word(f.max), string(f.name), [0],
          ...f.names.map(string), ...defs);
        if (size + 2 + entry.length > 512) break;
        entries.push(cat(word(entry.length), entry)); size += 2 + entry.length;
      }
      return cat([entries.length], ...entries);
    }
    if (sub === CMD.READ || sub === CMD.DEFAULTS) {
      const fields = this.fields.filter((f) => f.kind === kindOf(arg));
      return cat([1, arg, fields.length], ...fields.map((f) => {
        const { value, set } = this.value(f, arg, sub === CMD.DEFAULTS);
        return cat([f.id, +set, value.length], value);
      }));
    }
    if (sub === CMD.WRITE) {
      if (this.outcome === "busy") return held();
      this.result = [];
      for (let o = 0; o < data.length;) {
        const [target, id, length] = data.subarray(o, o + 3), value = data.slice(o + 3, o + 3 + length);
        const f = this.fields.find((f) => f.kind === kindOf(target) && f.id === id);
        const code = !f ? 1 : this.outcome === "failure" ? this.writeError || 3 : 0;
        this.result.push(code);
        if (!code) this.staged.set(`${target}:${id}`, value);
        o += 3 + length;
      }
    }
    if (sub === CMD.RESULT) return cat([this.result.length], this.result);
    if (sub === CMD.DROP) this.staged.clear();
    if (sub === CMD.COMMIT) {
      for (const [key, value] of this.staged) value.length ? this.saved.set(key, value) : this.saved.delete(key);
      this.staged.clear();
    }
    if (sub === CMD.QUICK_MACRO) {
      if (this.scenario !== "populated") return Uint8Array.of(1, 0, 0, 0);
      const raw = parseMacros(": +south 40 -south"), steps = raw.slice(2, raw[0] + 1);
      return cat([1, 1, 0, steps.length], steps.slice(arg));
    }
    if (sub === CMD.UPDATER || sub === CMD.REBOOT) {
      this.updater = sub === CMD.UPDATER;
      setTimeout(() => this.disconnect(), 0);
    }
    if (sub === CMD.UPDATE_BEGIN) this.info = { state: 1, written: 0, error: 0, begun: arg };
    if (sub === CMD.UPDATE_DATA) {
      if (this.outcome === "busy") return held();
      if (this.outcome === "failure") this.info.error = this.updateError || 4;
      else this.info.written = arg + data.length;
    }
    if (sub === CMD.UPDATE_COMMIT) this.info.state = 2;
    if (sub === CMD.UPDATE_INFO) return cat([1, this.info.state, this.info.error, +(this.scenario !== "no_app")], uint(0x3000), uint(0x80000),
      uint(this.info.written), uint(this.info.begun), uint(0), uint(488));
    return new Uint8Array(0);
  }
  link(kind = "hid") {
    let receive = () => {}, gone = () => {}, chunks = [], closed = false;
    const device = this;
    const link = { get commandTimeoutMs() { return device.guided && device.hold ? 0x7fffffff : undefined; }, kind, name: `Mock ${this.name}`, id: "mock-controller", reportSize: 512, maxWrite: 96,
      onInput(fn) { receive = fn; }, onReply(fn) { receive = fn; }, onDisconnect(fn) { gone = fn; },
      close() { closed = true; device.links.delete(link); },
      lose() { if (!closed) { link.close(); gone(); } },
      async sendFeature(report) {
        if (closed) throw new Error("Mock: link lost");
        const v = new DataView(report.buffer, report.byteOffset), out = report[18] & 1;
        const length = v.getUint16(16, true), client = v.getUint32(20, true), tag = report[19];
        const body = await device.answer(report[1], v.getUint32(2, true), out ? report.slice(24, 24 + length) : new Uint8Array(0));
        const answer = new Uint8Array(out ? 0 : length); answer.set(body.subarray(0, answer.length));
        for (let offset = 0;; offset += 56) {
          const r = new Uint8Array(64), w = new DataView(r.buffer);
          w.setUint32(0, client, true); r[4] = tag; r[5] = offset + 56 < answer.length ? 2 : 3;
          w.setUint16(6, offset, true); r.set(answer.subarray(offset, offset + 56), 8); receive(r);
          if (offset + 56 >= answer.length) break;
        }
      },
      async write(frame) {
        if (closed) throw new Error("Mock: link lost");
        const v = new DataView(frame.buffer, frame.byteOffset), tag = v.getUint32(1, true);
        chunks.push(frame.slice(9)); if (frame[0] & MORE) return;
        const data = cat(...chunks); chunks = [];
        const body = await device.answer(frame[0], v.getUint32(5, true), data);
        receive(cat([1, 0], uint(tag), word(body.length), body));
      },
    };
    this.links.add(link); return link;
  }
}

// A small stored ZIP with valid hashes, consumed by the real package reader.
export async function mockPackage(excludeProduct = null) {
  const entries = [], lines = ["jollymaam-firmware 2"];
  for (const [product] of Object.values(BOARDS)) {
    if (product === excludeProduct) continue;
    const data = new Uint8Array(0x120 + 1024), view = new DataView(data.buffer);
    data.set(enc("MAIN")); view.setUint32(4, 1024, true); view.setUint32(16, 1, true); data.set(enc(product), 24);
    const path = `${product}/update.bin`;
    lines.push(`${product} update ${path} ${data.length} ${await sha256(data)}`);
    entries.push([path, data]);
  }
  entries.unshift(["manifest.txt", enc(lines.join("\n") + "\n")]);
  const locals = [], central = []; let offset = 0;
  for (const [path, data] of entries) {
    const name = enc(path), local = cat(uint(0x04034b50), word(20), word(0), word(0), uint(0), uint(0),
      uint(data.length), uint(data.length), word(name.length), word(0), name, data);
    central.push(cat(uint(0x02014b50), word(20), word(20), word(0), word(0), uint(0), uint(0),
      uint(data.length), uint(data.length), word(name.length), word(0), word(0), word(0), word(0), uint(0), uint(offset), name));
    locals.push(local); offset += local.length;
  }
  const directory = cat(...central);
  return cat(...locals, directory, uint(0x06054b50), word(0), word(0), word(entries.length), word(entries.length),
    uint(directory.length), uint(offset), word(0));
}

export async function createMock(params, schema) {
  const option = (key, fallback) => params.get(key) || fallback;
  const device = new MockDevice(schema, { board: option("board", "c2"), scenario: option("mock", "normal"),
    outcome: option("outcome", "success"), speed: params.has("review") ? 0 : 30 });
  device.updateError = Number(params.get("error")) || 4;
  device.writeError = Number(params.get("refusal")) || 3;
  if (!SCENARIOS[device.scenario]) device.scenario = "normal";
  const mode = device.scenario === "unavailable" ? "browser" : option("view", APP_SCENARIOS.has(device.scenario) ? "app" : "browser"), packageState = option("package", "present");
  const packageBytes = packageState === "missing" ? null : packageState === "corrupt" ? enc("damaged package")
    : await mockPackage(packageState === "mismatch" ? device.product : null);
  const links = (kind) => ({
    available: () => device.scenario !== "unavailable",
    async choose() {
      if (device.scenario === "unavailable") throw new Error("Mock: this browser has no USB or Bluetooth API.");
      if (device.scenario === "no_devices") throw Object.assign(new Error("No mock pad chosen."), { name: "NotFoundError" });
      if (device.scenario === "permission" && kind !== "hid") throw new Error("Mock: Bluetooth permission denied.");
      return device.link(kind);
    },
    async reconnect() { return device.link(kind); },
  });
  const usb = links("hid"), bluetooth = { ...links("ble"), storageKey: "jollymaam-mock-bluetooth",
    async permission() { return device.scenario !== "permission"; },
    scan(found) {
      const emit = () => { if (!["no_devices", "permission", "stock", "manual", "jieli", "bitdo", "probe_error"].includes(device.scenario))
        found({ id: "mock-ble", name: `Mock ${device.name}` }); };
      emit(); const timer = setInterval(emit, 1000); return () => clearInterval(timer);
    },
  };
  const target = { product: device.product, name: device.name, chip: 1, byHand: "Mock: hold Home+Start while plugging in USB" };
  async function work(progress, log, backup = false) {
    log("Mock operation: no hardware is accessed.");
    if (backup) log("Saving mock backup…");
    for (let done = 0; done <= 100; done += 10) {
      progress(backup ? "reading" : "writing and reading back", done, 100);
      if (done === 30) await device.gate("job");
      await delay(params.has("review") ? 5 : 180);
      if (done === 30 && device.outcome === "busy") return held();
      if (done === 50 && device.outcome === "failure") throw new Error("Mock: transfer failed while checking the image.");
    }
    log("Mock operation completed.");
    return backup ? { backup: "Download/JollyMaam/mock-backup.bin" } : {};
  }
  const flasher = {
    async scan() {
      return device.scenario === "no_devices" ? [] : [{ device: "mock-usb", name: `Mock ${device.name}`,
        kind: ["stock", "manual"].includes(device.scenario) ? 3 : 0 }];
    },
    async probe() {
      await delay(80);
      if (device.scenario === "probe_error") throw new Error("Mock: USB identification failed.");
      return { ...target, uid: "mock-uid", build: "mock-build", updater: device.updater,
        kind: ["stock", "manual"].includes(device.scenario) ? 3 : device.scenario === "jieli" ? 2 : device.scenario === "bitdo" ? 1 : 0,
        canEnter: device.scenario !== "manual", byHand: target.byHand };
    },
    async package() {
      if (packageState === "corrupt") throw new Error("Mock: corrupt package.");
      return packageState === "mismatch" ? [{ ...target, product: "OTHER_BOARD", chip: 99 }] : [target];
    },
    async session(_device, options, progress, log) { return work(progress, log, options.backup); },
    async update(_device, _product, _uid, progress, log) { return work(progress, log); },
  };
  return { review: params.has("review"), device, usb, bluetooth, android: mode === "app" ? { usb, bluetooth, flasher } : null, packageBytes,
    notice(message) {
      let notice = document.getElementById("mock-notice");
      if (!notice) { notice = document.createElement("p"); notice.id = "mock-notice"; notice.setAttribute("role", "alert"); (params.has("review") ? document.querySelector("main") : document.getElementById("mock-controls")).prepend(notice); }
      notice.textContent = message;
    },
    controls(actions) {
      if (params.has("review")) useDialogs((kind, message, choices) => {
        if (kind === "confirm" && params.get("answer") !== "cancel") return true;
        let dialog = document.getElementById("review-dialog");
        if (!dialog) { dialog = document.createElement("section"); dialog.id = "review-dialog"; dialog.setAttribute("role", "dialog"); document.querySelector("main").prepend(dialog); }
        dialog.replaceChildren();
        const title = document.createElement("strong"); title.textContent = kind === "alert" ? "Browser alert" : "Browser confirmation";
        const text = document.createElement("p"); text.textContent = kind === "choose" ? `${message} [${choices.map((c) => c[1]).join(" / ")}]` : message;
        dialog.append(title, text); return false;
      });
      installControls(params, device, actions, mode);
    } };
}

function installControls(params, device, actions, mode) {
  if (device.scenario === "unavailable" && params.get("view") !== "browser") {
    const url = new URL(location.href); url.searchParams.set("view", "browser");
    history.replaceState(history.state, "", url);
  }
  const panel = document.createElement("aside"); panel.id = "mock-controls";
  panel.style.cssText = "padding:12px;border:2px solid #b76c00;margin:12px;background:var(--bg);position:relative;z-index:10;display:flex;flex-wrap:wrap;gap:6px;align-items:end";
  const title = document.createElement("strong"); title.textContent = "Mock UI — in-memory devices; no hardware"; title.style.width = "100%"; panel.append(title);
  const choices = (key, title, options, fallback) => {
    const label = document.createElement("label"); label.style.cssText = "display:inline-flex;flex-direction:column;max-width:100%;margin:6px;gap:4px"; label.append(title + " ");
    const select = document.createElement("select"); select.style.maxWidth = "100%"; select.setAttribute("aria-label", "Mock " + title);
    for (const [value, text] of Object.entries(options)) { const o = document.createElement("option"); o.value = value; o.textContent = text; select.append(o); }
    select.value = key === "view" ? mode : params.get(key) || fallback;
    if (key === "view" && device.scenario === "unavailable") select.disabled = true;
    select.onchange = () => { const url = new URL(location.href); url.searchParams.set(key, select.value); url.searchParams.delete("step"); if (key === "mock") url.searchParams.set("view", APP_SCENARIOS.has(select.value) ? "app" : "browser"); location.href = url; };
    label.append(select); panel.append(label);
  };
  choices("mock", "Scenario", SCENARIOS, "normal");
  choices("view", "View", { browser: "Browser", app: "Android app" }, mode);
  choices("board", "Board", Object.fromEntries(Object.entries(BOARDS).map(([k, v]) => [k, v[1]])), "c2");
  choices("outcome", "Operation", { success: "Success", failure: "Failure / refused save", busy: "Stay busy" }, "success");
  choices("package", "Package", { present: "Bundled mock package", missing: "No package", mismatch: "Other board", corrupt: "Damaged package" }, "present");
  const button = (label, action) => { const b = document.createElement("button"); b.type = "button"; b.textContent = label; b.onclick = action; panel.append(b); };
  button("Connect mock USB", actions.usb); button("Connect mock Bluetooth", actions.bluetooth);
  button("Disconnect mock pad", () => device.disconnect());
  button("Expand settings and help", () => document.querySelectorAll('#settings details').forEach((e) => { e.open = true; }));
  const live = (title, values, change) => {
    const label = document.createElement("label"), select = document.createElement("select");
    label.append(title + " "); select.style.maxWidth = "100%"; select.setAttribute("aria-label", "Mock " + title);
    for (const [value, name] of Object.entries(values)) { const o = document.createElement("option"); o.value = value; o.textContent = name; select.append(o); }
    select.value = title === "Connection state" ? String(device.state) : device.battery;
    select.onchange = () => change(select.value); label.append(select); panel.append(label);
  };
  live("Connection state", {4:"Connected", 0:"Boot", 1:"Idle", 2:"Connecting", 3:"Pairing", 5:"Stopping", 6:"Sleeping", 7:"Rebooting"}, (v) => { device.state = Number(v); });
  live("Battery", { charging:"Charging", battery:"On battery", low:"Low", full:"Full", unknown:"Unknown" }, (v) => { device.battery = v; });
  const download = (name, data) => {
    const url = URL.createObjectURL(new Blob([data], {type:"application/octet-stream"}));
    const a = document.createElement("a"); a.href = url; a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  button("Download mock package", async () => download("mock-firmware.zip", await mockPackage()));
  button("Download mock backup", () => download("mock-backup.bin", new Uint8Array(1024)));
  const note = document.createElement("p"); note.textContent = "Use the normal controls to edit settings, exercise validation, add rules/macros, save, discard and run firmware actions. Scenario changes reload and clear mock settings. Status controls appear on the next poll (5 s). Firmware actions can stay busy until reload; protocol commands still have normal timeouts."; panel.append(note);
  const reviewLink = document.createElement("a"); reviewLink.href = "review.html"; reviewLink.textContent = "Open workflow review →"; panel.prepend(reviewLink);
  document.body.prepend(panel);
  if (params.has("review")) panel.hidden = true;
  installTour(panel, { scenario: device.scenario, app: mode === "app", outcome: device.outcome, packageState: params.get("package") || "present", workflow: params.get("workflow") || "", answer: params.get("answer") || "accept" }, guidedActions(device, actions, params));
}

function guidedActions(device, actions, params) {
  let pending, input;
  const workflow = params.get("workflow") || "backup";
  const file = (element, name, bytes) => {
    const transfer = new DataTransfer(); transfer.items.add(new File([bytes], name));
    element.files = transfer.files; element.dispatchEvent(new Event("change", { bubbles: true }));
  };
  const wait = async (condition) => {
    const end = Date.now() + 12000;
    while (!condition()) { if (Date.now() > end) throw new Error("The expected UI state did not appear."); await delay(25); }
  };
  const click = (selector) => {
    const element = document.querySelector(selector);
    if (!element || element.disabled) throw new Error(`Control unavailable: ${selector}`);
    element.click();
  };
  const focus = (element) => {
    if (params.has("review")) window.scrollTo({ top: Math.max(0, element.getBoundingClientRect().top + window.scrollY - 80), behavior: "instant" });
    else element.scrollIntoView({ block: "center" });
  };
  const edit = (value) => { input.value = value; input.dispatchEvent(new Event("input", { bubbles: true })); };
  return async (action) => {
    device.guided = true;
    switch (action) {
      case "none": break;
      case "discovery":
        if (device.scenario !== "no_devices") await wait(() => document.querySelector("#homedevices button") || ["stock", "manual"].includes(device.scenario));
        break;
      case "manage":
        if (device.scenario === "no_devices") break;
        await wait(() => document.querySelector("#homedevices button"));
        await delay(120); click("#homedevices button");
        await wait(() => !document.getElementById("device").hidden);
        if (device.scenario === "permission") {
          await wait(() => !document.querySelector("#home .ble-permission-help").hidden);
          document.querySelector("#home .ble-permission-help").open = true;
        }
        break;
      case "backup":
        await wait(() => !document.getElementById("dobackup").disabled);
        click("#dobackup"); break;
      case "jobOpen":
        await wait(() => !document.getElementById(`do${workflow}`).disabled);
        click(`#do${workflow}`); break;
      case "jobPrompt":
        if (workflow === "restore") file(document.querySelector('#actionbody input[type="file"]'), "mock-backup.bin", new Uint8Array(1024));
        else click("#actionbody button");
        await wait(() => document.getElementById("review-dialog")); break;
      case "jobStart":
        device.hold = "job";
        if (workflow === "restore") file(document.querySelector('#actionbody input[type="file"]'), "mock-backup.bin", new Uint8Array(1024));
        else click("#actionbody button");
        await wait(() => device.waiting === "job"); break;
      case "jobBusy":
        device.release?.(); break;
      case "jobFinish":
        device.release?.();
        await wait(() => document.getElementById("actionresult").textContent.length > 0); break;
      case "connect":
        await (device.scenario === "permission" ? actions.bluetooth() : actions.usb()); break;
      case "firmware":
        if (!document.getElementById("config").hidden) click("#back");
        await wait(() => !document.getElementById("device").hidden);
        if (!document.getElementById("doupgrade").disabled) click("#doupgrade");
        break;
      case "loading":
        device.hold = "fields"; pending = actions.usb();
        await wait(() => device.waiting === "fields"); break;
      case "ready":
        device.release?.(); await pending; break;
      case "help": {
        const base = document.querySelector('#settings > details[data-target="18"]');
        base.open = true;
        const row = base.querySelector('tr[data-key="18:7"]');
        row.querySelector("details").open = true;
        input = row.querySelector('input[type="number"]');
        focus(base); break;
      }
      case "future": {
        const row = document.querySelector('tr[data-key="0:254"]');
        row.closest("details[data-target]").open = true;
        row.querySelector("details").open = true; focus(row); break;
      }
      case "macros":
        focus(document.querySelector('#settings > details[data-target="18"] .macroedit')); break;
      case "packageUnload": await actions.unloadPackage(); break;
      case "packageLoad": {
        const bytes = params.get("fixture") === "invalid" ? new TextEncoder().encode("broken zip") : await mockPackage();
        await actions.loadPackage(new File([bytes], "review-package.zip")); break;
      }
      case "presetLoad": {
        const bytes = params.get("fixture") === "invalid" ? "{}" : JSON.stringify({ format: 1, name: "Review preset", map: "south=east", rules: "" });
        file(document.querySelector('#settings > details[data-target="18"] .remap input[type="file"]'), "review-preset.json", bytes);
        await wait(() => document.getElementById("review-dialog") || !document.getElementById("savebar").hidden); break;
      }
      case "macroError": {
        const root = document.querySelector('#settings > details[data-target="18"] .macroedit');
        root.querySelector('button').click();
        const editor = root.querySelector("textarea"); editor.value = "invalid macro text";
        editor.dispatchEvent(new Event("input", { bubbles: true })); break;
      }
      case "updatePrompt":
        await wait(() => document.getElementById("update") && !document.getElementById("update").disabled);
        await actions.update(); break;
      case "updateStart":
        await wait(() => document.getElementById("update") && !document.getElementById("update").disabled);
        device.hold = "update"; pending = actions.update();
        await wait(() => device.waiting === "update"); break;
      case "updateFinish":
        device.release?.(); await pending; if (document.getElementById("mock-notice")) focus(document.getElementById("mock-notice")); break;
      case "quickMacroError": {
        const root = document.querySelector('#settings > details[data-target="18"] .macroedit');
        root.querySelector("button").click();
        await wait(() => root.querySelector(".quick-macro-import:disabled"));
        break;
      }
      case "radioEdit": {
        const row = document.querySelector('tr[data-key="0:24"]');
        row.closest("details[data-target]").open = true;
        const select = row.querySelector("select"); select.value = "dongle"; select.dispatchEvent(new Event("change")); break;
      }
      case "savePrompt": await actions.save(); break;
      case "discardPrompt":
        click("#back"); await wait(() => document.getElementById("review-dialog")); break;
      case "invalid": edit(Number(input.max) + 1); break;
      case "edit": edit(6); break;
      case "saveStart":
        device.hold = "write"; pending = actions.save();
        await wait(() => device.waiting === "write"); break;
      case "saveFinish": device.release?.(); await pending; if (document.getElementById("mock-notice")) focus(document.getElementById("mock-notice")); break;
      case "disconnect":
        device.disconnect();
        await wait(() => !document.getElementById("home").hidden); break;
    }
  };
}
