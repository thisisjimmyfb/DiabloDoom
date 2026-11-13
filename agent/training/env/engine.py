"""One DiabloDoom process running under -rlenv.

Protocol (see src/diablo/d_rlenv.h): commands go in on a FIFO, one per
line; JSON events come out on another FIFO, one per line; the frame
lives in a POSIX shared-memory segment. Every command is answered by
exactly one "decision" event (other events, e.g. "exit" or "version",
may come before it). The engine blocks at each decision until the next
command, so the process uses no CPU while Python thinks.
"""

from __future__ import annotations

import json
import mmap
import os
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[3]
DEFAULT_GAME = REPO / "build" / "src" / "chocolate-doom"
DEFAULT_IWAD = REPO / "wad" / "FreedoomPhase1" / "FREEDOOM1.WAD"

SHM_MAGIC = 0x42464444  # 'DDFB'
SHM_HDR = 64
SHM_PREFIX = "/diablodoom_rl_"
WORKDIR_PREFIX = "diablodoom_rl_"


class EngineError(RuntimeError):
    pass


@dataclass
class EngineConfig:
    game: str = str(DEFAULT_GAME)
    iwad: str = str(DEFAULT_IWAD)
    warp: str = "1"                 # "1" for MAP01, "1 1" for E1M1
    skill: int = 3
    extra_args: list[str] = field(default_factory=list)   # e.g. ["-rlnocombat"]
    # Directory whose files are copied into the instance's -savedir at
    # start (prepared save slots: doomsav3.dsg ... and autosaves).
    savedir_template: str | None = None
    # Parent for per-instance working directories (fast local disk; on
    # WSL2 keep this on the Linux filesystem).
    work_root: str | None = None
    step_timeout: float = 30.0      # seconds to wait for a decision
    start_timeout: float = 60.0
    # Continuous replay capture: when set, the instance saves a 15 s
    # (game time, 35 fps) segment MP4 into this directory every 525
    # tics, named diablodoom_YYYYMMDD_HHMMSS_mmm.mp4. Segments run
    # across episode resets; the partial last one is flushed on close.
    capture_dir: str | None = None
    close_timeout: float = 60.0     # QUIT waits for capture encodes


class Engine:
    """Spawn and talk to one -rlenv game process."""

    def __init__(self, cfg: EngineConfig, tag: str = "0"):
        self.cfg = cfg
        self.tag = f"{os.getpid()}_{tag}"
        self.proc: subprocess.Popen | None = None
        self.workdir: Path | None = None
        self._sfd = -1
        self._cfd = -1
        self._buf = b""
        self._shm: mmap.mmap | None = None
        self.shm_name = f"{SHM_PREFIX}{self.tag}"
        self.last_decision: dict | None = None

    # -- lifecycle -------------------------------------------------
    def start(self) -> dict:
        """Launch the game and return its first decision."""
        self.close()
        root = self.cfg.work_root or tempfile.gettempdir()
        self.workdir = Path(tempfile.mkdtemp(prefix=f"{WORKDIR_PREFIX}{self.tag}_", dir=root))
        savedir = self.workdir / "save"
        savedir.mkdir()
        if self.cfg.savedir_template:
            for f in Path(self.cfg.savedir_template).iterdir():
                if f.is_file():
                    shutil.copy2(f, savedir / f.name)
        cmd_path, state_path = self.workdir / "cmd", self.workdir / "state"
        os.mkfifo(cmd_path)
        os.mkfifo(state_path)

        args = [self.cfg.game, "-iwad", self.cfg.iwad, "-skill", str(self.cfg.skill),
                "-warp", *self.cfg.warp.split(), "-nosound", "-savedir", str(savedir),
                "-rlenv", "-cmdpipe", str(cmd_path), "-statepipe", str(state_path),
                "-shm", self.shm_name, *self.cfg.extra_args]
        if self.cfg.capture_dir:
            cap = Path(self.cfg.capture_dir).resolve()
            cap.mkdir(parents=True, exist_ok=True)
            args += ["--continuous-capture", "-capturedir", str(cap)]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        self._log = open(self.workdir / "log.txt", "wb")
        self.proc = subprocess.Popen(args, cwd=self.workdir, env=env, stdout=self._log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        # Non-blocking read end: lets the engine's blocking open of the
        # write end complete. O_RDWR on the command FIFO never blocks.
        self._sfd = os.open(state_path, os.O_RDONLY | os.O_NONBLOCK)
        self._cfd = os.open(cmd_path, os.O_RDWR)
        self._buf = b""
        d, _ = self._read_until_decision(self.cfg.start_timeout)
        self._map_shm()
        return d

    def close(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.write(self._cfd, b"QUIT\n")
                self.proc.wait(timeout=self.cfg.close_timeout if self.cfg.capture_dir else 3)
            except (OSError, subprocess.TimeoutExpired):
                self._kill()
        self.proc = None
        for fd in (self._sfd, self._cfd):
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
        self._sfd = self._cfd = -1
        if self._shm is not None:
            self._shm.close()
            self._shm = None
        try:
            os.unlink(f"/dev/shm{self.shm_name}")
        except FileNotFoundError:
            pass
        if getattr(self, "_log", None):
            self._log.close()
            self._log = None
        if self.workdir is not None:
            shutil.rmtree(self.workdir, ignore_errors=True)
            self.workdir = None

    def _kill(self) -> None:
        if self.proc is None:
            return
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self.proc.wait()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # -- protocol --------------------------------------------------
    def command(self, line: str) -> tuple[dict, list[dict]]:
        """Send one command; return (decision, events before it)."""
        if self.proc is None or self.proc.poll() is not None:
            raise EngineError(f"engine not running (rc={self.proc and self.proc.returncode})")
        os.write(self._cfd, (line.strip() + "\n").encode())
        return self._read_until_decision(self.cfg.step_timeout)

    def _read_until_decision(self, timeout: float) -> tuple[dict, list[dict]]:
        others: list[dict] = []
        deadline = time.monotonic() + timeout
        while True:
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                if not line.strip():
                    continue
                ev = json.loads(line)
                if ev.get("event") == "decision":
                    self.last_decision = ev
                    return ev, others
                others.append(ev)
            left = deadline - time.monotonic()
            if self.proc.poll() is not None:
                raise EngineError(f"engine exited (rc={self.proc.returncode}); log: {self.log_tail()}")
            if left <= 0:
                raise EngineError(f"no decision within {timeout}s; log: {self.log_tail()}")
            r, _, _ = select.select([self._sfd], [], [], min(left, 1.0))
            if r:
                try:
                    chunk = os.read(self._sfd, 1 << 16)
                except BlockingIOError:
                    continue
                self._buf += chunk

    def log_tail(self, n: int = 15) -> str:
        try:
            lines = (self.workdir / "log.txt").read_text(errors="replace").splitlines()
            return "\n".join(lines[-n:])
        except Exception:
            return "<no log>"

    # -- frame -----------------------------------------------------
    def _map_shm(self) -> None:
        fd = os.open(f"/dev/shm{self.shm_name}", os.O_RDONLY)
        try:
            self._shm = mmap.mmap(fd, 0, prot=mmap.PROT_READ)
        finally:
            os.close(fd)
        magic, ver, w, h = struct.unpack_from("<4I", self._shm, 0)
        if magic != SHM_MAGIC or ver != 1:
            raise EngineError(f"bad shm header {magic:#x} v{ver}")
        self.width, self.height = w, h

    def frame(self) -> tuple[np.ndarray, np.ndarray, int]:
        """(indices HxW uint8, palette 256x3 uint8, gametic). Safe to call
        at a decision: the engine is blocked, so the frame is stable."""
        m = self._shm
        seq, tic = struct.unpack_from("<2I", m, 16)
        if seq & 1:
            raise EngineError("frame read while the engine was writing it")
        w, h = self.width, self.height
        pix = np.frombuffer(m, dtype=np.uint8, count=w * h, offset=SHM_HDR).reshape(h, w).copy()
        pal = np.frombuffer(m, dtype=np.uint8, count=768, offset=SHM_HDR + w * h).reshape(256, 3).copy()
        return pix, pal, tic

    def frame_rgb(self) -> np.ndarray:
        pix, pal, _ = self.frame()
        return pal[pix]


def cleanup_stale() -> None:
    """Remove leftovers of crashed runs: orphaned shm segments and work
    directories whose owner process is gone, and their game processes
    (SPEC.md §7.4)."""
    def alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    for f in Path("/dev/shm").glob(SHM_PREFIX.lstrip("/") + "*"):
        pid = int(f.name[len(SHM_PREFIX) - 1:].split("_")[0])
        if not alive(pid):
            f.unlink(missing_ok=True)
    for d in Path(tempfile.gettempdir()).glob(WORKDIR_PREFIX + "*"):
        pid = int(d.name[len(WORKDIR_PREFIX):].split("_")[0])
        if not alive(pid):
            # Kill any game still running in it, then remove it.
            subprocess.run(["pkill", "-9", "-f", f"-cmdpipe {d}/cmd"], check=False)
            shutil.rmtree(d, ignore_errors=True)
