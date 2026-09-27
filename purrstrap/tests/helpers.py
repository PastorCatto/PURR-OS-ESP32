"""Shared test setup: import path, a capturing context, script writers."""

import io
import os
import sys
import tempfile
import textwrap

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOL_DIR not in sys.path:
    sys.path.insert(0, TOOL_DIR)

from lib.context import Context  # noqa: E402


def make_ctx(root=None):
    root = root or tempfile.mkdtemp()
    out = io.StringIO()
    return Context(root, root, out=out), out


def write_script(directory, name, body):
    path = os.path.join(directory, name + ".py")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(textwrap.dedent(body))
    return path


GOOD = """
from lib.model import Script, Action, Param

def hello(ctx, who, loud):
    ctx.info(("HELLO " if loud else "hello ") + who)
    return 0

SCRIPT = Script(
    name="{name}", title="Good", description="works",
    actions=(Action("hello", "Hello", hello, help="say hi",
                    params=(Param("who", "choice", choices=("a", "b"), default="a"),
                            Param("loud", "bool", default=False))),))
"""
