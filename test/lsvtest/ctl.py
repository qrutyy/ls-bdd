"""
Control of the lsv module and its devices.

A device is described in configfs and instantiated by writing create_new
(see src/ctl/configfs.c):

    mkdir /sys/kernel/config/lsv/<name>
    echo /dev/ram0 > .../backing_path
    echo ht        > .../index_ds
    echo 4096      > .../cell_size
    echo 1         > .../create_new

and shows up as /dev/<name>. rmdir tears it down.

Every path the module is reached through can be overridden, so the logic
here runs against a plain directory in the unit tests.
"""

import os
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

MODULE_NAME = "lsv"
CONFIGFS_ROOT = Path("/sys/kernel/config")
DEV_ROOT = Path("/dev")

# Attributes in the order they are written; create_new always goes last.
DEVICE_ATTRS = ("backing_path", "index_ds", "cell_size", "segment_size", "dedup")


class LsvError(RuntimeError):
    pass


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if check and proc.returncode:
        raise LsvError(f"{' '.join(cmd)}: {proc.stderr.strip() or proc.returncode}")
    return proc


def write_attr(path: Path, value) -> None:
    try:
        path.write_text(f"{value}\n")
    except OSError as e:
        raise LsvError(f"{path.name} = {value!r}: {os.strerror(e.errno)}") from e


@dataclass(frozen=True)
class DeviceSpec:
    name: str
    backing: str
    index_ds: str
    cell_size: int = 4096
    segment_size: Optional[int] = None
    dedup: bool = False

    def attrs(self) -> dict:
        values = {
            "backing_path": self.backing,
            "index_ds": self.index_ds,
            "cell_size": self.cell_size,
            "segment_size": self.segment_size,
            "dedup": int(self.dedup),
        }
        return {k: values[k] for k in DEVICE_ATTRS if values[k] is not None}


class Device:
    """A configfs-backed lsv device; usable as a context manager."""

    def __init__(
        self,
        spec: DeviceSpec,
        configfs_root: Path = CONFIGFS_ROOT,
        dev_root: Path = DEV_ROOT,
    ):
        self.spec = spec
        self.item = Path(configfs_root) / MODULE_NAME / spec.name
        self.path = Path(dev_root) / spec.name

    @property
    def created(self) -> bool:
        flag = self.item / "create_new"
        return flag.exists() and flag.read_text().strip() == "1"

    def create(self, timeout: float = 5.0) -> "Device":
        try:
            self.item.mkdir()
        except FileExistsError as e:
            raise LsvError(f"{self.spec.name} already exists") from e

        try:
            for attr, value in self.spec.attrs().items():
                write_attr(self.item / attr, value)
            write_attr(self.item / "create_new", 1)
        except LsvError:
            self.destroy()
            raise

        if timeout:
            self.wait_node(timeout)
        return self

    def wait_node(self, timeout: float) -> None:
        """The disk is added from create_new, its node comes from udev later."""
        deadline = time.monotonic() + timeout
        while not self.path.exists():
            if time.monotonic() > deadline:
                raise LsvError(f"{self.path} did not appear in {timeout}s")
            time.sleep(0.05)

    def destroy(self) -> None:
        if self.item.is_dir():
            self.item.rmdir()

    def __enter__(self) -> "Device":
        if not self.item.is_dir():
            self.create()
        return self

    def __exit__(self, *exc) -> None:
        self.destroy()


def device_names(configfs_root: Path = CONFIGFS_ROOT) -> list[str]:
    group = Path(configfs_root) / MODULE_NAME
    if not group.is_dir():
        return []
    return sorted(p.name for p in group.iterdir() if p.is_dir())


def module_loaded(sys_root: Path = Path("/sys")) -> bool:
    return (Path(sys_root) / "module" / MODULE_NAME).is_dir()


def ensure_configfs(configfs_root: Path = CONFIGFS_ROOT) -> None:
    if run(["mountpoint", "-q", str(configfs_root)], check=False).returncode:
        run(["modprobe", "configfs"], check=False)
        run(["mount", "-t", "configfs", "none", str(configfs_root)])


def load_module(ko: Path, params: Optional[dict] = None) -> None:
    ensure_configfs()
    if module_loaded():
        raise LsvError(f"{MODULE_NAME} is already loaded")
    args = [f"{k}={v}" for k, v in (params or {}).items()]
    run(["insmod", str(ko), *args])


def unload_module(configfs_root: Path = CONFIGFS_ROOT) -> None:
    """Devices pin the module, so they are torn down first."""
    for name in device_names(configfs_root):
        (Path(configfs_root) / MODULE_NAME / name).rmdir()
    if module_loaded():
        run(["rmmod", MODULE_NAME])


def drop_caches() -> None:
    os.sync()
    Path("/proc/sys/vm/drop_caches").write_text("3\n")


@dataclass
class Backing:
    """A block device lsv devices are put on top of."""

    path: str
    teardown: Callable[[], None] = field(default=lambda: None, repr=False)

    def __enter__(self) -> "Backing":
        return self

    def __exit__(self, *exc) -> None:
        self.teardown()


def brd(size_gib: int) -> Backing:
    """A ramdisk: the closest to a device with no cost of its own."""
    run(["modprobe", "-r", "brd"], check=False)
    run(["modprobe", "brd", "rd_nr=1", f"rd_size={size_gib * 1024 * 1024}"])
    return Backing("/dev/ram0", lambda: run(["modprobe", "-r", "brd"], check=False))


def null_blk(size_gib: int, memory_backed: bool = False) -> Backing:
    """
    Without |memory_backed| reads return zeroes, which is fine for performance
    and useless for verification.
    """
    run(["modprobe", "-r", "null_blk"], check=False)
    run(
        [
            "modprobe",
            "null_blk",
            "nr_devices=1",
            f"gb={size_gib}",
            "irqmode=0",
            f"memory_backed={int(memory_backed)}",
        ]
    )
    return Backing(
        "/dev/nullb0", lambda: run(["modprobe", "-r", "null_blk"], check=False)
    )


def open_backing(kind: str, size_gib: int = 4) -> Backing:
    """|kind| is "brd", "null_blk" or the path of an existing device."""
    if kind == "brd":
        return brd(size_gib)
    if kind == "null_blk":
        return null_blk(size_gib)
    if kind.startswith("/dev/"):
        return Backing(kind)
    raise LsvError(f"unknown backing {kind!r}: brd, null_blk or /dev/<name>")
