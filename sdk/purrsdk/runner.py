"""Runs purrstrap (the build tool) the way the kit does: same Python, the kit's root as its working folder,
the key password passed in the environment (never on a command line)."""

import os
import subprocess
import sys

from . import formats


def root():
    """The folder purrstrap treats as the repository: the checkout, or the kit's root (laid out the same)."""
    return os.path.dirname(formats.purrstrap_dir())


def purrstrap(args, password=None, on_line=None, cwd=None, extra_env=None, python=None):
    """Run `purrstrap <args>`. Returns {"rc": int, "output": str}. `on_line(text)` is called for each line as it arrives."""
    exe = python or os.environ.get("PURR_PYTHON") or sys.executable
    cmd = [exe, os.path.join(formats.purrstrap_dir(), "purrstrap.py")] + [str(a) for a in args]
    env = dict(os.environ)
    env["PYTHONUNBUFFERED"] = "1"
    env.setdefault("IDF_TOOLS_PATH", os.path.expanduser("~/.espressif"))
    if password is not None:
        env["PURRSTRAP_KEY_PASSWORD"] = password
    else:
        env.pop("PURRSTRAP_KEY_PASSWORD", None)
    env.update(extra_env or {})
    try:
        proc = subprocess.Popen(cmd, cwd=cwd or root(), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, errors="replace", stdin=subprocess.DEVNULL)
    except OSError as e:
        return {"rc": 127, "output": "cannot start the build tool: %s\n" % e}
    lines = []
    for line in proc.stdout:
        lines.append(line)
        if on_line:
            on_line(line.rstrip("\n"))
    return {"rc": proc.wait(), "output": "".join(lines)}
