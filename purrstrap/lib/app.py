"""Entry logic: choose the CLI or the TUI."""

import os
import sys

from . import cli, ui
from .context import Context
from .registry import OK, discover

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _script_menu(ctx, entry, state):
    script = entry.script
    while True:
        labels = [f"{a.title}  -  {a.help}" if a.help else a.title
                  for a in script.actions]
        pick = ui.menu(f"purrstrap > {script.title}", labels)
        if pick is None:
            return
        action = script.actions[pick]
        key = f"{script.name}.{action.name}"
        params = {}
        if action.params:
            params = ui.form(f"{script.title} > {action.title}", action.params,
                             state.get(key, {}))
            if params is None:
                continue
            state[key] = {k: v for k, v in params.items() if v is not None}
            ctx.save_state(state)
        sys.stdout.write(ui.CLEAR)
        code = cli.run_action(ctx, action, params)
        report = ctx.ok if code == 0 else ctx.error
        report(f"{action.title} finished with exit code {code}")
        ui.wait()


def tui(ctx, entries) -> int:
    ui.enable_ansi()
    state = ctx.load_state()
    while True:
        labels = []
        for e in entries:
            if e.status == OK:
                labels.append(f"{e.script.title}  -  {e.script.description}")
            else:
                labels.append(f"{e.name}  [{e.status}]")
        pick = ui.menu("purrstrap", labels)
        if pick is None:
            return 0
        entry = entries[pick]
        if entry.status != OK:
            ctx.error(f"{entry.name} is {entry.status}")
            ctx.info(entry.error)
            ui.wait()
            continue
        _script_menu(ctx, entry, state)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    ctx = Context(os.path.dirname(TOOL_DIR), TOOL_DIR)
    entries = discover(os.path.join(TOOL_DIR, "scripts"))
    if argv:
        return cli.main(argv, ctx, entries)
    if sys.stdin.isatty() and sys.stdout.isatty():
        return tui(ctx, entries)
    cli.print_list(ctx, entries)
    return 0
