// The remap and curve editors.  Each works on the text form of its fields
// (the inputs the settings table has for map, rules and curve points, in
// padtool's text) and writes back into them, so staging, defaults and
// saving stay the table's.
import { h, svg, setControl, nameMenu } from "./dom.js";
import {
  KEYS, TRIGGERS, TIMED, TURBO, MAX_HZ, RULE_SIZE, PRESETS, EFFECT_TWINS, effectsFor, rule, decodeMap, encodeMap,
  showMap, parseMap, decodeRules, encodeRules, showRule, showRules, parseRules, applyPreset, exportPreset, importPreset,
  CURVE_PRESETS, curvePreset, identifyCurve, parseCurve, MACRO_EFFECTS, MACROS, MACRO_NAME, MACRO_UNIT_MS,
  decodeMacros, encodeMacros, showMacros, parseMacros, macrosLength,
} from "./remap.js";

// The keys in the editor's order (padgui's).
const KEY_ORDER = ["south", "east", "west", "north", "up", "down", "left", "right", "l1", "r1", "l2", "r2", "l4", "r4",
  "l3", "r3", "minus", "plus", "home", "m", "capture"];
const TRIGGER_LABELS = { held: "is held", held_for: "is held for", pressed: "is pressed", released: "is released",
  tapped: "is tapped under", double_tapped: "is double-tapped within" };
const EFFECT_LABELS = { hold: "hold", turbo: "turbo", nothing: "nothing", tap: "tap", toggle_hold: "toggle hold",
  toggle_turbo: "toggle turbo", turbo_modifier: "turbo modifier", toggle_turbo_modifier: "toggle turbo modifier",
  macro: "play macro, again while held", play_macro: "play macro once", toggle_macro: "loop macro on/off" };

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

  // Which of the remap slot's macros a rule plays: by name, any number up
  // to MACROS (one not made yet plays nothing).
  function macroPicker(r) {
    const names = io.macroNames?.() ?? [];
    const options = Array.from({ length: Math.max(names.length, r.macro, 1) }, (_, i) =>
      [String(i + 1), i < names.length ? `${i + 1}: ${names[i] || "(no name)"}` : `${i + 1}: (none yet)`]);
    return select(options, String(r.macro), (v) => change(() => { r.macro = Number(v); }, false), { "aria-label": "macro" });
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
    if (MACRO_EFFECTS.includes(r.effect)) parts.push(macroPicker(r));
    else if (r.effect !== "nothing") parts.push(outputsPicker(r));
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
  root.redraw = () => { if (!writing) draw(); };   // the macros' names changed
  return root;
}

// ---- macros -------------------------------------------------------------------

const TAP_MS = 40;               // a new tap's press, long enough for any host to see
const ROOM = 4096;               // the editor's room: more than the pad's shows as too long, not as an error
const STEP_KINDS = [["press", "Press"], ["release", "Release"], ["pause", "Pause"]];
const kindOfStep = (s) => (s.key === null ? "pause" : s.press ? "press" : "release");

// The keys each macro holds down at its end: what the pad lets go of then
// (shown, so a missing release is no surprise).
function stuck(m) {
  const down = new Set();
  for (const s of m.steps) if (s.key) s.press ? down.add(s.key) : down.delete(s.key);
  return [...down];
}

// When each step happens, from the start: [{at, step}], and the whole run.
function timeline(m) {
  let t = 0;
  const at = m.steps.map((s) => (t += s.ms));
  return { at, total: t };
}

// A run as bars, a row per key, pressed from its press to its release (or
// the run's end): what the host sees.
function macroGraph(m, layout) {
  const { at, total } = timeline(m);
  const keys = [...new Set(m.steps.filter((s) => s.key).map((s) => s.key))];
  if (!keys.length) return null;
  const W = 300, ROW = 14, LABEL = 70, span = Math.max(total, 1), x = (t) => LABEL + (t / span) * (W - LABEL - 4);
  const g = svg("svg", { viewBox: `0 0 ${W} ${keys.length * ROW + 14}`, class: "macrograph", role: "img",
    "aria-label": `timeline, ${total} ms` });
  keys.forEach((k, row) => {
    const y = row * ROW;
    const label = svg("text", { x: 0, y: y + 10, class: "label" });
    label.textContent = keyLabel(layout, k);
    g.append(svg("line", { x1: LABEL, y1: y + ROW - 2, x2: W - 4, y2: y + ROW - 2, class: "track" }), label);
    let from = null;
    m.steps.forEach((s, i) => {
      if (s.key !== k) return;
      if (s.press && from === null) from = at[i];
      else if (!s.press && from !== null) {
        g.append(svg("rect", { x: x(from), y: y + 2, width: Math.max(x(at[i]) - x(from), 1.5), height: ROW - 5, class: "bar" }));
        from = null;
      }
    });
    if (from !== null) g.append(svg("rect", { x: x(from), y: y + 2, width: Math.max(x(total) - x(from), 1.5), height: ROW - 5, class: "bar open" }));
  });
  const end = svg("text", { x: W - 4, y: keys.length * ROW + 11, class: "label end" });
  end.textContent = `${total} ms`;
  g.append(end);
  return g;
}

// A remap slot's macros: a card each (name, steps, a timeline), what they
// take of the pad's room.  io: { input } the macros field's text input
// (padtool's text, which this writes), field (its size), saved and
// defaults (raw), inherited (none of its own: the base's are shown), base
// (this is the base's), layout, changed() after an edit, reset().
export function macroEditor(io) {
  let macros = [], writing = false;
  const list = h("div", { class: "macros" });
  const used = h("span", { class: "muted" });
  const add = h("button", { type: "button", class: "small" }, "Add macro");
  const reset = h("button", { type: "button", class: "small" }, io.base ? "Macros to default" : "Use the base's");
  const head = h("div", { class: "actions" }, h("strong", {}, "Macros"), used, add, reset);
  const note = h("p", { class: "muted" });
  const problem = h("p", { class: "error", hidden: true });
  const savedText = io.saved ? showMacros(io.saved) : null;

  const load = () => {
    try {
      macros = decodeMacros(parseMacros(io.input.value, ROOM));
      problem.hidden = true;
      return true;
    } catch (error) {
      problem.textContent = error.message;
      problem.hidden = false;
      return false;
    }
  };

  // The model into the text input; one the pad would refuse (too long)
  // stays as text, so the count says what is wrong.
  const write = () => {
    writing = true;
    try {
      let text;
      try {
        text = showMacros(encodeMacros(macros, io.field.size));
      } catch {
        text = showMacros(encodeMacros(macros, ROOM));
      }
      setControl(io.input, text);
    } catch (error) {
      problem.textContent = error.message;
      problem.hidden = false;
    } finally {
      writing = false;
    }
    io.changed?.();
  };

  const change = (fn, redraw = true) => {
    fn();
    write();
    if (redraw) draw();
    else counts();
  };

  function counts() {
    let n = null;
    try { n = macrosLength(macros); } catch { /* a step the pad would refuse: its error shows */ }
    used.textContent = `${macros.length}/${MACROS} · ${n ?? "?"} of ${io.field.size} bytes`;
    used.classList.toggle("error", n !== null && n > io.field.size);
    add.disabled = macros.length >= MACROS;
    head.classList.toggle("changed", savedText !== null && savedText !== io.input.value.trim());
    reset.hidden = io.base ? !io.defaults || showMacros(io.defaults) === io.input.value.trim() : io.inherited && !head.classList.contains("changed");
    note.textContent = io.inherited && !head.classList.contains("changed")
      ? "None of its own: this remap plays the base remap's macros. Changing them here makes them its own."
      : io.base ? "Every remap slot without macros of its own plays these." : "";
    note.hidden = !note.textContent;
  }

  const keyOptions = () => KEY_ORDER.map((k) => [k, keyLabel(io.layout, k)]);

  function stepRow(m, s, i) {
    const ms = h("input", { type: "number", min: 0, max: 60000, step: MACRO_UNIT_MS, class: "ms", "aria-label": "wait before, ms" });
    ms.value = String(s.ms);
    ms.addEventListener("change", () => change(() => { s.ms = Math.max(0, Math.round(Number(ms.value) || 0)); }));
    const kind = select(STEP_KINDS, kindOfStep(s), (v) => change(() => {
      if (v === "pause") s.key = null;
      else {
        s.key ??= m.steps.slice(0, i).reverse().find((x) => x.key)?.key ?? "south";
        s.press = v === "press";
      }
    }), { "aria-label": "step" });
    const parts = [h("span", { class: "muted" }, i ? "then after" : "after"), ms, h("span", { class: "muted" }, "ms"), kind];
    if (s.key !== null) parts.push(select(keyOptions(), s.key, (v) => change(() => { s.key = v; }), { "aria-label": "key" }));
    const up = h("button", { type: "button", class: "small", title: "earlier", disabled: !i }, "↑");
    up.addEventListener("click", () => change(() => { m.steps.splice(i - 1, 0, ...m.steps.splice(i, 1)); }));
    const down = h("button", { type: "button", class: "small", title: "later", disabled: i === m.steps.length - 1 }, "↓");
    down.addEventListener("click", () => change(() => { m.steps.splice(i + 1, 0, ...m.steps.splice(i, 1)); }));
    const del = h("button", { type: "button", class: "small", title: "delete this step" }, "×");
    del.addEventListener("click", () => change(() => { m.steps.splice(i, 1); }));
    return h("div", { class: "rule step" }, del, ...parts, h("span", { class: "grow" }), up, down);
  }

  function macroCard(m, n) {
    const name = h("input", { type: "text", maxlength: MACRO_NAME, placeholder: "name", spellcheck: "false",
      "aria-label": `macro ${n + 1} name` });
    name.value = m.name;
    name.addEventListener("input", () => {
      let v = name.value;
      while (new TextEncoder().encode(v).length > MACRO_NAME) v = v.slice(0, -1);
      if (v !== name.value) name.value = v;
      change(() => { m.name = v; }, false);
    });
    const del = h("button", { type: "button", class: "small", title: "delete this macro (rules playing a later one then play the one before it)" }, "Delete");
    del.addEventListener("click", () => {
      if (m.steps.length && !confirm(`Delete macro ${n + 1}${m.name ? ` "${m.name}"` : ""}?`)) return;
      change(() => { macros.splice(n, 1); });
    });
    const up = h("button", { type: "button", class: "small", title: "make it the macro before (rules play by number)", disabled: !n }, "↑");
    up.addEventListener("click", () => change(() => { macros.splice(n - 1, 0, ...macros.splice(n, 1)); }));
    // Adding: a tap of a key (its press and release), a press, a release, a pause.
    const key = select(keyOptions(), m.steps.findLast((s) => s.key)?.key ?? "south", () => {}, { "aria-label": "key to add" });
    const gap = () => (m.steps.length ? 48 : 0);   // 4 ms steps
    const tap = h("button", { type: "button", class: "small" }, "+ Tap");
    tap.addEventListener("click", () => change(() => {
      m.steps.push({ ms: gap(), key: key.value, press: true }, { ms: TAP_MS, key: key.value, press: false });
    }));
    const press = h("button", { type: "button", class: "small" }, "+ Press");
    press.addEventListener("click", () => change(() => { m.steps.push({ ms: gap(), key: key.value, press: true }); }));
    const release = h("button", { type: "button", class: "small" }, "+ Release");
    release.addEventListener("click", () => change(() => { m.steps.push({ ms: gap(), key: key.value, press: false }); }));
    const pause = h("button", { type: "button", class: "small", title: "a wait at the end: the gap before it can run again" }, "+ Pause");
    pause.addEventListener("click", () => change(() => { m.steps.push({ ms: 200, key: null, press: false }); }));
    const left = stuck(m);
    const { total } = timeline(m);
    return h("section", { class: "box macro" },
      h("div", { class: "actions" }, h("strong", {}, `Macro ${n + 1}`), name,
        h("span", { class: "muted" }, `${m.steps.length} step${m.steps.length === 1 ? "" : "s"}, ${total} ms`), up, del),
      macroGraph(m, io.layout),
      h("div", { class: "steps" }, m.steps.length ? m.steps.map((s, i) => stepRow(m, s, i))
        : h("p", { class: "muted" }, "No steps yet: add a tap below.")),
      left.length ? h("p", { class: "muted" }, `Still held at the end: ${left.map((k) => keyLabel(io.layout, k)).join(", ")} ` +
        "(let go when the run ends).") : null,
      h("div", { class: "actions add" }, key, tap, press, release, pause));
  }

  function draw() {
    if (!load()) return list.replaceChildren(), counts();
    list.replaceChildren(...macros.map(macroCard));
    counts();
  }

  add.addEventListener("click", () => change(() => {
    macros.push({ name: `Macro ${macros.length + 1}`, steps: [{ ms: 0, key: "south", press: true }, { ms: TAP_MS, key: "south", press: false }] });
  }));
  reset.addEventListener("click", () => io.reset());
  const reload = () => { if (!writing) draw(); };
  io.input.addEventListener("input", reload);
  io.input.addEventListener("change", reload);
  draw();
  return h("div", { class: "remap macroedit" }, head, note, problem, list,
    h("p", { class: "muted" }, "A rule above plays a macro by number: once, again while its key is held, or looping until ",
      "pressed again. While it plays, the keys it uses are its own. Waits go in 4 ms steps."));
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
