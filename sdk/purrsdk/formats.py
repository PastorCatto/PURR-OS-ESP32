"""Loads purrstrap's format modules (image container, purrcfg) by path, so the SDK reads and
writes exactly what the build tool does, from a repository checkout or from inside the kit."""

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def purrstrap_dir():
    """Where purrstrap's `lib` lives: the kit (lib/purrstrap) or the repository (purrstrap)."""
    env = os.environ.get("PURR_PURRSTRAP")
    candidates = [env] if env else []
    candidates += [os.path.join(HERE, "..", "purrstrap"),               # kit: lib/purrsdk next to lib/purrstrap
                   os.path.join(HERE, "..", "..", "purrstrap")]         # repository: sdk/purrsdk
    for c in candidates:
        c = os.path.abspath(c)
        if os.path.isfile(os.path.join(c, "lib", "image.py")):
            return c
    raise RuntimeError("cannot find purrstrap (set PURR_PURRSTRAP to its folder)")


def _load(alias, relative):
    if alias in sys.modules:
        return sys.modules[alias]
    path = os.path.join(purrstrap_dir(), relative)
    spec = importlib.util.spec_from_file_location(alias, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[alias] = mod
    spec.loader.exec_module(mod)
    return mod


def image():
    return _load("purrsdk_image", os.path.join("lib", "image.py"))


def purrcfg():
    return _load("purrsdk_purrcfg", os.path.join("lib", "purrcfg.py"))
