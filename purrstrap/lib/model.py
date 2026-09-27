"""Declarations a subscript uses to describe itself."""

from dataclasses import dataclass
from typing import Any, Callable, Tuple

PARAM_TYPES = ("str", "int", "bool", "choice", "path")


@dataclass(frozen=True)
class Param:
    name: str
    type: str = "str"
    default: Any = None
    choices: Tuple[str, ...] = ()
    help: str = ""
    required: bool = False


@dataclass(frozen=True)
class Action:
    name: str
    title: str
    run: Callable[..., int]
    help: str = ""
    params: Tuple[Param, ...] = ()


@dataclass(frozen=True)
class Script:
    name: str
    title: str
    description: str
    actions: Tuple[Action, ...]
    requires: Tuple[str, ...] = ()


def coerce(param: Param, raw):
    """Turn a raw value (usually a string) into the param's type.

    Raises ValueError with a readable message if it does not fit.
    """
    if raw is None:
        return None
    if param.type == "bool":
        if isinstance(raw, bool):
            return raw
        text = str(raw).strip().lower()
        if text in ("1", "true", "yes", "y", "on"):
            return True
        if text in ("0", "false", "no", "n", "off"):
            return False
        raise ValueError(f"{param.name}: expected yes/no, got {raw!r}")
    if param.type == "int":
        try:
            return int(str(raw).strip(), 0)
        except ValueError:
            raise ValueError(f"{param.name}: expected a number, got {raw!r}")
    if param.type == "choice":
        text = str(raw)
        if text not in param.choices:
            raise ValueError(
                f"{param.name}: {text!r} is not one of {', '.join(param.choices)}")
        return text
    return str(raw)
