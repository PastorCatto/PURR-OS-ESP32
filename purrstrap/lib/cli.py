"""Command line front end, generated from the subscript declarations."""

import argparse
import traceback

from .model import coerce
from .registry import OK


def run_action(ctx, action, params) -> int:
    """Run one action. Exceptions become a printed traceback and exit code 1."""
    try:
        code = action.run(ctx, **params)
    except KeyboardInterrupt:
        ctx.error("interrupted")
        return 130
    except BaseException:
        ctx.error("action crashed:")
        ctx.info(traceback.format_exc())
        return 1
    return 0 if code is None else int(code)


def print_list(ctx, entries):
    if not entries:
        ctx.info("no subscripts found")
        return
    for e in entries:
        if e.status == OK:
            ctx.info(f"{e.name}: {e.script.title}")
            ctx.info(f"    {e.script.description}")
            for a in e.script.actions:
                ctx.info(f"    {e.name} {a.name}: {a.help or a.title}")
                for p in a.params:
                    extra = f" ({'|'.join(p.choices)})" if p.choices else ""
                    dflt = f" [default {p.default}]" if p.default is not None else ""
                    req = " (required)" if p.required else ""
                    ctx.info(f"        --{p.name.replace('_', '-')}: "
                             f"{p.type}{extra}{dflt}{req} {p.help}".rstrip())
        else:
            ctx.warn(f"{e.name}: {e.status}")
            lines = e.error.strip().splitlines()
            ctx.info(f"    {lines[-1] if lines else ''}")


def _script_parser(entry):
    parser = argparse.ArgumentParser(
        prog=f"purrstrap.py {entry.name}", description=entry.script.description)
    sub = parser.add_subparsers(dest="_action", metavar="<action>", required=True)
    for action in entry.script.actions:
        ap = sub.add_parser(action.name, help=action.help or action.title,
                            description=action.help or action.title)
        for p in action.params:
            flag = "--" + p.name.replace("_", "-")
            kw = {"dest": p.name, "help": p.help or None}
            if p.type == "bool":
                ap.add_argument(flag, action=argparse.BooleanOptionalAction,
                                default=p.default if p.default is not None else False,
                                **kw)
                continue
            if p.type == "choice":
                kw["choices"] = list(p.choices)
            ap.add_argument(flag, default=p.default,
                            required=p.required and p.default is None, **kw)
    return parser


def main(argv, ctx, entries) -> int:
    if argv[0] in ("list", "--list"):
        print_list(ctx, entries)
        return 0
    entry = next((e for e in entries if e.name == argv[0]), None)
    if entry is None:
        ctx.error(f"unknown subscript {argv[0]!r}. Try: purrstrap.py list")
        return 2
    if entry.status != OK:
        ctx.error(f"{entry.name} is {entry.status}")
        ctx.info(entry.error)
        return 2
    args = _script_parser(entry).parse_args(argv[1:])
    action = next(a for a in entry.script.actions if a.name == args._action)
    try:
        params = {p.name: coerce(p, getattr(args, p.name)) for p in action.params}
    except ValueError as exc:
        ctx.error(str(exc))
        return 2
    return run_action(ctx, action, params)
