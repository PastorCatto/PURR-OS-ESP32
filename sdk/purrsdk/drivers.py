"""The essential drivers of a board, built from source, signed and packed into the `drivers` partition (Drivers/SPEC.md, "The driver partition").

Nothing is compiled into the kernel for them any more (only the W5500 is, a virtual device's flag). The image maker puts the file this module writes at the `drivers`
partition's offset, and an update that replaces one driver rewrites one 8 KB slot. Each driver is a signed module like any other: the device checks its signature when it
loads it, with the same key list as for apps and the other modules, so the key that signs them must be one the build trusts (the developer key, role developer).

Which drivers a board gets is the board's file: `drivers = ["st7789.cat", ...]` in its [board] section (at most ten, one per slot); a board file without the line gets the
default for its id below, which is what the kernel used to plant.
"""

import os

from . import runner

# file the driver is planted under -> (the module's name, its source in the repository, its entry symbol)
ESSENTIAL = {
    "kbd_i2c.cat": ("kbd_i2c", "Modules/drivers/kbd_i2c_module.c", "kbd_i2c_entry"),
    "gt911.cat": ("gt911", "Modules/drivers/gt911_module.c", "gt911_entry"),
    "st7789.cat": ("st7789", "Modules/drivers/st7789_module.c", "st7789_entry"),
    "battery.cat": ("battery-adc", "Modules/drivers/battery_module.c", "battery_entry"),
    "kbd-backlight.cat": ("kbd-backlight", "Modules/drivers/kbd_backlight_module.c", "kbd_backlight_entry"),
    "gamepad.cat": ("gamepad-mcp23017", "Modules/drivers/gamepad_mcp23017_module.c", "gamepad_mcp23017_entry"),
}

# what each board needs to start (its display, its input, its gauge): the kernel's board file declares the devices, these are the modules that drive them
DEFAULTS = {
    "tdeck_plus": ("st7789.cat", "gt911.cat", "kbd_i2c.cat", "battery.cat", "kbd-backlight.cat"),
    "tdeck": ("st7789.cat", "gt911.cat", "kbd_i2c.cat", "battery.cat", "kbd-backlight.cat"),
    "playtime": ("st7789.cat", "gamepad.cat"),
}
MAX_DRIVERS = 10


class DriverError(Exception):
    pass


def board_files(board, toml=None):
    """The driver file names for a board: its file's `drivers` line (`toml` is the parsed board file), else the default for its id; [] for a board with none (the 8 MB board)."""
    listed = None
    if toml:
        listed = (toml.get("board") or {}).get("drivers")
    files = list(listed) if listed is not None else list(DEFAULTS.get(board, ()))
    for f in files:
        if f not in ESSENTIAL:
            raise DriverError("The driver %r is not one the image maker knows how to build (%s)." % (f, ", ".join(sorted(ESSENTIAL))))
    if len(files) > MAX_DRIVERS:
        raise DriverError("A board's drivers partition has %d slots; %d drivers are listed." % (MAX_DRIVERS, len(files)))
    if len(set(files)) != len(files):
        raise DriverError("A driver is listed twice.")
    return files


def build(board, tree, key, key_id, password, out, version, files=None, run=None, on_line=print, on_step=None):
    """Compile each of the board's drivers from `tree` (the pulled source), sign it with `key`, and write the partition image to `out`. Returns {"ok", "out", "files", "problem"}.
    `run(args, extra_env=None, password=None)` is purrstrap (a parameter, so tests can stand in for the compiler)."""
    ps = run or (lambda args, extra_env=None, password=None: runner.purrstrap(args, password=password, on_line=on_line, extra_env=extra_env))
    env = {"PURR_REPO_ROOT": tree}
    files = list(files) if files is not None else board_files(board)

    def fail(note):
        if on_step:
            on_step("drivers", "fail", note)
        on_line(note)
        return {"ok": False, "out": out, "files": files, "problem": note}

    if not files:
        return fail("The board %s has no drivers to build." % board)
    if on_step:
        on_step("drivers", "run", "%d essential driver(s) for %s" % (len(files), board))
    work = os.path.join(os.path.dirname(os.path.abspath(out)) or ".", ".drivers-work")
    os.makedirs(work, exist_ok=True)
    cats = []
    for f in files:
        name, rel, entry = ESSENTIAL[f]
        src = os.path.join(tree, *rel.split("/"))
        if not os.path.isfile(src):
            return fail("The source of %s (%s) is not in the pulled tree." % (f, rel))
        dest = os.path.join(work, f)
        r = ps(["modules", "build", "--board", board, "--source", src, "--entry", entry, "--kind", "driver", "--name", name, "--version", version, "--out", dest], extra_env=env)
        if r["rc"] != 0:
            return fail("%s did not compile: %s" % (f, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
        r = ps(["keys", "sign", "--key", key, "--image", dest, "--key-id", str(int(key_id))], password=password)
        if r["rc"] != 0:
            return fail("Signing %s failed: %s" % (f, (r["output"].strip().splitlines() or ["no reason given"])[-1]))
        cats.append(dest)
    r = ps(["drvpart", "pack", "--out", out, "--files", ",".join(cats)])
    if r["rc"] != 0:
        return fail("Packing the drivers failed: %s" % (r["output"].strip().splitlines() or ["no reason given"])[-1])
    if on_step:
        on_step("drivers", "done", "%d driver(s) compiled, signed and packed" % len(cats))
    return {"ok": True, "out": out, "files": files, "problem": ""}
