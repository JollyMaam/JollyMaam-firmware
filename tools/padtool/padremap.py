"""Remap slots as text, for the CLI and the GUI.

Each key has a base row: while held it sends itself (default), another key or
nothing.  That is the map, which takes no rule entries.  Rules add to it.

Map: space-separated KEY=KEY or KEY=none for keys that differ from
themselves; "identity" when none do.

Rules: ';'-separated, each "INPUT TRIGGER[:MS] EFFECT [OUT+OUT...][@HZ|@max]",
e.g. "r4 held hold l1+r1; south held turbo south@max; r4 tapped:300 tap south".  A turbo
modifier's keys are the keys pressed: "r4 held turbo_modifier south+east@10" pulses
whatever south and east send while R4 is held.
"none" when there are none.  The layout matches struct remap_rule (src/core/pad.h).
A rule can play one of the remap slot's macros by number: "l4 pressed play_macro 2",
"r4 held macro 1" (again while held), "l3 pressed toggle_macro 3" (loops).

Macros (a remap slot's macros target): ';'-separated, each "NAME: STEP STEP ...", a
step +KEY (press), -KEY (release) or a number, the milliseconds before the next
(4 ms steps; one at the end is the pause before the macro may run again), e.g.
"jump: +south 40 -south; dash: +right 100 +east 40 -east -right".  "none" when
there are none.  The bytes match pad.h (REMAP_MACRO_BYTES).
"""

# PAD_* bit numbers; bit 15 is no key.
KEYS = ("south", "east", "capture", "west", "north", "m", "l1", "r1", "l4", "r4", "minus", "plus",
        "home", "l3", "r3", None, "up", "right", "down", "left", "l2", "r2")
NAMES = tuple(k for k in KEYS if k)
ALIASES = {"select": "minus", "start": "plus", "screenshot": "capture"}
INPUTS = len(KEYS)
OFF = 0xFF
RULES = 16
RULE_SIZE = 10
MAX_HZ = 30

TRIGGERS = (None, "held", "held_for", "pressed", "released", "tapped", "double_tapped")
EFFECTS = ("hold", "turbo", "nothing", "tap", "toggle_hold", "toggle_turbo",
           "turbo_modifier", "toggle_turbo_modifier", "macro", "play_macro", "toggle_macro")
LEVEL_TRIGGERS = ("held", "held_for")
LEVEL_EFFECTS = ("hold", "turbo", "turbo_modifier", "nothing", "macro")
EVENT_EFFECTS = ("tap", "toggle_hold", "toggle_turbo", "toggle_turbo_modifier", "play_macro", "toggle_macro")
MACRO_EFFECTS = ("macro", "play_macro", "toggle_macro")   # the rate byte: the macro's number, 1 the first
MACROS = 8
TIMED = {"held_for": 300, "tapped": 200, "double_tapped": 300}   # the GUI's starting times
MODIFIERS = ("turbo_modifier", "toggle_turbo_modifier")   # its keys are inputs: what they send pulses
TURBO = ("turbo", "toggle_turbo") + MODIFIERS
TURBO_HZ = 15


class RemapError(ValueError):
    """A map, rule or name the firmware would refuse; the message says why."""


def key_bit(name):
    name = ALIASES.get(name.lower(), name.lower())
    if name not in NAMES:
        raise RemapError(f"no key {name!r}; one of {' '.join(NAMES)}")
    return KEYS.index(name)


def effects_for(trigger):
    return LEVEL_EFFECTS if trigger in LEVEL_TRIGGERS else EVENT_EFFECTS


# ---- map --------------------------------------------------------------------

def decode_map(raw):
    """{key: what it sends while held, None for nothing} for every key."""
    raw = bytes(raw)
    out = {}
    for i, key in enumerate(KEYS):
        v = raw[i] if i < len(raw) else 0
        if key:
            out[key] = None if v == OFF else KEYS[v - 1] if v <= INPUTS and v and KEYS[v - 1] else key
    return out


def encode_map(base):
    raw = bytearray(INPUTS)
    for key, to in base.items():
        raw[key_bit(key)] = OFF if to is None else 0 if to == key else key_bit(to) + 1
    return bytes(raw)


def show_map(raw):
    base = decode_map(raw)
    return " ".join(f"{k}={base[k] or 'none'}" for k in NAMES if base[k] != k) or "identity"


def parse_map(text):
    if text.strip() in PRESETS:
        text = PRESETS[text.strip()][1]
    base = {k: k for k in NAMES}
    if text.strip() not in ("", "identity"):
        for part in text.split():
            if "=" not in part:
                raise RemapError(f"map: {part!r} is not KEY=KEY")
            key, to = part.split("=", 1)
            base[KEYS[key_bit(key)]] = None if to.lower() == "none" else KEYS[key_bit(to)]
    return encode_map(base)


# ---- rules ------------------------------------------------------------------

def rule(input, trigger, effect, outputs=(), ms=None, rate=None, macro=None):
    """A rule as a dict, with the GUI's starting time, rate and macro filled in."""
    return {"input": input, "trigger": trigger, "effect": effect, "outputs": list(outputs),
            "ms": TIMED.get(trigger) if ms is None else ms,
            "rate": TURBO_HZ if rate is None else rate, "macro": 1 if macro is None else macro}


def check_rule(r):
    key_bit(r["input"])
    if r["trigger"] not in TRIGGERS[1:]:
        raise RemapError(f"rule: no trigger {r['trigger']!r}; one of {' '.join(TRIGGERS[1:])}")
    if r["effect"] not in effects_for(r["trigger"]):
        raise RemapError(f"rule: {r['trigger']} goes with {', '.join(effects_for(r['trigger']))}, not {r['effect']}")
    for o in r["outputs"]:
        key_bit(o)
    if r["trigger"] in TIMED and not 10 <= (r["ms"] or 0) <= 2550:
        raise RemapError(f"rule: {r['trigger']} needs a time of 10-2550 ms")
    if r["effect"] in TURBO and not 0 <= r["rate"] <= MAX_HZ:
        raise RemapError(f"rule: turbo is 1-{MAX_HZ} Hz or max")
    if r["effect"] in MACRO_EFFECTS and not 1 <= r["macro"] <= MACROS:
        raise RemapError(f"rule: a macro is 1-{MACROS}")


def decode_rules(raw):
    out = []
    raw = bytes(raw)
    for i in range(0, len(raw) - RULE_SIZE + 1, RULE_SIZE):
        trigger, key, ms10, effect = raw[i:i + 4]
        if not trigger or trigger >= len(TRIGGERS) or effect >= len(EFFECTS) or key >= INPUTS or not KEYS[key]:
            continue
        mask = int.from_bytes(raw[i + 4:i + 8], "little")
        out.append(rule(KEYS[key], TRIGGERS[trigger], EFFECTS[effect],
                        [k for b, k in enumerate(KEYS) if k and mask >> b & 1],
                        ms=ms10 * 10 if TRIGGERS[trigger] in TIMED else None,
                        rate=raw[i + 8] if EFFECTS[effect] in TURBO else None,
                        macro=raw[i + 8] if EFFECTS[effect] in MACRO_EFFECTS else None))
    return out


def encode_rules(rules, count=RULES):
    """count: the rules the pad keeps, its field's size / RULE_SIZE."""
    if len(rules) > count:
        raise RemapError(f"at most {count} rules")
    raw = bytearray(count * RULE_SIZE)
    for n, r in enumerate(rules):
        check_rule(r)
        mask = 0
        if r["effect"] != "nothing" and r["effect"] not in MACRO_EFFECTS:
            for o in r["outputs"]:
                mask |= 1 << key_bit(o)
        o = n * RULE_SIZE
        raw[o:o + 4] = bytes([TRIGGERS.index(r["trigger"]), key_bit(r["input"]),
                              round(r["ms"] / 10) if r["trigger"] in TIMED else 0,
                              EFFECTS.index(r["effect"])])
        raw[o + 4:o + 8] = mask.to_bytes(4, "little")
        raw[o + 8] = r["rate"] if r["effect"] in TURBO else r["macro"] if r["effect"] in MACRO_EFFECTS else 0
    return bytes(raw)


def show_rule(r):
    text = f"{r['input']} {r['trigger']}"
    if r["trigger"] in TIMED:
        text += f":{r['ms']}"
    text += f" {r['effect']}"
    if r["effect"] in MACRO_EFFECTS:
        return text + f" {r['macro']}"
    if r["effect"] != "nothing":
        text += " " + ("+".join(r["outputs"]) or "-")
    if r["effect"] in TURBO:
        text += "@" + (str(r["rate"]) if r["rate"] else "max")
    return text


def show_rules(raw):
    return "; ".join(show_rule(r) for r in decode_rules(raw)) or "none"


def parse_rule(text):
    words = text.split()
    if len(words) < 3:
        raise RemapError(f"rule {text!r}: INPUT TRIGGER[:MS] EFFECT [OUT+OUT][@HZ|@max]")
    trigger, _, ms = words[1].partition(":")
    effect, outputs, rate, macro = words[2], [], None, None
    if effect in MACRO_EFFECTS and len(words) > 3:
        if not words[3].isdigit():
            raise RemapError(f"rule: macro {words[3]!r} is not a number")
        macro = int(words[3])
    elif len(words) > 3:
        out, _, hz = words[3].partition("@")
        outputs = [o for o in out.split("+") if o and o != "-"]
        if hz:
            rate = 0 if hz == "max" else int(hz)
    r = rule(ALIASES.get(words[0].lower(), words[0].lower()), trigger, effect,
             [ALIASES.get(o.lower(), o.lower()) for o in outputs], ms=int(ms) if ms else None, rate=rate,
             macro=macro)
    check_rule(r)
    return r


def parse_rules(text, count=RULES):
    if text.strip() in ("", "none"):
        return encode_rules([], count)
    return encode_rules([parse_rule(part) for part in text.split(";") if part.strip()], count)


# ---- macros -------------------------------------------------------------------

MACRO_UNIT_MS = 4
MACRO_PRESS = 0x20
MACRO_WAIT_ONLY = 0xFF
MACRO_NAME = 16
MACRO_BYTES = 200        # the pad's field size (REMAP_MACRO_BYTES) is the one that counts


def decode_macros(raw):
    """[{"name", "steps": [(ms before it, "+key" / "-key" / None for the pause at the end)]}]."""
    raw, out, o = bytes(raw), [], 0
    while o < len(raw) and raw[o]:
        end = o + 1 + raw[o]
        n = raw[o + 1] if o + 1 < len(raw) else 0
        name = raw[o + 2:o + 2 + n].decode("utf-8", "replace")
        steps, wait = [], 0
        for i in range(o + 2 + n, min(end, len(raw)) - 1, 2):
            wait += raw[i] * MACRO_UNIT_MS
            a = raw[i + 1]
            if a == MACRO_WAIT_ONLY:
                continue
            key = KEYS[a & 0x1F] if (a & 0x1F) < INPUTS else None
            if (a & ~MACRO_PRESS & 0xFF) >= INPUTS or not key:
                continue
            steps.append((wait, ("+" if a & MACRO_PRESS else "-") + key))
            wait = 0
        if wait:
            steps.append((wait, None))
        out.append({"name": name, "steps": steps})
        o = end
    return out


def encode_macros(macros, size=MACRO_BYTES):
    """macros as decode_macros gives them; RemapError if they do not fit or a step is wrong."""
    if len(macros) > MACROS:
        raise RemapError(f"at most {MACROS} macros")
    raw = bytearray()
    for m in macros:
        name = m["name"].encode()
        if len(name) > MACRO_NAME:
            raise RemapError(f"macro {m['name']!r}: a name is at most {MACRO_NAME} bytes")
        body = bytearray()
        for ms, action in m["steps"]:
            if not isinstance(ms, int) or ms < 0:
                raise RemapError(f"macro {m['name']!r}: a wait is a number of ms")
            units = (ms + MACRO_UNIT_MS // 2) // MACRO_UNIT_MS
            while units > 0xFF:
                body += bytes([0xFF, MACRO_WAIT_ONLY])
                units -= 0xFF
            if action is None:
                if units:
                    body += bytes([units, MACRO_WAIT_ONLY])
                continue
            if action[:1] not in "+-" or not action[1:]:
                raise RemapError(f"macro {m['name']!r}: {action!r} is not +KEY or -KEY")
            body += bytes([units, key_bit(action[1:]) | (MACRO_PRESS if action[0] == "+" else 0)])
        if not body:
            raise RemapError(f"macro {m['name']!r} has no steps")
        if 1 + len(name) + len(body) > 0xFF:
            raise RemapError(f"macro {m['name']!r} is too long")
        raw += bytes([1 + len(name) + len(body), len(name)]) + name + body
    if len(raw) > size:
        raise RemapError(f"the macros take {len(raw)} bytes; the pad has {size}")
    return bytes(raw.ljust(size, b"\0"))


def show_macros(raw):
    def step(ms, action):
        return " ".join(([str(ms)] if ms else []) + ([action] if action else []))
    return "; ".join(f"{m['name']}: " + " ".join(step(ms, a) for ms, a in m["steps"])
                     for m in decode_macros(raw)) or "none"


def parse_macros(text, size=MACRO_BYTES):
    text = text.strip()
    if text in ("", "none"):
        return encode_macros([], size)
    macros = []
    for part in (p for p in text.split(";") if p.strip()):
        name, colon, rest = part.partition(":")
        if not colon:
            raise RemapError(f"macro {part.strip()!r}: NAME: STEP STEP ...")
        steps, wait = [], 0
        for word in rest.split():
            if word.isdigit():
                wait += int(word)
            elif word[:1] in "+-":
                steps.append((wait, word[0] + KEYS[key_bit(word[1:])]))
                wait = 0
            else:
                raise RemapError(f"macro {name.strip()!r}: {word!r} is not +KEY, -KEY or a number of ms")
        if wait:
            steps.append((wait, None))
        macros.append({"name": name.strip(), "steps": steps})
    return encode_macros(macros, size)


# ---- presets ------------------------------------------------------------------

# name: (label, map text).  They set base rows only.
PRESETS = {
    "swap-faces": ("Swap faces (south/east, west/north)", "south=east east=south west=north north=west"),
    "swap-south-east": ("Swap south/east", "south=east east=south"),
    "swap-west-north": ("Swap west/north", "west=north north=west"),
    "capture-off": ("Capture off", "capture=none"),
}


def apply_preset(base, name):
    """base with the preset's keys set; other keys as they were."""
    preset = decode_map(parse_map(PRESETS[name][1]))
    return {k: preset[k] if preset[k] != k else base[k] for k in base}


# ---- preset files ---------------------------------------------------------------

FILE_FORMAT = 1


def export_preset(name, map_raw, rules_raw):
    """A remap slot as a JSON-able dict, in the CLI's text."""
    return {"format": FILE_FORMAT, "name": name, "map": show_map(map_raw), "rules": show_rules(rules_raw)}


def import_preset(data, count=RULES):
    """(name, map bytes, rules bytes) from export_preset's dict; RemapError if it is not one."""
    if not isinstance(data, dict) or data.get("format") != FILE_FORMAT:
        raise RemapError("not a remap preset file (format 1)")
    try:
        return str(data.get("name", "")), parse_map(str(data.get("map", ""))), parse_rules(str(data.get("rules", "")), count)
    except RemapError as error:
        raise RemapError(f"preset file: {error}")


# ---- the name -----------------------------------------------------------------

def show_name(raw):
    return bytes(raw).split(b"\0")[0].decode("utf-8", "replace")


def parse_name(text, size):
    raw = text.encode()
    if len(raw) > size:
        raise RemapError(f"name: at most {size} bytes")
    return raw.ljust(size, b"\0")
