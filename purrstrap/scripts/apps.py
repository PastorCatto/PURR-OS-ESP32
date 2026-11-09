"""Scaffold and build a PURR `.cat` app project: AppSDK/SPEC.md.

`new` copies a starter template (AppSDK/templates/<template>/) into a fresh project folder.
`build` reads that project's manifest.toml and reuses modules.py's own compile/relocate/pack
pipeline with --kind app (AppSDK/SPEC.md section 3: "nobody has to run the compiler by hand"),
the same pipeline `modules build --kind app` already uses directly.

Loads modules.py the same way the registry loads every subscript (importlib, by file path, not
a package import -- scripts/ has no __init__.py) because this genuinely depends on it: an app
IS built by modules.py's pipeline, not an independent thing that happens to look similar.
"""

import importlib.util
import os
import re
import shutil
import tomllib

from lib.model import Action, Param, Script

_SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")

# What the manifest may say for these fields (AppSDK/SPEC.md section 5) -- checked at build
# time only; nothing downstream enforces them yet (that spec's own note on what reaches the
# signed .cat today).
VALID_CLASSES = ("unprivileged", "privileged")
VALID_KINDS = ("daemon", "command-line", "ui", "game")


def _load_modules_script():
    path = os.path.join(_SCRIPTS_DIR, "modules.py")
    spec = importlib.util.spec_from_file_location("purrstrap_apps_modules_dep", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# Loaded eagerly, not lazily inside an action: an app's build is genuinely modules.py's own
# pipeline (--kind app), not an independent implementation that happens to look similar, so if
# modules.py is broken, apps.py being unusable too is the honest outcome, not a violation of
# "a broken subscript never breaks the tool" (that principle is about isolating unrelated
# subscripts, not hiding a real dependency).
_modules = _load_modules_script()


def _templates_dir(repo_root):
    return os.path.join(repo_root, "AppSDK", "templates")


def _available_templates(repo_root):
    d = _templates_dir(repo_root)
    if not os.path.isdir(d):
        return ()
    return tuple(sorted(n for n in os.listdir(d) if os.path.isdir(os.path.join(d, n))))


def new(ctx, name, template, out):
    if not NAME_RE.match(name):
        ctx.error(f"{name!r}: app names must be a C identifier (letters, digits, underscore, "
                 "not starting with a digit)")
        return 1
    templates = _available_templates(ctx.repo_root)
    if template not in templates:
        ctx.error(f"unknown template {template!r}. Known: {', '.join(templates) or '(none)'}")
        return 1

    dest = os.path.join(ctx.repo_root, out or "Apps", name)
    if os.path.exists(dest):
        ctx.error(f"{dest}: already exists")
        return 1

    src = os.path.join(_templates_dir(ctx.repo_root), template)
    shutil.copytree(src, dest)

    # The template's own placeholder is the literal name "myapp" (manifest.toml's name/entry,
    # src/app.c's entry symbol and log string) -- one substring replace handles all of them,
    # since "myapp_entry" is just "myapp" + "_entry".
    for root, _dirs, files in os.walk(dest):
        for fname in files:
            path = os.path.join(root, fname)
            with open(path, "r", encoding="utf-8") as fh:
                text = fh.read()
            text = text.replace("myapp", name)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)

    ctx.ok(f"{dest}: new {template} app {name!r} (entry {name}_entry)")
    ctx.info(f"build it: purrstrap apps build --project {os.path.relpath(dest, ctx.repo_root)}")
    return 0


def build(ctx, project, board, out):
    manifest_path = os.path.join(project, "manifest.toml")
    if not os.path.isfile(manifest_path):
        ctx.error(f"{manifest_path}: no such file")
        return 1
    try:
        with open(manifest_path, "rb") as fh:
            manifest = tomllib.load(fh)
    except tomllib.TOMLDecodeError as exc:
        ctx.error(f"{manifest_path}: {exc}")
        return 1

    missing = [k for k in ("name", "version", "entry") if k not in manifest]
    if missing:
        ctx.error(f"{manifest_path}: missing required field(s): {', '.join(missing)}")
        return 1
    name, version, entry = manifest["name"], manifest["version"], manifest["entry"]

    app_class = manifest.get("class", "unprivileged")
    if app_class not in VALID_CLASSES:
        ctx.error(f"{manifest_path}: class {app_class!r} must be one of {VALID_CLASSES}")
        return 1
    kind = manifest.get("kind", "command-line")
    if kind not in VALID_KINDS:
        ctx.error(f"{manifest_path}: kind {kind!r} must be one of {VALID_KINDS}")
        return 1
    # Validated above but not yet carried into the signed image (AppSDK/SPEC.md section 5's
    # own note on what purr_abi.h's header has room for today).
    ctx.info(f"class={app_class} kind={kind} "
             f"permissions={manifest.get('permissions', {}).get('requests', [])}")

    src_dir = os.path.join(project, "src")
    sources = sorted(
        os.path.join(src_dir, f) for f in os.listdir(src_dir) if f.endswith(".c")
    ) if os.path.isdir(src_dir) else []
    if not sources:
        ctx.error(f"{src_dir}: no .c files found")
        return 1

    return _modules.build(ctx, board, ",".join(sources), entry, "app", name, version, out)


SCRIPT = Script(
    name="apps",
    title="Apps",
    description="Scaffold and build a PURR .cat app project (AppSDK/SPEC.md).",
    actions=(
        Action("new", "New app", new,
               help="scaffold a project folder from a starter template",
               params=(
                   Param("name", "str", required=True,
                         help='the app\'s name, e.g. "weather" (must be a C identifier)'),
                   Param("template", "choice", default="unprivileged",
                         choices=tuple(_available_templates(
                             os.path.dirname(os.path.dirname(_SCRIPTS_DIR)))),
                         help="which starter template to copy"),
                   Param("out", "path", default="",
                         help="parent folder for the new project (default: Apps)"),
               )),
        Action("build", "Build", build,
               help="read a project's manifest.toml and build it (reuses modules.py's "
                    "compile/relocate/pack pipeline with --kind app)",
               params=(
                   Param("project", "path", required=True,
                         help="the project folder (holds manifest.toml and src/)"),
                   Param("board", "choice", default="tdeck_plus",
                         choices=tuple(_modules.BOARDS),
                         help="the board to build for (it decides the chip and compiler)"),
                   Param("out", "path", default="",
                         help="output .cat file (default: Apps/build/<name>/<name>.cat)"),
               )),
    ),
)
