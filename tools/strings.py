#!/usr/bin/env python3
"""Maintain flat UI catalogs; inspect all string-bearing source lines."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STRINGS = ROOT / "web/strings"


def read(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"{path}: duplicate key {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(), object_pairs_hook=unique)


def settings(cc):
    with tempfile.TemporaryDirectory() as directory:
        exe = str(Path(directory) / "settings-strings")
        subprocess.run(shlex.split(cc) + ["-O2", "-ffunction-sections", "-fdata-sections",
                       "-Wl,--gc-sections", str(ROOT / "tools/settings_strings.c"), "-o", exe], check=True)
        return json.loads(subprocess.check_output([exe], text=True))


def sync(cc):
    source = read(STRINGS / "en.json")
    mapping = {}
    boxed = {"maps_to": "Maps to", "x_axis": "Invert X axis", "y_axis": "Invert Y axis",
             "press": "Digital at", "mode": "Analog mode", "curve_points": "Curve"}
    for field in settings(cc):
        name = field["name"]
        prefix = f"settings.{field['group']}.{name}"
        mapping[f"{field['kind']}:{field['id']}"] = prefix
        side, _, rest = name.partition("_")
        grouped = field["group"] == "remap" and side in ("left", "right", "l2", "r2")
        label = boxed.get(rest, rest.replace("_", " ").capitalize()) if grouped else name.replace("_", " ").capitalize()
        source.setdefault(prefix + ".label", label)
        source.setdefault(prefix + ".help", "")
        for option in field["options"]:
            source.setdefault(prefix + ".options." + option, option.capitalize() if grouped else option)
    for name, value in (("en", source), ("settings", mapping)):
        (STRINGS / (name + ".json")).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    print("Added missing setting strings; existing English edits were preserved.")


def placeholders(value):
    return set(re.findall(r"\{([a-zA-Z][a-zA-Z0-9_]*)\}", value))


def check(cc):
    source = read(STRINGS / "en.json")
    mapping = read(STRINGS / "settings.json")
    errors = []
    schema = settings(cc)
    fixture = ROOT / "web/mock-fields.json"
    if fixture.exists() and read(fixture) != schema:
        errors.append("mock schema is stale: run make mock-fixture")
    for field in schema:
        prefix = f"settings.{field['group']}.{field['name']}"
        if mapping.get(f"{field['kind']}:{field['id']}") != prefix:
            errors.append(f"missing or changed field mapping: {prefix}")
        required = [prefix + ".label"]
        required.append(prefix + ".help")
        required += [prefix + ".options." + name for name in field['options']]
        errors += [f"missing English: {key}" for key in required if key not in source]
    for path in sorted(STRINGS.glob("*.json")):
        if path.name == "settings.json":
            continue
        catalog = read(path)
        for key, value in catalog.items():
            if not isinstance(value, str):
                errors.append(f"{path.name}: {key}: expected a string")
            elif key not in source:
                errors.append(f"{path.name}: unknown key {key}")
            elif placeholders(value) != placeholders(source[key]):
                errors.append(f"{path.name}: {key}: placeholders differ from English")
        if path.name != "en.json":
            print(f"{path.name}: {len(source.keys() - catalog.keys())} entries fall back to English")
    for path in (ROOT / "web").glob("*.js"):
        for key in re.findall(r'(?:msg|text)\("(ui\.[^"]+)"', path.read_text()):
            if key not in source:
                errors.append(f"{path.name}: missing English: {key}")
    # padtool's GUI takes its words from the same catalog (padstrings.py).
    for path in [*(ROOT / "tools/padtool").glob("*.py"), *(ROOT / "tools/padtool").glob("*.pyw")]:
        for key in re.findall(r'\bT\(\s*"(ui\.[^"]+)"', path.read_text()):
            if key not in source:
                errors.append(f"{path.name}: missing English: {key}")
    for key in re.findall(r'data-i18n="([^"]+)"', (ROOT / "web/index.html").read_text()):
        if key not in source:
            errors.append(f"index.html: missing English: {key}")
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"Checked {len(source)} English entries and {len(mapping)} setting mappings.")


def report(output):
    catalog = read(STRINGS / "en.json")
    # Deliberately broad: keep technical tokens and diagnostics visible rather
    # than guessing which source strings a person will want to translate.
    paths = list((ROOT / "web").glob("*.js")) + [ROOT / "web/index.html"]
    paths += list((ROOT / "android/app/src/main").rglob("*.kt"))
    paths += list((ROOT / "android/app/src/main").rglob("*.xml"))
    paths += list((ROOT / "android/app/src/main").rglob("*.c"))
    paths += list((ROOT / "src/lib").glob("*.c"))
    audit = []
    for path in sorted(paths):
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if any(quote in line for quote in ('"', "'", '`')):
                audit.append([f"{path.relative_to(ROOT)}:{number}", line.strip()])
    data = json.dumps({"catalog": catalog, "audit": audit}, ensure_ascii=False).replace("<", "\\u003c")
    template = (ROOT / "tools/strings-report.html").read_text()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(template.replace("/* DATA */", data))
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["check", "report", "sync-settings", "export-settings", "mock-fixture"])
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--output", type=Path, default=ROOT / "build/strings.html")
    args = parser.parse_args()
    if args.command == "check":
        check(args.cc)
    elif args.command == "mock-fixture":
        path = ROOT / "web/mock-fields.json"
        path.write_text(json.dumps(settings(args.cc), ensure_ascii=False, indent=2) + "\n")
        print(path)
    elif args.command == "sync-settings":
        sync(args.cc)
    elif args.command == "report":
        report(args.output)
    else:
        print(json.dumps(settings(args.cc), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
