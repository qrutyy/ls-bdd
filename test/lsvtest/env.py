"""
Fingerprint of the machine an experiment ran on, saved next to its results:
numbers without it cannot be compared or reproduced.
"""

import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional, Sequence


def _read(path: str) -> Optional[str]:
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def _cmd(*cmd: str, cwd: Optional[Path] = None) -> Optional[str]:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    except OSError:
        return None
    return proc.stdout.strip() if proc.returncode == 0 else None


def _cpu_model() -> Optional[str]:
    for line in (_read("/proc/cpuinfo") or "").splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    return platform.processor() or None


def _governors() -> list[str]:
    paths = Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor")
    return sorted({g for g in (_read(str(p)) for p in paths) if g})


def _mem_total_kib() -> Optional[int]:
    for line in (_read("/proc/meminfo") or "").splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1])
    return None


def _git(repo: Path) -> dict:
    return {
        "commit": _cmd("git", "rev-parse", "HEAD", cwd=repo),
        "branch": _cmd("git", "rev-parse", "--abbrev-ref", "HEAD", cwd=repo),
        "dirty": bool(
            _cmd("git", "status", "--porcelain", "--untracked-files=no", cwd=repo)
        ),
    }


def _block_queue(dev: str) -> dict:
    queue = Path("/sys/class/block") / Path(dev).name / "queue"
    keys = ("scheduler", "nomerges", "write_cache", "nr_requests", "rotational")
    return {k: _read(str(queue / k)) for k in keys}


def fingerprint(repo: Path, devices: Sequence[str] = ()) -> dict:
    uname = platform.uname()
    return {
        "time": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "host": uname.node,
        "kernel": uname.release,
        "kernel_version": uname.version,
        "cmdline": _read("/proc/cmdline"),
        "arch": uname.machine,
        "cpu": _cpu_model(),
        "cpus_online": os.cpu_count(),
        "governors": _governors(),
        "mem_total_kib": _mem_total_kib(),
        "fio": _cmd("fio", "--version"),
        "python": platform.python_version(),
        "git": _git(repo),
        "module_srcversion": _read("/sys/module/lsbdd/srcversion"),
        "block": {dev: _block_queue(dev) for dev in devices},
    }
