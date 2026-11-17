// Remap slots and stick curves as text and as objects: a port of
// tools/padtool/padremap.py and padcurves.py, same text, same bytes (the
// tests check them against vectors from the Python).
//
// Each key has a base row, the map: while held it sends itself, another key
// or nothing.  Rules add to it: "INPUT TRIGGER[:MS] EFFECT [OUT+OUT...][@HZ|@max]",
// ';'-separated.  The layout matches struct remap_rule (src/core/pad.h).

export class RemapError extends Error {}

// IN_* bit numbers; bit 15 is no key.
export const KEYS = ["south", "east", "capture", "west", "north", "m", "l1", "r1", "l4", "r4", "minus", "plus",
  "home", "l3", "r3", null, "up", "right", "down", "left", "l2", "r2"];
export const NAMES = KEYS.filter(Boolean);
const ALIASES = { select: "minus", start: "plus", screenshot: "capture" };
export const INPUTS = KEYS.length;
const OFF = 0xff;
export const RULES = 16, RULE_SIZE = 10, MAX_HZ = 30;

export const TRIGGERS = [null, "held", "held_for", "pressed", "released", "tapped", "double_tapped"];
export const EFFECTS = ["hold", "turbo", "nothing", "tap", "toggle_hold", "toggle_turbo",
  "turbo_modifier", "toggle_turbo_modifier", "macro", "play_macro", "toggle_macro"];
const LEVEL_TRIGGERS = ["held", "held_for"];
const LEVEL_EFFECTS = ["hold", "turbo", "turbo_modifier", "nothing", "macro"];
const EVENT_EFFECTS = ["tap", "toggle_hold", "toggle_turbo", "toggle_turbo_modifier", "play_macro", "toggle_macro"];
// They play one of the remap slot's macros: the rate byte is its number, 1 the first.
export const MACRO_EFFECTS = ["macro", "play_macro", "toggle_macro"];
export const MACROS = 8;
export const TIMED = { held_for: 300, tapped: 200, double_tapped: 300 };   // the editors' starting times
export const TURBO = ["turbo", "toggle_turbo", "turbo_modifier", "toggle_turbo_modifier"];
export const TURBO_HZ = 15;
// Switching between a level and an event trigger keeps the effect's kind.
export const EFFECT_TWINS = { hold: "tap", tap: "hold", toggle_hold: "hold", turbo: "toggle_turbo",
  toggle_turbo: "turbo", turbo_modifier: "toggle_turbo_modifier", toggle_turbo_modifier: "turbo_modifier", nothing: "tap",
  macro: "play_macro", play_macro: "macro", toggle_macro: "macro" };

export function keyBit(name) {
  const n = ALIASES[name.toLowerCase()] ?? name.toLowerCase();
  if (!NAMES.includes(n)) throw new RemapError(`no key ${JSON.stringify(name)}; one of ${NAMES.join(" ")}`);
  return KEYS.indexOf(n);
}

export const effectsFor = (trigger) => (LEVEL_TRIGGERS.includes(trigger) ? LEVEL_EFFECTS : EVENT_EFFECTS);

// ---- map -------------------------------------------------------------------

// {key: what it sends while held, null for nothing} for every key.
export function decodeMap(raw) {
  const out = {};
  KEYS.forEach((key, i) => {
    if (!key) return;
    const v = raw[i] ?? 0;
    out[key] = v === OFF ? null : v && v <= INPUTS && KEYS[v - 1] ? KEYS[v - 1] : key;
  });
  return out;
}

export function encodeMap(base) {
  const raw = new Uint8Array(INPUTS);
  for (const [key, to] of Object.entries(base)) raw[keyBit(key)] = to === null ? OFF : to === key ? 0 : keyBit(to) + 1;
  return raw;
}

export function showMap(raw) {
  const base = decodeMap(raw);
  return NAMES.filter((k) => base[k] !== k).map((k) => `${k}=${base[k] ?? "none"}`).join(" ") || "identity";
}

export function parseMap(text) {
  text = text.trim();
  if (PRESETS[text]) text = PRESETS[text][1];
  const base = Object.fromEntries(NAMES.map((k) => [k, k]));
  if (text && text !== "identity") {
    for (const part of text.split(/\s+/)) {
      const at = part.indexOf("=");
      if (at < 0) throw new RemapError(`map: ${JSON.stringify(part)} is not KEY=KEY`);
      const key = part.slice(0, at), to = part.slice(at + 1);
      base[KEYS[keyBit(key)]] = to.toLowerCase() === "none" ? null : KEYS[keyBit(to)];
    }
  }
  return encodeMap(base);
}

// ---- rules -----------------------------------------------------------------

// A rule, with the editors' starting time and rate filled in.
export function rule(input, trigger, effect, outputs = [], ms = undefined, rate = undefined, macro = undefined) {
  return { input, trigger, effect, outputs: [...outputs], ms: ms ?? TIMED[trigger] ?? null, rate: rate ?? TURBO_HZ,
    macro: macro ?? 1 };
}

export function checkRule(r) {
  keyBit(r.input);
  if (!TRIGGERS.slice(1).includes(r.trigger)) {
    throw new RemapError(`rule: no trigger ${JSON.stringify(r.trigger)}; one of ${TRIGGERS.slice(1).join(" ")}`);
  }
  if (!effectsFor(r.trigger).includes(r.effect)) {
    throw new RemapError(`rule: ${r.trigger} goes with ${effectsFor(r.trigger).join(", ")}, not ${r.effect}`);
  }
  r.outputs.forEach(keyBit);
  if (r.trigger in TIMED && !(r.ms >= 10 && r.ms <= 2550)) throw new RemapError(`rule: ${r.trigger} needs a time of 10-2550 ms`);
  if (TURBO.includes(r.effect) && !(Number.isInteger(r.rate) && r.rate >= 0 && r.rate <= MAX_HZ)) {
    throw new RemapError(`rule: turbo is 1-${MAX_HZ} Hz or max`);
  }
  if (MACRO_EFFECTS.includes(r.effect) && !(Number.isInteger(r.macro) && r.macro >= 1 && r.macro <= MACROS)) {
    throw new RemapError(`rule: a macro is 1-${MACROS}`);
  }
}

export function decodeRules(raw) {
  const out = [];
  for (let i = 0; i + RULE_SIZE <= raw.length; i += RULE_SIZE) {
    const [trigger, key, ms10, effect] = raw.subarray(i, i + 4);
    if (!trigger || trigger >= TRIGGERS.length || effect >= EFFECTS.length || key >= INPUTS || !KEYS[key]) continue;
    const mask = (raw[i + 4] | (raw[i + 5] << 8) | (raw[i + 6] << 16) | (raw[i + 7] << 24)) >>> 0;
    out.push(rule(KEYS[key], TRIGGERS[trigger], EFFECTS[effect], KEYS.filter((k, b) => k && (mask >>> b) & 1),
      TRIGGERS[trigger] in TIMED ? ms10 * 10 : undefined, TURBO.includes(EFFECTS[effect]) ? raw[i + 8] : undefined,
      MACRO_EFFECTS.includes(EFFECTS[effect]) ? raw[i + 8] : undefined));
  }
  return out;
}

// count: the rules the pad keeps, its field's size / RULE_SIZE.
export function encodeRules(rules, count = RULES) {
  if (rules.length > count) throw new RemapError(`at most ${count} rules`);
  const raw = new Uint8Array(count * RULE_SIZE);
  rules.forEach((r, n) => {
    checkRule(r);
    let mask = 0;
    if (r.effect !== "nothing" && !MACRO_EFFECTS.includes(r.effect)) for (const o of r.outputs) mask |= 1 << keyBit(o);
    const o = n * RULE_SIZE;
    raw.set([TRIGGERS.indexOf(r.trigger), keyBit(r.input), r.trigger in TIMED ? roundEven(r.ms / 10) : 0,
      EFFECTS.indexOf(r.effect), mask & 0xff, (mask >>> 8) & 0xff, (mask >>> 16) & 0xff, (mask >>> 24) & 0xff,
      TURBO.includes(r.effect) ? r.rate : MACRO_EFFECTS.includes(r.effect) ? r.macro : 0], o);
  });
  return raw;
}

export function showRule(r) {
  let text = `${r.input} ${r.trigger}`;
  if (r.trigger in TIMED) text += `:${r.ms}`;
  text += ` ${r.effect}`;
  if (MACRO_EFFECTS.includes(r.effect)) return `${text} ${r.macro}`;
  if (r.effect !== "nothing") text += " " + (r.outputs.join("+") || "-");
  if (TURBO.includes(r.effect)) text += "@" + (r.rate ? r.rate : "max");
  return text;
}

export const showRules = (raw) => decodeRules(raw).map(showRule).join("; ") || "none";

const intOf = (s, what) => {
  if (!/^-?\d+$/.test(s)) throw new RemapError(`rule: ${what} ${JSON.stringify(s)} is not a number`);
  return Number(s);
};

export function parseRule(text) {
  const words = text.trim().split(/\s+/);
  if (words.length < 3) throw new RemapError(`rule ${JSON.stringify(text.trim())}: INPUT TRIGGER[:MS] EFFECT [OUT+OUT][@HZ|@max]`);
  const [trigger, ms] = words[1].split(/:(.*)/s);
  let outputs = [], rate, macro;
  if (MACRO_EFFECTS.includes(words[2]) && words.length > 3) {
    macro = intOf(words[3], "macro");
  } else if (words.length > 3) {
    const [out, hz] = words[3].split(/@(.*)/s);
    outputs = out.split("+").filter((o) => o && o !== "-");
    if (hz) rate = hz === "max" ? 0 : intOf(hz, "rate");
  }
  const alias = (k) => ALIASES[k.toLowerCase()] ?? k.toLowerCase();
  const r = rule(alias(words[0]), trigger, words[2], outputs.map(alias), ms ? intOf(ms, "time") : undefined, rate, macro);
  checkRule(r);
  return r;
}

export function parseRules(text, count = RULES) {
  text = text.trim();
  if (!text || text === "none") return encodeRules([], count);
  return encodeRules(text.split(";").filter((p) => p.trim()).map(parseRule), count);
}

// ---- macros -------------------------------------------------------------------
//
// A remap slot's macros (padremap.py's text, pad.h's bytes): ';'-separated
// "NAME: STEP ...", a step +KEY (press), -KEY (release) or the milliseconds
// before the next (4 ms steps; one at the end is the pause before the macro
// can run again).  As objects: {name, steps: [{ms, key, press}]}, ms the wait
// before the step; a step with key null is that pause.

export const MACRO_UNIT_MS = 4, MACRO_NAME = 16, MACRO_BYTES = 200;
const MACRO_PRESS = 0x20, MACRO_WAIT_ONLY = 0xff;
const utf8 = (s) => new TextEncoder().encode(s);

export function decodeMacros(raw) {
  const out = [];
  for (let o = 0; o < raw.length && raw[o]; o += 1 + raw[o]) {
    const end = Math.min(o + 1 + raw[o], raw.length), n = raw[o + 1] ?? 0;
    const name = new TextDecoder().decode(raw.subarray(o + 2, o + 2 + n));
    const steps = [];
    let wait = 0;
    for (let i = o + 2 + n; i + 1 < end; i += 2) {
      wait += raw[i] * MACRO_UNIT_MS;
      const a = raw[i + 1], key = KEYS[a & 0x1f];
      if (a === MACRO_WAIT_ONLY || (a & ~MACRO_PRESS & 0xff) >= INPUTS || !key) continue;
      steps.push({ ms: wait, key, press: !!(a & MACRO_PRESS) });
      wait = 0;
    }
    if (wait) steps.push({ ms: wait, key: null, press: false });
    out.push({ name, steps });
  }
  return out;
}

// Each macro's bytes: {length, name length, name, steps}.
function macroBytes(m) {
  const name = utf8(m.name);
  if (name.length > MACRO_NAME) throw new RemapError(`macro ${JSON.stringify(m.name)}: a name is at most ${MACRO_NAME} bytes`);
  const body = [];
  for (const s of m.steps) {
    if (!(Number.isInteger(s.ms) && s.ms >= 0)) throw new RemapError(`macro ${JSON.stringify(m.name)}: a wait is a number of ms`);
    let units = Math.floor((s.ms + MACRO_UNIT_MS / 2) / MACRO_UNIT_MS);
    for (; units > 0xff; units -= 0xff) body.push(0xff, MACRO_WAIT_ONLY);
    if (s.key === null) {
      if (units) body.push(units, MACRO_WAIT_ONLY);
    } else {
      body.push(units, keyBit(s.key) | (s.press ? MACRO_PRESS : 0));
    }
  }
  if (!body.length) throw new RemapError(`macro ${JSON.stringify(m.name)} has no steps`);
  if (1 + name.length + body.length > 0xff) throw new RemapError(`macro ${JSON.stringify(m.name)} is too long`);
  return [1 + name.length + body.length, name.length, ...name, ...body];
}

// The bytes the macros take, for the editor's count.
export const macrosLength = (macros) => macros.reduce((n, m) => n + macroBytes(m).length, 0);

export function encodeMacros(macros, size = MACRO_BYTES) {
  if (macros.length > MACROS) throw new RemapError(`at most ${MACROS} macros`);
  const bytes = macros.flatMap(macroBytes);
  if (bytes.length > size) throw new RemapError(`the macros take ${bytes.length} bytes; the pad has ${size}`);
  const raw = new Uint8Array(size);
  raw.set(bytes);
  return raw;
}

export const showMacros = (raw) => decodeMacros(raw).map((m) => `${m.name}: ` + m.steps.map((s) =>
  [s.ms ? String(s.ms) : null, s.key ? (s.press ? "+" : "-") + s.key : null].filter(Boolean).join(" ")).join(" ")).join("; ") || "none";

export function parseMacros(text, size = MACRO_BYTES) {
  text = text.trim();
  if (!text || text === "none") return encodeMacros([], size);
  return encodeMacros(text.split(";").filter((p) => p.trim()).map((part) => {
    const at = part.indexOf(":");
    if (at < 0) throw new RemapError(`macro ${JSON.stringify(part.trim())}: NAME: STEP STEP ...`);
    const name = part.slice(0, at).trim(), steps = [];
    let wait = 0;
    for (const word of part.slice(at + 1).trim().split(/\s+/).filter(Boolean)) {
      if (/^\d+$/.test(word)) wait += Number(word);
      else if (word[0] === "+" || word[0] === "-") {
        steps.push({ ms: wait, key: KEYS[keyBit(word.slice(1))], press: word[0] === "+" });
        wait = 0;
      } else throw new RemapError(`macro ${JSON.stringify(name)}: ${JSON.stringify(word)} is not +KEY, -KEY or a number of ms`);
    }
    if (wait) steps.push({ ms: wait, key: null, press: false });
    return { name, steps };
  }), size);
}

// ---- presets -----------------------------------------------------------------

// name: [label, map text].  They set base rows only.
export const PRESETS = {
  "swap-faces": ["Swap faces (south/east, west/north)", "south=east east=south west=north north=west"],
  "swap-south-east": ["Swap south/east", "south=east east=south"],
  "swap-west-north": ["Swap west/north", "west=north north=west"],
  "capture-off": ["Capture off", "capture=none"],
};

// base with the preset's keys set; other keys as they were.
export function applyPreset(base, name) {
  const preset = decodeMap(parseMap(PRESETS[name][1]));
  return Object.fromEntries(Object.keys(base).map((k) => [k, preset[k] !== k ? preset[k] : base[k]]));
}

// Preset files (padgui's Import/Export preset): a remap slot in the CLI's text.
export const FILE_FORMAT = 1;

export const exportPreset = (name, mapRaw, rulesRaw) =>
  ({ format: FILE_FORMAT, name, map: showMap(mapRaw), rules: showRules(rulesRaw) });

export function importPreset(data, count = RULES) {
  if (!data || typeof data !== "object" || Array.isArray(data) || data.format !== FILE_FORMAT) {
    throw new RemapError("not a remap preset file (format 1)");
  }
  try {
    return { name: String(data.name ?? ""), map: parseMap(String(data.map ?? "")), rules: parseRules(String(data.rules ?? ""), count) };
  } catch (error) {
    throw new RemapError(`preset file: ${error.message}`);
  }
}

// ---- stick curves ------------------------------------------------------------

// Python's round(): halves to even, so the presets match padcurves.py.
function roundEven(x) {
  const r = Math.round(x);
  return Math.abs(x % 1) === 0.5 && r % 2 ? r - 1 : r;
}

export const POINT_COUNT = 15;
export const CURVE_PRESETS = ["linear", "concave", "s-curve"];

export function validateCurve(points, count = POINT_COUNT) {
  const p = Uint8Array.from(points);
  if (p.length !== count || points.some((v) => !(Number.isInteger(v) && v >= 0 && v <= 255)) ||
      p.some((v, i) => i && p[i - 1] > v)) {
    throw new RemapError(`a curve needs ${count} nondecreasing output values (0-255)`);
  }
  return p;
}

// Outputs at inputs 1/16 ... 15/16 of the way out.
export function curvePreset(name, count = POINT_COUNT) {
  if (!CURVE_PRESETS.includes(name)) throw new RemapError(`unknown curve preset: ${name}`);
  return validateCurve(Array.from({ length: count }, (_, i) => {
    const x = (i + 1) / (count + 1);
    const y = name === "linear" ? x : name === "concave" ? x * x : x <= 0.5 ? 2 * x * (1 - x) : 1 - 2 * (1 - x) * x;
    return roundEven(y * 255);
  }), count);
}

export const identifyCurve = (points) =>
  CURVE_PRESETS.find((n) => curvePreset(n, points.length).every((v, i) => v === points[i])) ?? "custom";

// Curve points from text: a preset's name or N,N,... (padtool's form).
export function parseCurve(text, count = POINT_COUNT) {
  text = text.trim();
  if (CURVE_PRESETS.includes(text)) return curvePreset(text, count);
  return validateCurve(text.split(/[\s,]+/).filter(Boolean).map(Number), count);
}
