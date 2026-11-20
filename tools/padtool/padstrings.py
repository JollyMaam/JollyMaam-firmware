"""The web page's string catalog, for the GUI: one set of words for both
(web/strings/README.md).  en.json is the English, a locale file beside it
(de.json, pt-br.json) a translation, settings.json maps a field's kind:id to
its entries.  In padtool's zip the folder is padtool/strings; in the tree,
web/strings.  Without one, every text is its fallback."""
import json
import locale
import os
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
_english, _translated, _fields = {}, {}, {}
language = "en"


def folder():
    for path in (HERE / "strings", HERE.parents[1] / "web" / "strings"):
        if (path / "en.json").is_file():
            return path
    return None


def _read(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def load(lang=None):
    """The catalog for lang (PADTOOL_LANG, else the system's), falling back
    to its base language, then English, as the web page does."""
    global _english, _translated, _fields, language
    where = folder()
    _english = _read(where / "en.json") if where else {}
    _fields = _read(where / "settings.json") if where else {}
    _translated, language = {}, "en"
    lang = (lang or os.environ.get("PADTOOL_LANG") or (locale.getlocale()[0] or "")).lower().replace("_", "-")
    if where and re.fullmatch(r"[a-z]{2,3}(-[a-z0-9]{2,8})*", lang):
        for name in dict.fromkeys([lang, lang.split("-")[0]]):
            if name == "en":
                break
            if (where / f"{name}.json").is_file():
                _translated, language = _read(where / f"{name}.json"), name
                break


def text(key, fallback=None, **params):
    """The words for key, {name} filled from params; fallback (else the key)
    when the catalog lacks it."""
    value = _translated.get(key) or _english.get(key) or (fallback if fallback is not None else key)
    return re.sub(r"\{([a-zA-Z][a-zA-Z0-9_]*)\}", lambda m: str(params[m[1]]) if m[1] in params else m[0], value)


def field(f, part, fallback):
    """A setting's label or help (part), by its kind and id, as the web page
    shows it; the rule count in help comes from the field's size."""
    prefix = _fields.get(f"{f['kind']}:{f['id']}")
    if not prefix:
        return fallback
    return text(f"{prefix}.{part}", fallback, count=f.get("size", 0) // 10) or fallback


load()
