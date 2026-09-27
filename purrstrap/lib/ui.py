"""Menus and forms using only the standard library and ANSI codes."""

import os
import sys

from .model import coerce

CLEAR = "\033[2J\033[H"


# -- keys --------------------------------------------------------------
def _plain(ch):
    if ch == "\x03":
        raise KeyboardInterrupt
    return {"\r": "enter", "\n": "enter", " ": "space", "\x1b": "esc",
            "\x7f": "backspace", "\x08": "backspace"}.get(ch, ch)


def _read_key_windows():
    import msvcrt
    ch = msvcrt.getwch()
    if ch in ("\x00", "\xe0"):
        return {"H": "up", "P": "down", "K": "left", "M": "right"}.get(
            msvcrt.getwch(), "")
    return _plain(ch)


def _read_key_posix():
    import select
    import termios
    import tty
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        ch = sys.stdin.read(1)
        if ch == "\x1b":
            if select.select([sys.stdin], [], [], 0.05)[0]:
                seq = sys.stdin.read(2)
                return {"[A": "up", "[B": "down", "[D": "left",
                        "[C": "right"}.get(seq, "")
            return "esc"
        return _plain(ch)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def read_key():
    """Return one key: up/down/left/right/enter/esc/space/backspace or a char."""
    return _read_key_windows() if os.name == "nt" else _read_key_posix()


def read_line(prompt):
    return input(prompt)


def enable_ansi():
    if os.name == "nt":
        os.system("")  # switches the Windows console into ANSI mode


# -- widgets -----------------------------------------------------------
def menu(title, items, out=None, footer="Up/Down move, Enter select, Esc back"):
    """Show items, return the chosen index or None if the user backed out."""
    out = out or sys.stdout
    if not items:
        return None
    pos = 0
    while True:
        out.write(CLEAR + f"{title}\n\n")
        for i, text in enumerate(items):
            out.write(f"  {'>' if i == pos else ' '} {text}\n")
        out.write(f"\n  {footer}\n")
        out.flush()
        key = read_key()
        if key == "up":
            pos = (pos - 1) % len(items)
        elif key == "down":
            pos = (pos + 1) % len(items)
        elif key == "enter":
            return pos
        elif key in ("esc", "q"):
            return None


def _show(param, value):
    if param.type == "bool":
        return "yes" if value else "no"
    return "" if value is None else str(value)


def form(title, params, values, out=None, run_label="Run"):
    """Edit the params of an action. Returns {name: value} or None if cancelled."""
    out = out or sys.stdout
    values = dict(values)
    for p in params:
        values.setdefault(p.name, p.default)
    pos, message = 0, ""
    last = len(params)  # index of the Run row
    while True:
        out.write(CLEAR + f"{title}\n\n")
        for i, p in enumerate(params):
            mark = ">" if i == pos else " "
            req = "*" if p.required else " "
            out.write(f"  {mark} {p.name + req:<16} {_show(p, values[p.name])}\n")
            if i == pos and p.help:
                out.write(f"      {p.help}\n")
        out.write(f"  {'>' if pos == last else ' '} [ {run_label} ]\n")
        if message:
            out.write(f"\n  {message}\n")
        out.write("\n  Up/Down move, Enter edit/run, Left/Right change choice, "
                  "Space toggle, Esc cancel\n")
        out.flush()
        message = ""
        key = read_key()
        p = params[pos] if pos < last else None
        if key == "up":
            pos = (pos - 1) % (last + 1)
        elif key == "down":
            pos = (pos + 1) % (last + 1)
        elif key == "esc":
            return None
        elif p is not None and p.type == "bool" and key in ("space", "enter"):
            values[p.name] = not values[p.name]
        elif p is not None and p.type == "choice" and key in (
                "left", "right", "enter", "space"):
            cur = (p.choices.index(values[p.name])
                   if values[p.name] in p.choices else -1)
            step = -1 if key == "left" else 1
            values[p.name] = p.choices[(cur + step) % len(p.choices)]
        elif p is not None and key == "enter":
            raw = read_line(f"  {p.name}: ")
            if raw.strip() == "":
                continue
            try:
                values[p.name] = coerce(p, raw.strip())
            except ValueError as exc:
                message = str(exc)
        elif p is None and key == "enter":
            missing = [q.name for q in params
                       if q.required and values[q.name] in (None, "")]
            if missing:
                message = "still needed: " + ", ".join(missing)
            else:
                return values


def wait(out=None):
    out = out or sys.stdout
    out.write("\n  Press any key to continue...")
    out.flush()
    read_key()
