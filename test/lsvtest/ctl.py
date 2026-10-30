"""
Control of the lsbdd module and its devices.

The driver is driven through its module parameters (see src/main.c):

    echo ht            > /sys/module/lsbdd/parameters/set_data_structure
    echo "1 /dev/ram0" > /sys/module/lsbdd/parameters/set_redirect_bd
    cat /sys/module/lsbdd/parameters/get_vbd_names    # "1. lsvbd1 -> ram0"
    echo 1             > /sys/module/lsbdd/parameters/delete_bd

set_data_structure picks the index of the devices created after it;
set_redirect_bd creates /dev/lsvbd<N> on top of the given backing device;
delete_bd takes the position of a device in the get_vbd_names listing.

The head of the log is global to the module and only reset by loading it,
so a fresh device in the measurement sense needs a module reload (see
reload_module()).

Every path the module is reached through can be overridden, so the logic
here runs against a plain directory in the unit tests.
"""

import os
import re
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

MODULE_NAME = "lsbdd"
SYS_ROOT = Path("/sys")
DEV_ROOT = Path("/dev")
NAME_PREFIX = "lsvbd"

# The indexes the driver knows (available_ds in src/main.h).
DATA_STRUCTURES = ("bt", "sl", "ht", "rb")

_LISTING = re.compile(r"\s*(\d+)\.\s+(\S+)\s+->\s+(\S+)")


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


def params_dir(sys_root: Path = SYS_ROOT) -> Path:
    return Path(sys_root) / "module" / MODULE_NAME / "parameters"


@dataclass(frozen=True)
class DeviceSpec:
    name: str
    backing: str
    index_ds: str

    @property
    def index(self) -> int:
        """lsvbd<N> is created by passing N to set_redirect_bd."""
        m = re.fullmatch(rf"{NAME_PREFIX}(\d+)", self.name)
        if not m:
            raise LsvError(f"device name {self.name!r} must be {NAME_PREFIX}<N>")
        return int(m.group(1))

    def attrs(self) -> dict:
        """Parameters in the order they are written."""
        return {
            "set_data_structure": self.index_ds,
            "set_redirect_bd": f"{self.index} {self.backing}",
        }


def listing(sys_root: Path = SYS_ROOT) -> list[tuple[int, str, str]]:
    """(position, device, backing) as get_vbd_names prints them."""
    path = params_dir(sys_root) / "get_vbd_names"
    try:
        text = path.read_text()
    except OSError:
        return []
    rows = []
    for line in text.splitlines():
        m = _LISTING.match(line)
        if m:
            rows.append((int(m.group(1)), m.group(2), m.group(3)))
    return rows


def device_names(sys_root: Path = SYS_ROOT) -> list[str]:
    return sorted(name for _, name, _ in listing(sys_root))


class Device:
    """An lsbdd device; usable as a context manager."""

    def __init__(
        self,
        spec: DeviceSpec,
        sys_root: Path = SYS_ROOT,
        dev_root: Path = DEV_ROOT,
    ):
        self.spec = spec
        self.sys_root = Path(sys_root)
        self.params = params_dir(sys_root)
        self.path = Path(dev_root) / spec.name

    @property
    def created(self) -> bool:
        return self.spec.name in device_names(self.sys_root)

    def create(self, timeout: float = 5.0) -> "Device":
        if self.spec.index_ds not in DATA_STRUCTURES:
            raise LsvError(
                f"unknown index {self.spec.index_ds!r}, "
                f"expected one of {', '.join(DATA_STRUCTURES)}"
            )
        if self.created:
            raise LsvError(f"{self.spec.name} already exists")

        for attr, value in self.spec.attrs().items():
            write_attr(self.params / attr, value)

        if timeout:
            try:
                self.wait_node(timeout)
            except LsvError:
                self.destroy()
                raise
        return self

    def wait_node(self, timeout: float) -> None:
        """The disk is added from set_redirect_bd, its node comes from udev later."""
        deadline = time.monotonic() + timeout
        while not self.path.exists():
            if time.monotonic() > deadline:
                raise LsvError(f"{self.path} did not appear in {timeout}s")
            time.sleep(0.05)

    def destroy(self) -> None:
        for position, name, _ in listing(self.sys_root):
            if name == self.spec.name:
                write_attr(self.params / "delete_bd", position)
                return

    def __enter__(self) -> "Device":
        if not self.created:
            self.create()
        return self

    def __exit__(self, *exc) -> None:
        self.destroy()


def module_loaded(sys_root: Path = SYS_ROOT) -> bool:
    return (Path(sys_root) / "module" / MODULE_NAME).is_dir()


def load_module(ko: Path, params: Optional[dict] = None) -> None:
    if module_loaded():
        raise LsvError(f"{MODULE_NAME} is already loaded")
    if not Path(ko).is_file():
        raise LsvError(f"{ko} does not exist, build it with: make -C ../src type=lf")
    args = [f"{k}={v}" for k, v in (params or {}).items()]
    run(["insmod", str(ko), *args])


def unload_module(sys_root: Path = SYS_ROOT) -> None:
    """Devices pin the module, so they are torn down first."""
    # positions shift after each deletion, the first one is always 1
    for _ in listing(sys_root):
        write_attr(params_dir(sys_root) / "delete_bd", 1)
    if module_loaded(sys_root):
        run(["rmmod", MODULE_NAME])


def reload_module(ko: Path) -> None:
    """The only way to get a log that starts from the head of the backing device."""
    unload_module()
    load_module(ko)


def drop_caches() -> None:
    os.sync()
    Path("/proc/sys/vm/drop_caches").write_text("3\n")


@dataclass
class Backing:
    """A block device lsbdd devices are put on top of."""

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
