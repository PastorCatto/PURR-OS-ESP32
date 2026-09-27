"""What a subscript gets to work with: paths, output, a process runner."""

import json
import os
import subprocess
import sys


class Context:
    def __init__(self, repo_root, tool_dir, out=None):
        self.repo_root = repo_root
        self.tool_dir = tool_dir
        self.out = out or sys.stdout
        self._color = getattr(self.out, "isatty", lambda: False)()
        self.state_path = os.path.join(tool_dir, ".state.json")

    # -- output ---------------------------------------------------------
    def _say(self, prefix, text, code):
        if self._color and code:
            prefix = f"\033[{code}m{prefix}\033[0m"
        print(f"{prefix} {text}" if prefix else text, file=self.out)

    def info(self, text):  self._say("", text, "")
    def ok(self, text):    self._say("[ok]", text, "92")
    def warn(self, text):  self._say("[warn]", text, "93")
    def error(self, text): self._say("[error]", text, "91")

    # -- processes ------------------------------------------------------
    def run(self, argv, cwd=None, log_path=None, env=None):
        """Run a command, streaming output (and optionally logging it).

        Returns the exit code, or 127 if the program cannot be started.
        """
        log = None
        try:
            if log_path:
                os.makedirs(os.path.dirname(log_path), exist_ok=True)
                log = open(log_path, "w", encoding="utf-8")
            try:
                proc = subprocess.Popen(
                    argv, cwd=cwd, env=env, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True,
                    encoding="utf-8", errors="replace")
            except OSError as exc:
                self.error(f"cannot run {argv[0]}: {exc}")
                return 127
            for line in proc.stdout:
                print(line, end="", file=self.out)
                if log:
                    log.write(line)
            return proc.wait()
        finally:
            if log:
                log.close()

    # -- remembered values (TUI prefill only) ---------------------------
    def load_state(self):
        try:
            with open(self.state_path, encoding="utf-8") as fh:
                data = json.load(fh)
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def save_state(self, state):
        try:
            with open(self.state_path, "w", encoding="utf-8") as fh:
                json.dump(state, fh, indent=2)
        except OSError:
            pass  # remembered values are a convenience, never required
