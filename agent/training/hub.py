"""The Hugging Face Hub copy of the checkpoint (--hub-repo, SPEC.md §7.4):
the checkpoint is pulled before training (hub_pull) and pushed back while
it runs (HubUploader), and eval.py downloads it (hub_fetch_checkpoint).
Everything goes through the `hf` CLI (logged in with `hf auth login`), so
the venv needs no extra package. Used by train.py and eval.py."""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import torch

from model import CKPT_NAME, load_latest_checkpoint

HUB_FILE = CKPT_NAME   # the same name in the repo (its history is the repo's commits)


def add_hub_args(p) -> None:
    """The --hub-* arguments, shared by train.py and eval.py."""
    p.add_argument("--hub-repo", default=None,
                   help="Hugging Face model repo (e.g. user/name): its doom-runner.pt is pulled into "
                        "--run-dir before training (resumed from, unless the run directory has a newer "
                        "one) and pushed back every --hub-every updates and at the end (uses the `hf` "
                        "CLI, logged in with `hf auth login`)")
    p.add_argument("--no-hub-download", dest="hub_download", action="store_false",
                   help="don't pull the --hub-repo checkpoint (start from --run-dir alone, e.g. fresh)")
    p.add_argument("--no-hub-upload", dest="hub_upload", action="store_false",
                   help="don't push checkpoints to --hub-repo")
    p.add_argument("--hub-every", type=int, default=200,  # 100 before 2026-11-18
                   help="push the checkpoint to --hub-repo every N updates (the end always pushes)")


def _hf() -> str:
    exe = shutil.which("hf")
    if exe is None:
        sys.exit("--hub-repo needs the Hugging Face CLI (`hf`) on PATH, logged in with `hf auth login`")
    return exe


class HubError(Exception):
    pass


def _hub_files(repo: str) -> list[str]:
    out = subprocess.run([_hf(), "models", "info", repo], capture_output=True, text=True)
    if out.returncode != 0 or "{" not in out.stdout:
        raise HubError(f"cannot read {repo}: {(out.stderr or out.stdout).strip()[-500:]}")
    info = json.loads(out.stdout[out.stdout.index("{"):])
    return [s["rfilename"] for s in info.get("siblings", [])]


def hub_files(repo: str) -> list[str]:
    """The files in the model repo. Exits when the repo can't be read, so
    a network or login problem never starts a fresh run that would then
    replace the repo's checkpoint."""
    try:
        return _hub_files(repo)
    except HubError as e:
        sys.exit(f"[hub] {e}")


def hub_fetch_checkpoint(repo: str, dest: Path) -> Path | None:
    """Download just the repo's checkpoint into dest (for eval.py).
    Returns its path, or None (with the reason printed) when the repo has
    none or can't be reached (or the hf CLI isn't on PATH)."""
    if shutil.which("hf") is None:
        print("[hub] the hf CLI isn't on PATH")
        return None
    try:
        if HUB_FILE not in _hub_files(repo):
            print(f"[hub] {repo} has no {HUB_FILE} yet")
            return None
    except HubError as e:
        print(f"[hub] {e}")
        return None
    out = subprocess.run([_hf(), "download", repo, HUB_FILE, "--local-dir", str(dest)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        print(f"[hub] cannot download {repo}/{HUB_FILE}: {(out.stderr or out.stdout).strip()[-500:]}")
        return None
    return dest / HUB_FILE


# The repo holds just the checkpoint: a run on a fresh machine resumes the
# policy and the map rotation (from the iteration) and starts each map
# from its beginning. Progress saves are never uploaded (and never kept
# across runs): a save only loads in the exact IWAD file it was made with,
# and machines may have different releases (Freedoom's maps change).
HUB_STALE = "frontier/*"   # saves an older version uploaded: removed at the next push


def hub_pull(repo: str, run_dir: Path) -> Path | None:
    """Download the repo's checkpoint (CKPT_NAME) into run_dir, so
    training resumes from it, unless run_dir's checkpoint is at that
    iteration or a newer one (then the local run wins). Returns the new
    checkpoint, or None."""
    files = hub_files(repo)
    if HUB_FILE not in files:
        print(f"[hub] {repo} has no {HUB_FILE} yet")
        return None
    dl = run_dir / "hub" / "download"
    shutil.rmtree(dl, ignore_errors=True)
    out = subprocess.run([_hf(), "download", repo, HUB_FILE, "--local-dir", str(dl)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"[hub] cannot download from {repo}: {(out.stderr or out.stdout).strip()[-500:]}")
    ckpt = torch.load(dl / HUB_FILE, map_location="cpu", weights_only=False)
    it = int(ckpt["iteration"])
    local = load_latest_checkpoint(run_dir, quiet=True)
    if local is not None and int(local["iteration"]) >= it:
        print(f"[hub] {repo} has iteration {it}; {run_dir} is at {local['iteration']}, so it is kept")
        return None
    dst = run_dir / CKPT_NAME
    os.replace(dl / HUB_FILE, dst)
    print(f"[hub] pulled iteration {it} from {repo} -> {dst}")
    return dst


# A stuck upload (2026-11-18): when the Hub closes the connection, hf
# (hf_xet) never notices and waits forever. hf prints progress whenever
# bytes move, so a log that stays quiet this long means a dead upload;
# none may run longer than UPLOAD_MAX_S either. Either way it is killed.
UPLOAD_STALL_S = 300
UPLOAD_MAX_S = 1800
UPLOAD_TRIES = 3          # at the end of a run (finish); during it, the next save retries


class HubUploader:
    """Pushes the checkpoint to the repo as HUB_FILE, in the background
    (one upload at a time; a push while one is running is skipped). It is
    staged in run_dir/hub/upload first, so the next save can't change it
    mid-upload. The hf output goes to run_dir/hub_upload.log. A stuck
    upload is killed (UPLOAD_STALL_S, UPLOAD_MAX_S) and counts as failed."""

    def __init__(self, repo: str, run_dir: Path):
        self.repo, self.dir, self.hf = repo, run_dir / "hub", _hf()
        self.log = open(run_dir / "hub_upload.log", "a")
        self.proc: subprocess.Popen | None = None
        self.it: int | None = None         # the upload in flight (or last started)
        self.done: int | None = None       # the last iteration uploaded
        self.started = 0.0                 # when the upload in flight started

    def _check(self) -> None:
        """Kill the upload in flight if it is stuck (see UPLOAD_STALL_S)."""
        if self.proc is None or self.proc.poll() is not None:
            return
        now = time.time()
        quiet = now - max(self.started, os.fstat(self.log.fileno()).st_mtime)
        why = (f"over {UPLOAD_MAX_S}s" if now - self.started > UPLOAD_MAX_S else
               f"no progress for {UPLOAD_STALL_S}s" if quiet > UPLOAD_STALL_S else None)
        if why is None:
            return
        print(f"[hub] upload of iteration {self.it} stuck ({why}): killed")
        self.log.write(f"--- iteration {self.it}: killed, {why}\n")
        self.log.flush()
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(self.proc.pid, sig)      # hf and anything it started
            except ProcessLookupError:
                break
            try:
                self.proc.wait(10)
                break
            except subprocess.TimeoutExpired:
                pass

    def busy(self) -> bool:
        self._check()
        return self.proc is not None and self.proc.poll() is None

    def _reap(self) -> None:
        if self.proc is None:
            return
        while self.busy():
            time.sleep(1)
        rc = self.proc.wait()
        self.proc = None
        if rc == 0:
            self.done = self.it
            print(f"[hub] uploaded iteration {self.it} to {self.repo}")
        else:
            print(f"[hub] upload of iteration {self.it} failed (exit {rc}); see hub_upload.log")

    def _stage(self, ckpt: Path) -> Path:
        staged = self.dir / "upload"
        shutil.rmtree(staged, ignore_errors=True)
        staged.mkdir(parents=True)
        shutil.copyfile(ckpt, staged / HUB_FILE)
        return staged

    def push(self, ckpt: Path, it: int) -> bool:
        """Start uploading ckpt (at iteration it); False (skipped) if an
        upload is running."""
        if self.busy():
            return False
        self._reap()
        staged = self._stage(ckpt)
        self.it, self.started = it, time.time()
        self.log.write(f"--- iteration {self.it}\n")
        self.log.flush()
        # --delete: progress saves an older version of this code uploaded
        # are removed, so no machine finds them.
        self.proc = subprocess.Popen([self.hf, "upload", self.repo, str(staged), ".",
                                      "--delete", HUB_STALE,
                                      "--commit-message", f"iteration {self.it}"],
                                     stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     start_new_session=True)      # its own group, for _check's kill
        return True

    def finish(self, ckpt: Path | None, it: int | None) -> None:
        """At the end: wait for the running upload, then upload ckpt (the
        newest checkpoint, at iteration it) if the repo doesn't have it yet."""
        if self.busy():
            print(f"[hub] waiting for the upload of iteration {self.it}...")
        self._reap()
        for _ in range(UPLOAD_TRIES):
            if ckpt is None or it is None or it == self.done:
                break
            self.push(ckpt, it)
            print(f"[hub] uploading iteration {self.it}...")
            self._reap()
        self.log.close()
