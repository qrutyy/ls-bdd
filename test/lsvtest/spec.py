"""
Experiment description: a TOML file whose [matrix] is expanded into the
cross product of its values, one measurement point per combination.

    name = "ds_compare"

    [device]                  # defaults of every lsbdd device
    backing = "brd"           # brd, null_blk or /dev/<name>
    backing_size_gib = 4

    [matrix]                  # every key is a list
    ds = ["ht", "sl"]
    rw = ["randwrite", "randread", "randrw:70"]
    bs = ["4k"]
    numjobs = [1, 8]
    iodepth = [1, 32]

    [fio]
    ioengine = "io_uring"
    runtime = 30
    extra = { cpus_allowed_policy = "shared" }

    [run]
    repeats = 5
    baseline = true           # the same workloads on the bare backing device
    precondition = "auto"     # auto, always or never
    region = 0.25             # share of the device the workload touches

    [tags]                    # copied into every result row
    mode = "lf"

Any device or workload key may sit either in [matrix] (as a list) or in its
own section (as a scalar default).

When numjobs and iodepth should vary together rather than as a cross
product, [matrix] takes a list of [numjobs, iodepth] pairs instead:

    concurrency = [[1, 4], [2, 8], [4, 16], [8, 32]]

The lf/sy mode of the driver is a build option (make type=...), not a point
of the matrix: run once per build and tell the runs apart by [tags].
"""

import itertools
import re
import tomllib
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path
from typing import Optional

DEVICE_KEYS = ("ds",)
WORKLOAD_KEYS = ("rw", "bs", "numjobs", "iodepth")
PAIRED_KEY = "concurrency"
MATRIX_KEYS = DEVICE_KEYS + WORKLOAD_KEYS + (PAIRED_KEY,)

# Device options of later drivers, which the lsbdd module has no notion of.
UNSUPPORTED_KEYS = ("cell_size", "segment_size", "dedup")

DATA_STRUCTURES = ("bt", "sl", "ht", "rb")

RW_MODES = ("read", "write", "randread", "randwrite", "rw", "randrw")
PRECONDITION = ("auto", "always", "never")

RAW = "raw"


class SpecError(ValueError):
    pass


_SIZE_UNITS = {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30}


def parse_size(value) -> int:
    """'4k' -> 4096; plain integers are bytes."""
    if isinstance(value, int):
        return value
    m = re.fullmatch(r"(\d+)\s*([kmg]?)i?b?", str(value).strip().lower())
    if not m:
        raise SpecError(f"bad size {value!r}")
    return int(m.group(1)) * _SIZE_UNITS[m.group(2)]


def parse_rw(value: str) -> tuple[str, Optional[int]]:
    """'randrw:70' -> ('randrw', 70): the number is the read share in percent."""
    mode, _, mix = str(value).partition(":")
    if mode not in RW_MODES:
        raise SpecError(f"bad rw {value!r}, expected one of {', '.join(RW_MODES)}")
    if mode in ("rw", "randrw"):
        mix_val = int(mix) if mix else 50
        if not 0 <= mix_val <= 100:
            raise SpecError(f"bad read share in {value!r}")
        return mode, mix_val
    if mix:
        raise SpecError(f"{mode} takes no read share: {value!r}")
    return mode, None


@dataclass(frozen=True)
class Point:
    """One measurement point; |target| is "lsv" or "raw" (the baseline)."""

    target: str
    ds: str
    rw: str
    rwmixread: Optional[int]
    bs: int
    numjobs: int
    iodepth: int

    @property
    def read_share(self) -> float:
        if self.rwmixread is not None:
            return self.rwmixread / 100
        return 1.0 if self.rw in ("read", "randread") else 0.0

    @property
    def writes(self) -> bool:
        return self.read_share < 1.0

    def key(self) -> str:
        """Stable identifier, the base of run ids."""
        rw = self.rw + (f"{self.rwmixread}" if self.rwmixread is not None else "")
        parts = [
            self.target,
            self.ds,
            rw,
            f"bs{self.bs}",
            f"nj{self.numjobs}",
            f"id{self.iodepth}",
        ]
        return "-".join(parts)

    def columns(self) -> dict:
        return asdict(self)


@dataclass
class Experiment:
    name: str
    matrix: dict
    device: dict = field(default_factory=dict)
    fio: dict = field(default_factory=dict)
    run: dict = field(default_factory=dict)
    tags: dict = field(default_factory=dict)
    source: str = ""

    @property
    def repeats(self) -> int:
        return int(self.run.get("repeats", 3))

    @property
    def baseline(self) -> bool:
        return bool(self.run.get("baseline", False))

    @property
    def precondition(self) -> str:
        return self.run.get("precondition", "auto")

    @property
    def region(self) -> float:
        return float(self.run.get("region", 0.25))

    @property
    def backing(self) -> str:
        return self.device.get("backing", "brd")

    @property
    def backing_size_gib(self) -> int:
        return int(self.device.get("backing_size_gib", 4))

    def _axes(self) -> dict:
        axes = {}
        for key in DEVICE_KEYS + WORKLOAD_KEYS:
            if key in self.matrix:
                axes[key] = list(self.matrix[key])
            elif key in self.device:
                axes[key] = [self.device[key]]
            elif key in self.fio:
                axes[key] = [self.fio[key]]
        return axes

    def points(self) -> list[Point]:
        axes = self._axes()
        if PAIRED_KEY in self.matrix:
            axes.pop("numjobs", None)
            axes.pop("iodepth", None)
            axes[PAIRED_KEY] = [tuple(pair) for pair in self.matrix[PAIRED_KEY]]
        defaults = {"numjobs": 1, "iodepth": 1}
        names = list(axes)
        points = []
        for combo in itertools.product(*(axes[n] for n in names)):
            values = {**defaults, **dict(zip(names, combo))}
            if PAIRED_KEY in values:
                values["numjobs"], values["iodepth"] = values.pop(PAIRED_KEY)
            rw, mix = parse_rw(values["rw"])
            points.append(
                Point(
                    target="lsv",
                    ds=str(values["ds"]),
                    rw=rw,
                    rwmixread=mix,
                    bs=parse_size(values["bs"]),
                    numjobs=int(values["numjobs"]),
                    iodepth=int(values["iodepth"]),
                )
            )

        if self.baseline:
            seen = set()
            for p in list(points):
                raw = replace(p, target=RAW, ds=RAW)
                if raw not in seen:
                    seen.add(raw)
                    points.append(raw)
        return points

    def validate(self) -> None:
        legacy = (set(self.matrix) | set(self.device)) & set(UNSUPPORTED_KEYS)
        if legacy:
            raise SpecError(
                f"{', '.join(sorted(legacy))}: not supported by the lsbdd driver "
                "of v1.0-isp2026 (it has no cells, segments or deduplication)"
            )
        unknown = set(self.matrix) - set(MATRIX_KEYS)
        if unknown:
            raise SpecError(
                f"unknown matrix keys: {', '.join(sorted(unknown))}; "
                f"known: {', '.join(MATRIX_KEYS)}"
            )
        for key, values in self.matrix.items():
            if not isinstance(values, list) or not values:
                raise SpecError(f"matrix.{key} must be a non-empty list")
        self._validate_concurrency()
        for ds in self._axes().get("ds", []):
            if ds not in DATA_STRUCTURES:
                raise SpecError(
                    f"unknown ds {ds!r}, expected one of {', '.join(DATA_STRUCTURES)}"
                )
        for required in ("ds", "rw", "bs"):
            if required not in self._axes():
                raise SpecError(f"{required} is not set in [matrix] or its section")
        if self.precondition not in PRECONDITION:
            raise SpecError(f"run.precondition must be one of {PRECONDITION}")
        if not 0 < self.region <= 1:
            raise SpecError("run.region must be in (0, 1]")
        if self.repeats < 1:
            raise SpecError("run.repeats must be at least 1")
        self.points()

    def _validate_concurrency(self) -> None:
        if PAIRED_KEY not in self.matrix:
            return
        clash = {"numjobs", "iodepth"} & set(self.matrix)
        if clash:
            raise SpecError(
                f"matrix.{PAIRED_KEY} sets numjobs and iodepth, "
                f"drop matrix.{' and matrix.'.join(sorted(clash))}"
            )
        for pair in self.matrix[PAIRED_KEY]:
            if (
                not isinstance(pair, list)
                or len(pair) != 2
                or not all(isinstance(v, int) and v > 0 for v in pair)
            ):
                raise SpecError(
                    f"matrix.{PAIRED_KEY} takes [numjobs, iodepth] pairs "
                    f"of positive integers, got {pair!r}"
                )


def load(path: Path) -> Experiment:
    path = Path(path)
    source = path.read_text()
    try:
        doc = tomllib.loads(source)
    except tomllib.TOMLDecodeError as e:
        raise SpecError(f"{path}: {e}") from e
    return from_dict(doc, default_name=path.stem, source=source)


def from_dict(doc: dict, default_name: str = "experiment", source: str = ""):
    exp = Experiment(
        name=doc.get("name", default_name),
        matrix=doc.get("matrix", {}),
        device=doc.get("device", {}),
        fio=doc.get("fio", {}),
        run=doc.get("run", {}),
        tags=doc.get("tags", {}),
        source=source,
    )
    exp.validate()
    return exp
