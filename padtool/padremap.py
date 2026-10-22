"""Remap slots as text, for the CLI and the GUI.

Each key has a base row: while held it sends itself (default), another key or
nothing.  That is the map, which takes no rule entries.  Rules add to it.

Map: space-separated KEY=KEY or KEY=none for keys that differ from
themselves; "identity" when none do.

Rules: ';'-separated, each "INPUT TRIGGER[:MS] EFFECT [OUT+OUT...][@HZ|@max]",
e.g. "r4 held hold l1+r1; a held turbo a@max; r4 tapped:300 tap a".  A turbo
modifier's keys are the keys pressed: "r4 held turbo_modifier a+b@10" pulses
whatever A and B send while R4 is held.
"none" when there are none.  The layout matches struct remap_rule (src/core/pad.h).
"""

# PAD_* bit numbers; bit 15 is no key.
KEYS = ("a", "b", "capture", "x", "y", "m", "l1", "r1", "l4", "r4", "minus", "plus",
        "home", "l3", "r3", None, "up", "right", "down", "left")
NAMES = tuple(k for k in KEYS if k)
ALIASES = {"select": "minus", "start": "plus", "screenshot": "capture"}
INPUTS = len(KEYS)
OFF = 0xFF
RULES = 16
RULE_SIZE = 10
MAX_HZ = 30

TRIGGERS = (None, "held", "held_for", "pressed", "released", "tapped", "double_tapped")
EFFECTS = ("hold", "turbo", "nothing", "tap", "toggle_hold", "toggle_turbo",
           "turbo_modifier", "toggle_turbo_modifier")
LEVEL_TRIGGERS = ("held", "held_for")
LEVEL_EFFECTS = ("hold", "turbo", "turbo_modifier", "nothing")
EVENT_EFFECTS = ("tap", "toggle_hold", "toggle_turbo", "toggle_turbo_modifier")
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

def rule(input, trigger, effect, outputs=(), ms=None, rate=None):
    """A rule as a dict, with the GUI's starting time and rate filled in."""
    return {"input": input, "trigger": trigger, "effect": effect, "outputs": list(outputs),
            "ms": TIMED.get(trigger) if ms is None else ms,
            "rate": TURBO_HZ if rate is None else rate}


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
                        rate=raw[i + 8] if EFFECTS[effect] in TURBO else None))
    return out


def encode_rules(rules):
    if len(rules) > RULES:
        raise RemapError(f"at most {RULES} rules")
    raw = bytearray(RULES * RULE_SIZE)
    for n, r in enumerate(rules):
        check_rule(r)
        mask = 0
        if r["effect"] != "nothing":
            for o in r["outputs"]:
                mask |= 1 << key_bit(o)
        o = n * RULE_SIZE
        raw[o:o + 4] = bytes([TRIGGERS.index(r["trigger"]), key_bit(r["input"]),
                              round(r["ms"] / 10) if r["trigger"] in TIMED else 0,
                              EFFECTS.index(r["effect"])])
        raw[o + 4:o + 8] = mask.to_bytes(4, "little")
        raw[o + 8] = r["rate"] if r["effect"] in TURBO else 0
    return bytes(raw)


def show_rule(r):
    text = f"{r['input']} {r['trigger']}"
    if r["trigger"] in TIMED:
        text += f":{r['ms']}"
    text += f" {r['effect']}"
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
    effect, outputs, rate = words[2], [], None
    if len(words) > 3:
        out, _, hz = words[3].partition("@")
        outputs = [o for o in out.split("+") if o and o != "-"]
        if hz:
            rate = 0 if hz == "max" else int(hz)
    r = rule(ALIASES.get(words[0].lower(), words[0].lower()), trigger, effect,
             [ALIASES.get(o.lower(), o.lower()) for o in outputs], ms=int(ms) if ms else None, rate=rate)
    check_rule(r)
    return r


def parse_rules(text):
    if text.strip() in ("", "none"):
        return encode_rules([])
    return encode_rules([parse_rule(part) for part in text.split(";") if part.strip()])


# ---- presets ------------------------------------------------------------------

# name: (label, map text).  They set base rows only.
PRESETS = {
    "swap-ab-xy": ("Swap A/B and X/Y", "a=b b=a x=y y=x"),
    "swap-ab": ("Swap A/B", "a=b b=a"),
    "swap-xy": ("Swap X/Y", "x=y y=x"),
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


def import_preset(data):
    """(name, map bytes, rules bytes) from export_preset's dict; RemapError if it is not one."""
    if not isinstance(data, dict) or data.get("format") != FILE_FORMAT:
        raise RemapError("not a remap preset file (format 1)")
    try:
        return str(data.get("name", "")), parse_map(str(data.get("map", ""))), parse_rules(str(data.get("rules", "")))
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
