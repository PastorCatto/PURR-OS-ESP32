"""The dev server: hosts apps, drivers and recovery images to a device on your network (purrstrap's `dev serve`), run as a child process
so Studio can start it, watch it and stop it."""

import os
import subprocess
import sys
import threading

from . import formats, runner

DEFAULT_PORT = 47801


class DevServer:
    def __init__(self):
        self.proc = None
        self.lines = []
        self.folder = None
        self.port = None
        self._lock = threading.Lock()

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self, folder, port=DEFAULT_PORT, name="", bind="0.0.0.0"):
        if self.running():
            return {"ok": False, "text": "The server is already running on port %s." % self.port}
        os.makedirs(folder, exist_ok=True)
        exe = os.environ.get("PURR_PYTHON") or sys.executable
        cmd = [exe, os.path.join(formats.purrstrap_dir(), "purrstrap.py"), "dev", "serve", "--dir", folder, "--port", str(int(port)), "--bind", bind]
        if name:
            cmd += ["--name", name]
        env = dict(os.environ, PYTHONUNBUFFERED="1")
        try:
            self.proc = subprocess.Popen(cmd, cwd=runner.root(), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         universal_newlines=True, errors="replace", stdin=subprocess.DEVNULL)
        except OSError as e:
            return {"ok": False, "text": "Could not start the server: %s" % e}
        self.folder, self.port, self.lines = folder, int(port), []
        threading.Thread(target=self._pump, args=(self.proc,), daemon=True).start()
        return {"ok": True, "text": "Serving %s on port %d." % (folder, self.port)}

    def _pump(self, proc):
        for line in proc.stdout:
            with self._lock:
                self.lines.append(line.rstrip("\n"))
                del self.lines[:-300]

    def stop(self):
        if self.running():
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        return {"ok": True, "text": "The server is stopped."}

    def status(self):
        with self._lock:
            tail = list(self.lines[-60:])
        return {"running": self.running(), "folder": self.folder, "port": self.port, "log": tail}
