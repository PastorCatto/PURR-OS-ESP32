"""Find, load and validate subscripts. A bad subscript never stops the rest."""

import importlib.util
import os
import traceback
from dataclasses import dataclass
from typing import List, Optional

from .model import PARAM_TYPES, Script, coerce

OK, BROKEN, UNAVAILABLE = "ok", "broken", "unavailable"


@dataclass
class Entry:
    name: str
    path: str
    status: str
    script: Optional[Script] = None
    error: str = ""


def validate(script, name: str) -> str:
    """Return an error message, or '' if the declaration is valid."""
    if not isinstance(script, Script):
        return "SCRIPT is not a Script"
    if script.name != name:
        return f"SCRIPT.name {script.name!r} must equal the file name {name!r}"
    seen = set()
    for action in script.actions:
        if not action.name or action.name in seen:
            return f"duplicate or empty action name {action.name!r}"
        seen.add(action.name)
        if not callable(action.run):
            return f"action {action.name}: run is not callable"
        pseen = set()
        for p in action.params:
            where = f"action {action.name}, param {p.name}"
            if not p.name or p.name in pseen:
                return f"{where}: duplicate or empty name"
            pseen.add(p.name)
            if p.type not in PARAM_TYPES:
                return f"{where}: unknown type {p.type!r}"
            if p.type == "choice" and not p.choices:
                return f"{where}: choice param has no choices"
            if p.default is not None:
                try:
                    coerce(p, p.default)
                except ValueError as exc:
                    return f"{where}: bad default ({exc})"
    return ""


def _missing(requires) -> List[str]:
    return [r for r in requires if importlib.util.find_spec(r) is None]


def _load_one(path: str, name: str) -> Entry:
    try:
        spec = importlib.util.spec_from_file_location(
            f"purrstrap_script_{name}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    except BaseException:
        return Entry(name, path, BROKEN, error=traceback.format_exc())
    script = getattr(module, "SCRIPT", None)
    problem = validate(script, name)
    if problem:
        return Entry(name, path, BROKEN, error=problem)
    missing = _missing(script.requires)
    if missing:
        return Entry(name, path, UNAVAILABLE, script=script,
                     error="missing packages: " + ", ".join(missing))
    return Entry(name, path, OK, script=script)


def discover(scripts_dir: str) -> List[Entry]:
    entries = []
    if not os.path.isdir(scripts_dir):
        return entries
    for fname in sorted(os.listdir(scripts_dir)):
        if not fname.endswith(".py") or fname.startswith("_"):
            continue
        entries.append(_load_one(os.path.join(scripts_dir, fname), fname[:-3]))
    return entries
