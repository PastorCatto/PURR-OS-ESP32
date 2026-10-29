"""Where a person's things live: projects, keys and builds, in one folder (default ~/PURR-Projects)."""

import os

from . import formats, runner


def default_dir():
    return os.environ.get("PURR_WORKSPACE") or os.path.join(os.path.expanduser("~"), "PURR-Projects")


class Workspace:
    def __init__(self, path=None):
        self.path = os.path.abspath(path or default_dir())

    @property
    def projects(self):
        return os.path.join(self.path, "projects")

    @property
    def keys(self):
        return os.path.join(self.path, "keys")

    @property
    def builds(self):
        return os.path.join(self.path, "builds")

    @property
    def repo(self):
        return os.path.join(self.path, "devrepo")

    def ensure(self):
        for d in (self.projects, self.keys, self.builds, self.repo):
            os.makedirs(d, exist_ok=True)
        return self


def kit_root():
    return runner.root()


def kit_path(*parts):
    return os.path.join(kit_root(), *parts)


def sdk_path(*parts):
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), *parts)
