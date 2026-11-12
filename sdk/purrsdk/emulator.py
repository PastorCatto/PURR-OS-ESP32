"""The emulator inside the kit: where it is, how it is started, and how `purr run` drives it.

The emulator is a separate project (a Rust shell around a patched QEMU for the ESP32-S3). The kit carries a built copy in `emulator/`;
a developer checkout of the emulator works too. `purr emulator` opens the window. `purr run` starts it without a window, so a script
can press the screen, take pictures and read the serial log: that is what the `Headless` class talks to (the emulator's `headless_tap`
program reads one command per line from a file).

Commands the headless program understands (one per line, appended to the command file while it runs):
    tap X Y            press the screen at X,Y (the screen is 320 x 240)
    drag X1 Y1 X2 Y2   press, slide and let go
    shot NAME          write <stem>-NAME.png
    dump               write <stem>-serial.txt (everything the board printed so far)
    type TEXT          press keys (the keyboard model, not a text field)
    quit               stop
The program does no waiting of its own between commands, so the caller paces them (the emulated board is slower than the clock).
"""

import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time

from . import workspace

DEFAULT_BOARD = "t-deck-plus-vm"          # the board with the VM hardware: the Ethernet model, so the guest reaches the host at 10.0.2.2
HOST_ADDRESS = "10.0.2.2"                  # where the guest sees the computer running the emulator (QEMU's user-mode network)
SCREEN = (320, 240)

_WINDOWS = os.name == "nt"
# The launcher first: it takes the board as an argument, the bare app does not read BOARD_TOML. On Windows the launcher is a .cmd for people; the SDK starts
# the program itself (it is given the board file explicitly, and finds its QEMU in vendor/qemu/bin next to it).
_GUI_NAMES = ("esp32-emulator.exe",) if _WINDOWS else ("run-emulator", "esp32-emulator")
_HEADLESS_NAMES = ("headless_tap.exe",) if _WINDOWS else ("headless_tap",)
_SUBDIRS = ("", "bin", "target/release", "target/release/examples", "target/debug", "target/debug/examples")


class EmulatorError(Exception):
    pass


def _dev_checkout():
    p = os.path.join(os.path.expanduser("~"), "Projects", "ESP32-Emulator")
    return p if os.path.isdir(os.path.join(p, "boards")) else None


def _roots():
    seen = []
    for r in (os.environ.get("PURR_EMULATOR_DIR"), workspace.kit_path("emulator"), _dev_checkout()):
        if r and os.path.isdir(r) and r not in seen:
            seen.append(r)
    return seen


def _find_exe(root, names):
    for sub in _SUBDIRS:
        for n in names:
            p = os.path.join(root, sub, n)
            if os.path.isfile(p) and os.access(p, os.X_OK):
                return p
    return None


def find():
    """Where the emulator is. Returns {"root", "gui", "headless", "boards"} (a path or None for each part), or None when there is no emulator.
    Looked for in: $PURR_EMULATOR_DIR, the kit's emulator/ folder, a checkout of the emulator project. $PURR_EMULATOR names a launcher for the window."""
    launcher = os.environ.get("PURR_EMULATOR")
    for root in _roots():
        gui = launcher if launcher and os.path.isfile(launcher) else _find_exe(root, _GUI_NAMES)
        headless = _find_exe(root, _HEADLESS_NAMES)
        boards = os.path.join(root, "boards")
        if gui or headless:
            return {"root": root, "gui": gui, "headless": headless, "boards": boards if os.path.isdir(boards) else None}
    if launcher and os.path.isfile(launcher):
        return {"root": os.path.dirname(launcher), "gui": launcher, "headless": None, "boards": None}
    return None


def vm_image():
    """The VM flash image to boot: $PURR_VM_IMAGE, or the kit's images/vm-flash.bin. None when there is none."""
    for p in (os.environ.get("PURR_VM_IMAGE"), workspace.kit_path("images", "vm-flash.bin")):
        if p and os.path.isfile(p):
            return p
    return None


def board_file(em, board=DEFAULT_BOARD):
    if os.path.isfile(board):
        return os.path.abspath(board)
    if em and em.get("boards"):
        p = os.path.join(em["boards"], board if board.endswith(".toml") else board + ".toml")
        if os.path.isfile(p):
            return p
    raise EmulatorError("I cannot find the board file %r. The emulator's boards/ folder lists the ones it knows." % board)


def _scratch_copy(image, workdir):
    """The emulator writes to the flash image it boots, so it always gets a copy: the original is never touched."""
    os.makedirs(workdir, exist_ok=True)
    dst = os.path.join(workdir, "flash.bin")
    shutil.copyfile(image, dst)
    return dst


def open_window(image=None, board=DEFAULT_BOARD, workdir=None):
    """Start the emulator's window on a copy of the image. Returns the child process (the caller decides whether to wait for it)."""
    em = find()
    if not em or not em["gui"]:
        raise EmulatorError("There is no emulator in this kit. A kit built with the emulator has an emulator/ folder; set PURR_EMULATOR_DIR to use another.")
    image = image or vm_image()
    if not image or not os.path.isfile(image):
        raise EmulatorError("I need a flash image to boot: pass one, or put the VM image at images/vm-flash.bin (or set PURR_VM_IMAGE).")
    work = workdir or tempfile.mkdtemp(prefix="purr-emulator-")
    flash = _scratch_copy(image, work)
    board_path = board_file(em, board)
    env = dict(os.environ, BOARD_TOML=board_path)
    cmd = [em["gui"], flash, board_path] + (["--run"] if os.path.splitext(os.path.basename(em["gui"]))[0] == "esp32-emulator" else [])    # the app classifies files like a drop
    return subprocess.Popen(cmd, cwd=em["root"], env=env)


class _WinJob:
    """A Windows job object holding the headless runner and everything it starts (QEMU is its child). Windows has no process groups to signal:
    ending the job ends exactly the processes this session started, never any other. KILL_ON_JOB_CLOSE also takes them if this program dies first."""

    def __init__(self, proc):
        import ctypes
        from ctypes import wintypes
        self._ct = ctypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = wintypes.HANDLE
        k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        k32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        k32.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        self._k32 = k32

        class Basic(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wintypes.DWORD),
                        ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

        class IoCounters(ctypes.Structure):
            _fields_ = [(n, ctypes.c_ulonglong) for n in ("Read", "Write", "Other", "ReadBytes", "WriteBytes", "OtherBytes")]

        class Extended(ctypes.Structure):
            _fields_ = [("Basic", Basic), ("Io", IoCounters), ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                        ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

        class Accounting(ctypes.Structure):
            _fields_ = [("TotalUserTime", ctypes.c_int64), ("TotalKernelTime", ctypes.c_int64), ("ThisPeriodUserTime", ctypes.c_int64),
                        ("ThisPeriodKernelTime", ctypes.c_int64), ("TotalPageFaultCount", wintypes.DWORD), ("TotalProcesses", wintypes.DWORD),
                        ("ActiveProcesses", wintypes.DWORD), ("TotalTerminatedProcesses", wintypes.DWORD)]
        self._Accounting = Accounting
        self.handle = k32.CreateJobObjectW(None, None)
        if not self.handle:
            raise OSError("CreateJobObject failed (%d)" % ctypes.get_last_error())
        info = Extended()
        info.Basic.LimitFlags = 0x2000                                  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not k32.SetInformationJobObject(self.handle, 9, ctypes.byref(info), ctypes.sizeof(info)):       # JobObjectExtendedLimitInformation
            self.close()
            raise OSError("SetInformationJobObject failed (%d)" % ctypes.get_last_error())
        if not k32.AssignProcessToJobObject(self.handle, wintypes.HANDLE(int(proc._handle))):
            err = ctypes.get_last_error()
            self.close()
            raise OSError("AssignProcessToJobObject failed (%d)" % err)

    def active(self):
        """How many processes of the job are still running."""
        if not self.handle:
            return 0
        acc = self._Accounting()
        if not self._k32.QueryInformationJobObject(self.handle, 1, self._ct.byref(acc), self._ct.sizeof(acc), None):         # JobObjectBasicAccountingInformation
            return 0
        return int(acc.ActiveProcesses)

    def terminate(self):
        if self.handle:
            self._k32.TerminateJobObject(self.handle, 1)

    def close(self):
        if getattr(self, "handle", None):
            self._k32.CloseHandle(self.handle)
            self.handle = None


class Headless:
    """One headless run of the emulator, driven through a command file."""

    def __init__(self, image=None, board=DEFAULT_BOARD, workdir=None, seconds=1800, exe=None, root=None):
        em = find()
        self.exe = exe or (em or {}).get("headless")
        self.root = root or (em or {}).get("root") or os.getcwd()
        if not self.exe:
            raise EmulatorError("The emulator's headless runner is not in this kit (no headless_tap). `purr emulator` opens the window instead.")
        image = image or vm_image()
        if not image or not os.path.isfile(image):
            raise EmulatorError("I need a flash image to boot: pass one, or put the VM image at images/vm-flash.bin (or set PURR_VM_IMAGE).")
        self.image = image
        self.board = board_file(em, board) if (em or os.path.isfile(board)) else board
        self.workdir = workdir or tempfile.mkdtemp(prefix="purr-run-")
        self.seconds = int(seconds)
        self.proc = None
        self.cmd_file = os.path.join(self.workdir, "cmd.txt")
        self.stem = os.path.join(self.workdir, "s")
        self.log = os.path.join(self.workdir, "run.log")

    # -- lifecycle
    def start(self):
        flash = _scratch_copy(self.image, self.workdir)
        with open(self.cmd_file, "w"):
            pass
        # The runner works on its own copy of the flash image in $TMPDIR/esp32-emulator-headless-tap/, one for the whole computer: a second run overwrites the first's.
        # Each session gets a temp folder of its own, so two can run side by side, and the guest's writes stay readable after the run (self.guest_flash()).
        self.tmpdir = os.path.join(self.workdir, "tmp")
        os.makedirs(self.tmpdir, exist_ok=True)
        env = dict(os.environ, BOARD_TOML=self.board, HEADLESS_CMD=self.cmd_file, TMPDIR=self.tmpdir)
        if _WINDOWS:
            env.update(TMP=self.tmpdir, TEMP=self.tmpdir)               # Windows takes its temp folder from TMP/TEMP, not TMPDIR
            own = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        else:
            own = {"start_new_session": True}                           # its own process group: QEMU is a child, and stopping must take it too
        with open(self.log, "w") as lg:
            self.proc = subprocess.Popen([self.exe, flash, str(self.seconds), self.stem + ".png"], cwd=self.root, env=env,
                                         stdout=lg, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **own)
        self.pgid = self.proc.pid
        self._job = None
        if _WINDOWS:                                                    # the Windows equivalent of the group: a job object holding the runner and its children
            try:
                self._job = _WinJob(self.proc)
            except OSError:
                self._job = None                                        # then stop() falls back to taskkill on the runner's tree
        return self

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def guest_flash(self):
        """The flash image as the guest has changed it (accounts, settings, installed apps): the runner's working copy in this session's temp folder, or None."""
        p = os.path.join(getattr(self, "tmpdir", ""), "esp32-emulator-headless-tap", "flash.bin")
        return p if os.path.isfile(p) else None

    def stop(self, grace=15):
        """Ask it to quit; if it does not, stop the process this object started (never any other)."""
        if self.proc is None:
            return
        if self.running():
            try:
                self.send("quit")
            except OSError:
                pass
            end = time.time() + grace
            while self.running() and time.time() < end:
                time.sleep(0.2)
        self._reap_group()
        self.proc = None

    def _group_alive(self):
        if _WINDOWS:
            job = getattr(self, "_job", None)
            return (job.active() > 0) if job else self.running()
        try:
            os.killpg(self.pgid, 0)
            return True
        except OSError:
            return False

    def _reap_job(self):
        """Windows: the runner starts QEMU as its child, inside this session's job. If the runner is gone but QEMU is not, the job
        (only the one this object started, never another process) is ended."""
        job = getattr(self, "_job", None)
        if job is not None:
            if job.active() > 0:
                job.terminate()
                end = time.time() + 5
                while time.time() < end and job.active() > 0:
                    time.sleep(0.1)
            job.close()
            self._job = None
        elif self.running():                                           # no job (it could not be made): end the runner and what it started
            subprocess.run(["taskkill", "/PID", str(self.proc.pid), "/T", "/F"], capture_output=True)
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass

    def _reap_group(self):
        """The runner starts QEMU as its child, in the same new process group. If the runner is gone but QEMU is not, the group
        (only the one this object started, never another) is stopped: first politely, then hard."""
        if _WINDOWS:
            return self._reap_job()
        pgid = getattr(self, "pgid", None)
        if pgid and pgid != os.getpgrp():
            for sig in (signal.SIGTERM, signal.SIGKILL):
                if not self._group_alive():
                    break
                try:
                    os.killpg(pgid, sig)
                except OSError:
                    break
                end = time.time() + 3
                while time.time() < end and self._group_alive():
                    time.sleep(0.1)
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    # -- commands
    def send(self, command, wait=0.0):
        with open(self.cmd_file, "a") as fh:
            fh.write(command.strip() + "\n")
        if wait:
            time.sleep(wait)

    def tap(self, x, y, wait=8.0):
        self.send("tap %d %d" % (x, y), wait)

    def drag(self, x1, y1, x2, y2, wait=8.0):
        self.send("drag %d %d %d %d" % (x1, y1, x2, y2), wait)

    def _wait_file(self, path, since, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if os.path.getmtime(path) >= since and os.path.getsize(path) > 0:
                    return True
            except OSError:
                pass
            if not self.running():
                break
            time.sleep(0.2)
        return os.path.isfile(path) and os.path.getmtime(path) >= since

    def shot(self, name, timeout=30):
        """Take a picture of the screen. Returns the path of the PNG, or raises when the emulator did not write one."""
        path = "%s-%s.png" % (self.stem, name)
        since = time.time() - 0.5
        self.send("shot " + name)
        if not self._wait_file(path, since, timeout):
            raise EmulatorError("The emulator did not take the picture %r (is it still running? see %s)." % (name, self.log))
        return path

    def serial(self, timeout=30):
        """Everything the board has printed so far, as text."""
        path = self.stem + "-serial.txt"
        since = time.time() - 0.5
        self.send("dump")
        if not self._wait_file(path, since, timeout):
            return ""
        with open(path, "r", errors="replace") as fh:
            return fh.read()

    def wait_serial(self, text, timeout=300, poll=10):
        """Wait until the board has printed `text`. Returns True when it did, False on timeout or when the emulator stopped."""
        end = time.time() + timeout
        while time.time() < end:
            if text in self.serial():
                return True
            if not self.running():
                return False
            time.sleep(poll)
        return False

    def panicked(self):
        s = self.serial()
        return "Guru Meditation Error" in s or "Backtrace:" in s


def run_steps(session, steps, on_step=None):
    """Run a list of steps on a Headless session. A step is ("tap", x, y, wait), ("drag", x1, y1, x2, y2, wait), ("shot", name),
    ("wait_serial", text, timeout) or ("pause", seconds). Returns the list of pictures taken. Stops with EmulatorError on a failed wait or a panic."""
    pictures = []
    for st in steps:
        kind = st[0]
        if on_step:
            on_step(st)
        if kind == "tap":
            session.tap(st[1], st[2], st[3] if len(st) > 3 else 8.0)
        elif kind == "drag":
            session.drag(st[1], st[2], st[3], st[4], st[5] if len(st) > 5 else 8.0)
        elif kind == "shot":
            pictures.append(session.shot(st[1]))
        elif kind == "wait_serial":
            if not session.wait_serial(st[1], st[2] if len(st) > 2 else 300):
                raise EmulatorError("The board never printed %r (see %s)." % (st[1], session.log))
        elif kind == "pause":
            time.sleep(st[1])
        else:
            raise EmulatorError("Unknown step %r." % (kind,))
        if kind in ("tap", "drag") and session.running() is False:
            raise EmulatorError("The emulator stopped while I was working (see %s)." % session.log)
    return pictures


# ---------------------------------------------------------------------------------------------------- purr run
# The steps that put an app from the dev server onto the VM through the Developer Portal, for the Android-style shell image
# (the coordinates are the 320 x 240 screen; the waits are seconds of real time, because the emulated board is slower than the clock).
# They were worked out by hand on the 1.0.27 image; `purr run --window` is the way to do it by eye on any other shell.
BOOT_DONE = "store: the Store and its companion are installed"          # the last line of start-up in the full profile
ANDROID_START = [
    ("wait_serial", "android-home:", 300),
    ("pause", 10),
    ("shot", "01-home"),
    ("drag", 287, 207, 287, 50, 24),               # slide the lock handle up
    ("tap", 160, 184, 12),                         # the apps icon in the dock
    ("tap", 45, 70, 24),                           # Developer Portal in the drawer
    ("tap", 100, 191, 14),                         # "Tap to find or enter one": the SDK servers page
    ("shot", "02-servers"),
]
# Worked by hand on the same image, one step at a time; not yet run as a script, because the emulator's touch panel panics when a press arrives
# right after the screen's idle switch-off (reported to the emulator's maintainer). Keep the steps close together in time.
ANDROID_PORTAL = [
    ("tap", 68, 125, 24),                          # Find servers
    ("tap", 100, 165, 22),                         # the server that was found
    ("shot", "03-trust"),
    ("tap", 42, 187, 28),                          # Trust
    ("shot", "04-apps"),
    ("drag", 160, 190, 160, 125, 10),              # scroll the page a little at a time (a long drag overshoots)
    ("drag", 160, 190, 160, 100, 10),
    ("shot", "05-available"),
    ("tap", 270, 147, 30),                         # Install on the first app in the list
    ("shot", "06-installed"),
]


def _port_busy(port):
    """True when something on this computer already answers on the port."""
    sock = socket.socket()
    sock.settimeout(0.5)
    try:
        return sock.connect_ex(("127.0.0.1", int(port))) == 0
    finally:
        sock.close()


def serve_folder(folder, port=47801, wait=10):
    """Start the dev server on a folder (a child process); returns the DevServer. The guest reaches it at 10.0.2.2:PORT.
    Refuses a port that is already taken (the emulated device would talk to that server, not yours), and waits until the new server answers."""
    from . import devserver
    if _port_busy(port):
        raise EmulatorError("Port %d is already in use, probably by another dev server. The emulated device would reach that one instead of "
                            "the one holding your app. Stop it first (or serve your build from there)." % port)
    srv = devserver.DevServer()
    r = srv.start(folder, port=port, name="purr run")
    if not r["ok"]:
        raise EmulatorError(r["text"])
    end = time.time() + wait
    while time.time() < end:
        if not srv.running():
            break
        if _port_busy(port):
            return srv
        time.sleep(0.2)
    log = " ".join(srv.status()["log"][-3:])
    srv.stop()
    raise EmulatorError("The dev server did not start on port %d. %s" % (port, log))


def make_repo(cat_file, folder):
    """A throwaway repository with just this app in it, so it is the only entry in the Portal's list."""
    from . import runner
    os.makedirs(folder, exist_ok=True)
    res = runner.purrstrap(["dev", "publish", "--file", cat_file, "--dir", folder])
    if res["rc"] != 0:
        raise EmulatorError("Could not publish %s into the throwaway repository." % os.path.basename(cat_file))
    return folder


def run_app(cat_file, image=None, board=DEFAULT_BOARD, shots=None, port=47801, headless=True, on_line=print, steps=None):
    """Put a built, signed app on the VM and show it. With headless=True the emulator runs without a window and the pictures go to `shots`;
    otherwise the window opens and the instructions are printed. Returns {"shots": [...], "ok": bool}."""
    work = tempfile.mkdtemp(prefix="purr-run-")
    repo = make_repo(cat_file, os.path.join(work, "repo"))
    srv = serve_folder(repo, port)
    try:
        on_line("Serving your app at http://%s:%d (the emulated device reaches this computer at that address)." % (HOST_ADDRESS, port))
        if not headless:
            proc = open_window(image, board, os.path.join(work, "vm"))
            on_line("The emulator is open. On the device: unlock, open Developer Portal, Find servers, Trust, then Install your app.")
            on_line("Close the emulator window to finish.")
            proc.wait()
            return {"ok": True, "shots": []}
        out = shots or os.path.join(work, "shots")
        os.makedirs(out, exist_ok=True)
        pics = []
        with Headless(image, board, os.path.join(work, "vm")) as h:
            pics = run_steps(h, steps if steps is not None else ANDROID_START + ANDROID_PORTAL, on_step=lambda s: on_line("  " + " ".join(str(x) for x in s)))
            if h.panicked():
                raise EmulatorError("The board crashed while it ran (see %s)." % h.log)
            kept = []
            for p in pics:
                dst = os.path.join(out, os.path.basename(p))
                shutil.copyfile(p, dst)
                kept.append(dst)
        return {"ok": True, "shots": kept}
    finally:
        srv.stop()


# ---------------------------------------------------------------------------------------------------- test a built image
# What a built image does when it starts in the emulator, read from its serial log and its screen. The stages, from the log lines the firmware prints (checked on a real
# build of 1.1 in the emulator): the bootloader, the boot package it verifies, the system, then wherever the system gets to.
EMU_BOARDS = {"tdeck_plus": "t-deck-plus", "tdeck": "t-deck-plus"}      # the T-Deck has the same screen and keys; the e-paper board is not emulated
STAGES = (("android-home:", "the home screen"), ("oobe", "the setup wizard"), ("wifi_init", "the recovery loader, starting the network (a blank device installs the rest from the internet)"))
CRASH = ("Guru Meditation Error", "Backtrace:", "abort() was called", "assert failed", "panic'ed", "Rebooting...")


def emulator_board_for(board):
    """The emulator's board file for a PURR board, or None when this board cannot be emulated."""
    return EMU_BOARDS.get(board)


def clean_serial(text):
    """The serial dump without ANSI colours and without the duplicate copy the USB Serial/JTAG port prints (the dump has one section per port)."""
    text = re.sub(r"--- USB Serial/JTAG ---\n.*?(?=--- UART0 ---|\Z)", "", text, flags=re.S)
    return re.sub(r"\x1b\[[0-9;]*m", "", text)


def lit_fraction(png_path):
    """How much of a screenshot is not black (0 to 1). A screen with nothing on it is the sign of a board that never drew."""
    from . import uimaker
    w, h, rgba = uimaker.read_png(png_path)
    n = w * h
    lit = sum(1 for i in range(n) if rgba[i * 4 + 3] and (rgba[i * 4] > 24 or rgba[i * 4 + 1] > 24 or rgba[i * 4 + 2] > 24))
    return lit / float(n or 1)


def available(board="tdeck_plus"):
    """(True, "") when a built image for this board can be tested here, else (False, why in plain words)."""
    if emulator_board_for(board) is None:
        return False, "The %s board is not emulated yet." % board
    em = find()
    if em is None or not em["headless"]:
        return False, "There is no emulator in this kit. A kit built with the emulator has an emulator/ folder; set PURR_EMULATOR_DIR to use another."
    return True, ""


def test_image(image, board="tdeck_plus", seconds=120, workdir=None, on_check=None, on_line=print, poll=4):
    """Start a built flash image in the emulator without a window and see what it does. Never touches the image (the emulator works on a copy).
    Returns {"ok", "checks": [{"id", "status", "text"}], "stage", "shots": [png paths], "seconds", "tail": [last lines of the log]}; ok is False only when a check failed
    (a crash, a boot loop, a boot package that did not verify, a board that never started)."""
    ok_, why = available(board)
    if not ok_:
        raise EmulatorError(why)
    if not os.path.isfile(image):
        raise EmulatorError("I cannot find the image %s." % image)
    work = workdir or tempfile.mkdtemp(prefix="purr-test-")
    checks = []

    def check(i, status, text):
        checks.append({"id": i, "status": status, "text": text})
        if on_check:
            on_check(i, status, text)

    t0 = time.time()
    shots, lit, stage, stage_at, serial = [], [], None, None, ""
    with Headless(image, emulator_board_for(board), work, seconds=seconds + 90) as h:
        on_line("The emulator is starting the image (the board takes about a minute to settle).")
        n = 0
        while time.time() - t0 < seconds:
            time.sleep(poll)
            if not h.running():
                break
            serial = clean_serial(h.serial(timeout=20))
            for marker, label in STAGES:                      # the list is ordered from the furthest stage to the earliest: the first one in the log is where it got to
                if marker in serial:
                    if stage != label:
                        stage, stage_at = label, time.time()
                    break
            screen_off = "screen off" in serial
            if (n == 0 or time.time() - t0 > 12 * (n + 1)) and not screen_off and n < 4 and "purros: PURR OS starting" in serial:
                try:
                    p = h.shot("t%d" % n)
                    shots.append(p)
                    lit.append(lit_fraction(p))
                    n += 1
                except EmulatorError:
                    pass
            if stage and stage_at and time.time() - stage_at > 10 and shots:
                break
            if any(c in serial for c in CRASH):
                break
        serial = clean_serial(h.serial(timeout=30)) or serial
        alive = h.running()
    took = time.time() - t0

    boots = serial.count("purr_boot: PURR OS bootloader")
    if boots >= 1:
        check("start", "ok", "The bootloader started.")
    else:
        check("start", "bad", "The board never printed the bootloader's banner: nothing booted from this image.")
    m = re.search(r"bootpkg ([\w.-]+) loaded \(key id (\d+), ([^)]*)\)", serial)
    if m:                                               # the dump has a header between the UART and USB copies, and it can land in the middle of this line
        status = re.sub(r"\s*--- [^\n]* ---\s*", " ", m.group(3)).strip()
        m_status = status
    if m and m_status == "ok":
        check("bootpkg", "ok", "The boot package %s verified (signed by key %s)." % (m.group(1), m.group(2)))
    elif m:
        check("bootpkg", "bad", "The boot package did not verify: %s." % re.sub(r"\s+", " ", m.group(0)))
    else:
        check("bootpkg", "warn" if boots else "bad", "The boot package's check did not show in the log.")
    crashed = [c for c in CRASH if c in serial and c != "Rebooting..."]
    if crashed:
        check("crash", "bad", "The board crashed: its log says %r." % crashed[0])
    elif boots > 1:
        check("crash", "bad", "The board restarted %d times in %d seconds: a boot loop." % (boots, took))
    else:
        check("crash", "ok", "No crash and no restart.")
    if "purros: PURR OS starting" in serial:
        check("system", "ok", "The system started.")
    else:
        check("system", "bad" if boots else "warn", "The system never announced itself after the boot package.")
    if lit:
        best = max(lit)
        check("screen", "ok" if best > 0.01 else "bad", "The screen shows something (%d%% of it lit)." % round(best * 100) if best > 0.01 else "The screen stayed black.")
    else:
        check("screen", "warn", "No picture of the screen could be taken.")
    check("stage", "ok" if stage else "warn", ("It got as far as " + stage + ".") if stage else "It started but did not reach the home screen, the setup wizard or the recovery loader within %d seconds." % seconds)
    return {"ok": not any(c["status"] == "bad" for c in checks), "checks": checks, "stage": stage or "", "shots": shots, "seconds": round(took), "tail": serial.strip().split("\n")[-30:], "alive": alive}
