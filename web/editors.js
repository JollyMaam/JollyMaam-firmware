// The remap and curve editors.  Each works on the text form of its fields
// (the inputs the settings table has for map, rules and curve points, in
// padtool's text) and writes back into them, so staging, defaults and
// saving stay the table's.
import { h, svg, setControl, nameMenu } from "./dom.js";
import {
  KEYS, TRIGGERS, TIMED, TURBO, MAX_HZ, RULE_SIZE, PRESETS, EFFECT_TWINS, effectsFor, rule, decodeMap, encodeMap,
  showMap, parseMap, decodeRules, encodeRules, showRule, showRules, parseRules, applyPreset, exportPreset, importPreset,
  CURVE_PRESETS, curvePreset, identifyCurve, parseCurve,
} from "./remap.js";

// The keys in the editor's order (padgui's).
const KEY_ORDER = ["south", "east", "west", "north", "up", "down", "left", "right", "l1", "r1", "l2", "r2", "l4", "r4",
  "l3", "r3", "minus", "plus", "home", "m", "capture"];
const TRIGGER_LABELS = { held: "is held", held_for: "is held for", pressed: "is pressed", released: "is released",
  tapped: "is tapped under", double_tapped: "is double-tapped within" };
const EFFECT_LABELS = { hold: "hold", turbo: "turbo", nothing: "nothing", tap: "tap", toggle_hold: "toggle hold",
  toggle_turbo: "toggle turbo", turbo_modifier: "turbo modifier", toggle_turbo_modifier: "toggle turbo modifier" };

const SHORT = ["m", "l1", "r1", "l2", "r2", "l4", "r4", "l3", "r3"];
const role = (k) => (SHORT.includes(k) ? k.toUpperCase() : k[0].toUpperCase() + k.slice(1));

// A key as the pad prints it, with its role when that differs: "B (south)",
// "LB (L1)", "Up".  layout: Pad.layout().
export function keyLabel(layout, k) {
  const printed = layout?.find((e) => e.input === KEYS.indexOf(k) && e.label)?.label;
  return printed && printed.toLowerCase() !== role(k).toLowerCase() ? `${printed} (${SHORT.includes(k) ? role(k) : k})` : role(k);
}

function select(options, value, onChange, attrs = {}) {
  const e = h("select", attrs, options.map(([v, label]) => h("option", { value: v }, label)));
  e.value = value;
  e.addEventListener("change", () => onChange(e.value));
  return e;
}

function download(name, text) {
  const a = h("a", { href: URL.createObjectURL(new Blob([text], { type: "application/json" })), download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// ---- remap ------------------------------------------------------------------

// A remap slot: per key what it sends while held (the map), then the rules,
// one list under it with a count of what is used.
// io: { map, rules, name } inputs (text), { mapField, rulesField },
// saved/defaults raw values, layout, file name, resetAll().
export function remapEditor(io) {
  const count = io.rulesField.size / RULE_SIZE;
  const decode = (raw) => (raw ? { base: decodeMap(raw.map), rules: decodeRules(raw.rules) } : null);
  const saved = decode(io.saved), def = decode(io.defaults);
  let base, rules, writing = false;
  const body = h("tbody");
  const ruleList = h("div", { class: "rules" });
  const used = h("span", { class: "muted" });
  const addRule = h("button", { type: "button", class: "small" }, "Add rule");
  const resetRules = h("button", { type: "button", class: "small", title: "the rules back to the default" }, "Rules to default");
  const rulesHead = h("div", { class: "actions" }, h("strong", {}, "Rules"), used, addRule, resetRules);
  const problem = h("p", { class: "error", hidden: true });

  const load = () => {
    try {
      base = decodeMap(parseMap(io.map.value));
      rules = decodeRules(parseRules(io.rules.value, count));
      problem.hidden = true;
      return true;
    } catch (error) {
      problem.textContent = `${error.message}: use "All to default" to start again`;
      problem.hidden = false;
      return false;
    }
  };

  // The model into the text inputs; a rule the pad would refuse stays as
  // text, so the list shows what is wrong.
  const write = () => {
    writing = true;
    try {
      setControl(io.map, showMap(encodeMap(base)));
      let text;
      try {
        text = showRules(encodeRules(rules, count));
      } catch {
        text = rules.map(showRule).join("; ");
      }
      setControl(io.rules, text);
    } finally {
      writing = false;
    }
  };

  const change = (fn, redraw = true) => {
    fn();
    write();
    if (redraw) draw();
    else marks();
  };

  const rulesText = (m) => m.rules.map(showRule).join("; ");
  const markCells = new Map();
  function marks() {
    for (const [k, { tr, reset }] of markCells) {
      tr.classList.toggle("changed", !!saved && saved.base[k] !== base[k]);
      reset.hidden = !def || def.base[k] === base[k];
    }
    rulesHead.classList.toggle("changed", !!saved && rulesText(saved) !== rulesText({ rules }));
    resetRules.hidden = !def || rulesText(def) === rulesText({ rules });
    used.textContent = `${rules.length}/${count} used`;
    addRule.disabled = rules.length >= count;
  }

  const keyOptions = () => [...KEY_ORDER.map((k) => [k, keyLabel(io.layout, k)]), ["", "Nothing"]];

  function outputsPicker(r) {
    const summary = h("summary", {});
    const show = () => { summary.textContent = r.outputs.map((k) => keyLabel(io.layout, k)).join(" + ") || "Choose keys…"; };
    show();
    const boxes = KEY_ORDER.map((k) => {
      const box = h("input", { type: "checkbox" });
      box.checked = r.outputs.includes(k);
      box.addEventListener("change", () => change(() => {
        r.outputs = KEY_ORDER.filter((x) => (x === k ? box.checked : r.outputs.includes(x)));
        show();
      }, false));
      return h("label", {}, box, " ", keyLabel(io.layout, k));
    });
    return h("details", { class: "keys" }, summary, h("div", { class: "keylist" }, boxes));
  }

  // A rule: × When [KEY] [is held…] [time ms] [effect] [keys] at [rate] Hz.
  function ruleEditor(r) {
    const remove = h("button", { type: "button", class: "small", title: "delete this rule" }, "×");
    remove.addEventListener("click", () => change(() => { rules = rules.filter((x) => x !== r); }));
    const parts = [remove, h("span", {}, "When"),
      select(KEY_ORDER.map((k) => [k, keyLabel(io.layout, k)]), r.input, (v) => change(() => { r.input = v; }, false),
        { "aria-label": "input" }),
      select(TRIGGERS.slice(1).map((t) => [t, TRIGGER_LABELS[t]]), r.trigger, (v) => change(() => {
        r.trigger = v;
        const allowed = effectsFor(v);
        if (!allowed.includes(r.effect)) r.effect = allowed.includes(EFFECT_TWINS[r.effect]) ? EFFECT_TWINS[r.effect] : allowed[0];
        if (v in TIMED && !r.ms) r.ms = TIMED[v];
      }), { "aria-label": "trigger" })];
    if (r.trigger in TIMED) {
      const ms = h("input", { type: "number", min: 10, max: 2550, step: 10, class: "ms", "aria-label": "milliseconds" });
      ms.value = String(r.ms);
      ms.addEventListener("change", () => change(() => { r.ms = Number(ms.value); }, false));
      parts.push(ms, h("span", {}, "ms"));
    }
    // Nothing only where a rule has it: mapping the key to Nothing says it.
    const effects = effectsFor(r.trigger).filter((e) => e !== "nothing" || r.effect === "nothing");
    parts.push(select(effects.map((e) => [e, EFFECT_LABELS[e]]), r.effect,
      (v) => change(() => { r.effect = v; }), { "aria-label": "effect" }));
    if (r.effect !== "nothing") parts.push(outputsPicker(r));
    if (TURBO.includes(r.effect)) {
      parts.push(h("span", {}, "at"), select([...Array.from({ length: MAX_HZ }, (_, i) => [String(i + 1), String(i + 1)]), ["0", "max"]],
        String(r.rate), (v) => change(() => { r.rate = Number(v); }), { "aria-label": "rate" }),
        h("span", {}, r.rate ? "Hz" : "(every report)"));
    }
    return h("div", { class: "rule" }, ...parts);
  }

  function keyRow(k) {
    const sends = select(keyOptions(), base[k] ?? "", (v) => change(() => { base[k] = v || null; }, false),
      { "aria-label": `${keyLabel(io.layout, k)} maps to` });
    const reset = h("button", { type: "button", class: "small" }, "Back to default");
    reset.addEventListener("click", () => change(() => { base[k] = def.base[k]; }));
    const menu = nameMenu(keyLabel(io.layout, k), h("p", { class: "muted" }, "What this key sends while held."), reset);
    const tr = h("tr", { class: "key" }, h("th", { scope: "row" }, menu), h("td", {}, sends));
    markCells.set(k, { tr, reset });
    return tr;
  }

  // The pad's keys (from its layout), and any other key mapped elsewhere.
  const shownKeys = () => {
    const has = io.layout ? new Set(io.layout.map((e) => KEYS[e.input]).filter(Boolean)) : null;
    return KEY_ORDER.filter((k) => !has || has.has(k) || base[k] !== k);
  };
  // A new rule's key: the first the pad has that no rule uses yet.
  const freeKey = () => shownKeys().find((k) => !rules.some((r) => r.input === k)) ?? KEY_ORDER[0];

  addRule.addEventListener("click", () => change(() => {
    const k = freeKey();
    rules.push(rule(k, "held", "hold", [k]));
  }));
  resetRules.addEventListener("click", () => change(() => { rules = def.rules.map((r) => ({ ...r, outputs: [...r.outputs] })); }));

  function draw() {
    markCells.clear();
    if (!load()) {
      ruleList.replaceChildren();
      return body.replaceChildren();
    }
    body.replaceChildren(...shownKeys().map(keyRow));
    ruleList.replaceChildren(...rules.map(ruleEditor));
    marks();
  }

  const reload = () => { if (!writing) draw(); };
  for (const input of [io.map, io.rules]) {
    input.addEventListener("input", reload);
    input.addEventListener("change", reload);
  }

  const presets = select([["", "Preset…"], ...Object.entries(PRESETS).map(([n, [label]]) => [n, label])], "", (v) => {
    if (!v || !base) return;
    change(() => { base = applyPreset(base, v); });
    presets.value = "";
  }, { "aria-label": "mapping preset" });
  const exportButton = h("button", { type: "button", class: "small" }, "Export preset…");
  exportButton.addEventListener("click", () => {
    try {
      download(`${io.file}.json`, JSON.stringify(exportPreset(io.name?.value ?? "", encodeMap(base), encodeRules(rules, count)), null, 1) + "\n");
    } catch (error) {
      alert(`Fix the rules first: ${error.message}`);
    }
  });
  const file = h("input", { type: "file", accept: ".json,application/json", hidden: true });
  file.addEventListener("change", async () => {
    const f = file.files[0];
    file.value = "";
    if (!f) return;
    try {
      const got = importPreset(JSON.parse(await f.text()), count);
      if (io.name) setControl(io.name, got.name);
      change(() => { base = decodeMap(got.map); rules = decodeRules(got.rules); });
    } catch (error) {
      alert(`${f.name}: ${error.message}`);
    }
  });
  const importButton = h("button", { type: "button", class: "small" }, "Import preset…");
  importButton.addEventListener("click", () => file.click());
  const all = h("button", { type: "button", class: "small", title: "the whole mapping and every rule back to the default" }, "All to default");
  all.addEventListener("click", () => io.resetAll());

  draw();
  const root = h("div", { class: "remap" },
    h("div", { class: "actions" }, presets, importButton, exportButton, all, file),
    h("p", { class: "muted" }, "A key sends itself unless changed; map it to Nothing to drop its own press. ",
      "Rules add to what the keys send. A trigger mapped to a trigger keeps its analog value."),
    problem,
    h("table", { class: "keys" }, h("colgroup", {}, h("col", { class: "c-key" }), h("col")),
      body),
    rulesHead, ruleList);
  // An open keys list closes on a click elsewhere.
  root.addEventListener("pointerdown", (e) => {
    for (const d of root.querySelectorAll("details.keys[open]")) if (!d.contains(e.target)) d.open = false;
  });
  return root;
}

// ---- curves -----------------------------------------------------------------

// The points with these set ({index: value}), the others pushed out of the
// way so the curve never goes down; the last one set wins.
export function placePoints(start, set, last) {
  const p = Uint8Array.from(start);
  for (const [i, v] of set) p[i] = Math.max(0, Math.min(255, Math.round(v)));
  for (let j = last + 1; j < p.length; j++) p[j] = Math.max(p[j], p[j - 1]);
  for (let j = last - 1; j >= 0; j--) p[j] = Math.min(p[j], p[j + 1]);
  return p;
}

// A stick curve: a preset, the graph and the points as text.  On the graph
// the points under the pointer follow it: press and move up or down for one
// point, sweep sideways to draw; the others make way.  input: the points'
// text input; enabled(): the curve's on/off select, which follows the points
// (linear is off: the calibrated input exactly).
export function curveEditor(input, count, enabled) {
  const W = 100, PAD = 4;
  let points = null;
  const graph = svg("svg", { viewBox: `${-PAD} ${-PAD} ${W + 2 * PAD} ${W + 2 * PAD}`, class: "curve", role: "img",
    "aria-label": "stick curve: drag to draw" });
  const preset = select([...CURVE_PRESETS.map((n) => [n, n]), ["custom", "custom"]], "custom", (v) => {
    if (v !== "custom") set(curvePreset(v, count));
  }, { "aria-label": "curve preset" });
  preset.querySelector('option[value="custom"]').disabled = true;

  const set = (p) => {
    setControl(input, Array.from(p).join(","));
    const e = enabled();
    if (e) setControl(e, identifyCurve(p) === "linear" ? "disabled" : "enabled", "change");
  };

  const xy = (i, v) => [((i + 1) / (count + 1)) * W, W - (v / 255) * W];
  const line = svg("polyline", { class: "line" });
  const grid = [1, 2, 3].map((k) => [svg("line", { x1: 0, y1: (k * W) / 4, x2: W, y2: (k * W) / 4, class: "grid" }),
    svg("line", { x1: (k * W) / 4, y1: 0, x2: (k * W) / 4, y2: W, class: "grid" })]).flat();
  const dots = Array.from({ length: count }, (_, i) => {
    const dot = svg("circle", { r: 2.4, class: "dot", tabindex: 0 });
    dot.addEventListener("keydown", (e) => {
      const step = { ArrowUp: 1, ArrowDown: -1, PageUp: 16, PageDown: -16, Home: -255, End: 255 }[e.key];
      if (!step || !points) return;
      e.preventDefault();
      set(placePoints(points, [[i, points[i] + step]], i));
    });
    return dot;
  });
  graph.append(svg("rect", { x: 0, y: 0, width: W, height: W, class: "frame" }), ...grid,
    svg("line", { x1: 0, y1: W, x2: W, y2: 0, class: "diagonal" }), line, ...dots);

  // The graph from the text (in place, so a drag keeps its pointer).
  function draw() {
    try {
      points = parseCurve(input.value, count);
    } catch {
      points = null;
    }
    preset.value = points ? identifyCurve(points) : "custom";
    graph.classList.toggle("invalid", !points);
    if (!points) return;
    line.setAttribute("points", [[0, W], ...Array.from(points, (v, i) => xy(i, v)), [W, 0]].map((p) => p.join(",")).join(" "));
    points.forEach((v, i) => {
      const [x, y] = xy(i, v);
      dots[i].setAttribute("cx", x);
      dots[i].setAttribute("cy", y);
      dots[i].setAttribute("aria-label", `point ${i + 1}: ${v}`);
    });
  }

  // Pointer to (point index, value).
  const at = (ev) => {
    const box = graph.getBoundingClientRect(), scale = box.width / (W + 2 * PAD);
    const x = (ev.clientX - box.left) / scale - PAD, y = (ev.clientY - box.top) / scale - PAD;
    const i = Math.max(0, Math.min(count - 1, Math.round((x / W) * (count + 1)) - 1));
    return [i, (1 - y / W) * 255];
  };

  graph.addEventListener("pointerdown", (e) => {
    if (!points || e.button > 0) return;
    e.preventDefault();
    try { graph.setPointerCapture(e.pointerId); } catch { /* a synthetic pointer */ }
    const start = points, touched = new Map();
    let last = null;
    const follow = (ev) => {
      const [i, v] = at(ev);
      // Sweeping across points sets every one passed, along the way.
      if (last !== null && Math.abs(i - last[0]) > 1) {
        const [j, w] = last, step = i > j ? 1 : -1;
        for (let k = j + step; k !== i; k += step) touched.set(k, w + ((v - w) * (k - j)) / (i - j));
      }
      touched.delete(i);
      touched.set(i, v);
      last = [i, v];
      set(placePoints(start, touched, i));
    };
    const end = () => {
      graph.removeEventListener("pointermove", follow);
      graph.removeEventListener("pointerup", end);
      graph.removeEventListener("pointercancel", end);
    };
    follow(e);
    graph.addEventListener("pointermove", follow);
    graph.addEventListener("pointerup", end);
    graph.addEventListener("pointercancel", end);
  });

  input.addEventListener("input", draw);
  input.addEventListener("change", draw);
  draw();
  return h("div", { class: "curvebox" }, graph,
    h("div", { class: "curveside" }, h("label", {}, "Preset ", preset),
      h("p", { class: "muted" }, "Drag on the graph: up or down moves a point, sideways draws through the points. " +
        "Arrow keys move a selected point."),
      h("label", { class: "muted" }, "Points (output 0–255 at 1/16 … 15/16 of the travel)"), input));
}
