"""
Runs an experiment: every point of the matrix, every repeat, on a fresh
lsbdd device each time. The head of the log is global to the module, so
"fresh" means a module reload before every run.

Layout of a result directory:

    experiment.toml   the spec as it was run
    meta.json         environment fingerprint
    runs.csv          one row per run and IO direction
    failures.jsonl    runs fio failed on, with its stderr
    fio/<run_id>.json raw fio output

Repeats are the outer loop: each pass goes over the whole matrix, so a slow
drift of the machine (thermal, page cache, background load) spreads over
all points instead of biasing the ones measured last.
"""

import csv
import json
import os
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

from . import ctl, env, fio, spec
from .spec import RAW, Point

DEVICE_NAME = "lsvbd1"
DEFAULT_KO = Path(__file__).resolve().parents[2] / "src" / f"{ctl.MODULE_NAME}.ko"

# Sectors lsbdd skips at the head of the backing device (LSBDD_SECTOR_OFFSET).
META_BYTES = 32 * 512
ALIGN = 1 << 20
# Share of the free physical space a run may write, the rest is slack.
WRITE_SLACK = 0.9

RUN_COLUMNS = ["experiment", "run_id", "repeat", "time"]


class FioError(RuntimeError):
    pass


@dataclass(frozen=True)
class IoPlan:
    size: int
    precondition: bool
    io_size: Optional[int]


def plan_io(point: Point, dev_bytes: int, region: float, precondition: str) -> IoPlan:
    """
    lsbdd has no garbage collection: every write takes the next sectors of
    the log, and a device that has written as much as its backing holds
    fails every further write. So a run that writes is bounded by
    bytes rather than by time, sized to what is left after preconditioning.

    The baseline gets the same plan as its lsv twin, so both do the same IO.
    """
    size = int(dev_bytes * region) // ALIGN * ALIGN
    if size < ALIGN:
        raise spec.SpecError(f"region of {dev_bytes} bytes is below {ALIGN}")

    pre = precondition == "always" or (precondition == "auto" and point.read_share > 0)
    if not point.writes:
        return IoPlan(size, pre, None)

    free = dev_bytes - META_BYTES - (size if pre else 0)
    total = free * WRITE_SLACK / (1 - point.read_share)
    per_job = int(total / point.numjobs) // point.bs * point.bs
    if per_job < point.bs:
        raise spec.SpecError(f"{point.key()}: no write budget left, lower run.region")
    return IoPlan(size, pre, per_job)


class System:
    """What the runner does to the machine."""

    def __init__(self, ko: Path = DEFAULT_KO):
        self.ko = Path(ko)

    def open_backing(self, kind: str, size_gib: int) -> ctl.Backing:
        return ctl.open_backing(kind, size_gib)

    def device_size(self, path: str) -> int:
        sectors = Path("/sys/class/block") / Path(path).name / "size"
        return int(sectors.read_text()) * 512

    def create_device(self, dspec: ctl.DeviceSpec) -> str:
        ctl.reload_module(self.ko)
        return str(ctl.Device(dspec).create().path)

    def destroy_device(self, name: str) -> None:
        ctl.Device(ctl.DeviceSpec(name, "", "")).destroy()
        ctl.unload_module()

    def settle(self) -> None:
        ctl.drop_caches()

    def fio(self, args: list[str]) -> Optional[dict]:
        proc = subprocess.run(args, capture_output=True, text=True)
        if proc.returncode:
            raise FioError(proc.stderr.strip() or f"fio exited {proc.returncode}")
        # fio may print warnings ahead of the JSON document
        start = proc.stdout.find("{")
        if start < 0:
            raise FioError("fio printed no JSON")
        return json.loads(proc.stdout[start:])

    def check(self, exp: spec.Experiment) -> None:
        if os.geteuid():
            raise ctl.LsvError("run as root: the module is reloaded for every run")
        if any(p.target != RAW for p in exp.points()) and not self.ko.is_file():
            raise ctl.LsvError(
                f"{self.ko} does not exist, build it with: make -C ../src type=lf"
            )


class DryRun(System):
    """Prints what would run; touches nothing."""

    def __init__(self, backing_size_gib: int):
        self.backing_size = backing_size_gib << 30

    def open_backing(self, kind: str, size_gib: int) -> ctl.Backing:
        return ctl.Backing(kind if kind.startswith("/dev/") else f"/dev/<{kind}>")

    def device_size(self, path: str) -> int:
        return self.backing_size

    def create_device(self, dspec: ctl.DeviceSpec) -> str:
        print(f"  create {dspec}")
        return f"/dev/{dspec.name}"

    def destroy_device(self, name: str) -> None:
        pass

    def settle(self) -> None:
        pass

    def fio(self, args: list[str]) -> Optional[dict]:
        print("  " + " ".join(args))
        return None

    def check(self, exp: spec.Experiment) -> None:
        pass


def result_dir(root: Path, exp: spec.Experiment) -> Path:
    return Path(root) / f"{exp.name}-{datetime.now():%Y%m%d-%H%M%S}"


class Runner:
    def __init__(self, exp: spec.Experiment, out: Path, system: System, repo: Path):
        self.exp = exp
        self.out = Path(out)
        self.system = system
        self.repo = repo
        self.dry = isinstance(system, DryRun)
        self.fields = (
            RUN_COLUMNS
            + list(Point.__dataclass_fields__)
            + sorted(exp.tags)
            + fio.ROW_FIELDS
        )

    @property
    def runs_csv(self) -> Path:
        return self.out / "runs.csv"

    def done(self) -> set[str]:
        if not self.runs_csv.exists():
            return set()
        with self.runs_csv.open() as f:
            return {row["run_id"] for row in csv.DictReader(f)}

    def _prepare(self, backing: str) -> None:
        if self.dry:
            return
        (self.out / "fio").mkdir(parents=True, exist_ok=True)
        (self.out / "experiment.toml").write_text(self.exp.source)
        meta = env.fingerprint(self.repo, [backing])
        meta["experiment"] = self.exp.name
        (self.out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")

    def run(self) -> int:
        """Returns the number of failed runs."""
        self.system.check(self.exp)
        points = self.exp.points()
        done = self.done()
        total = len(points) * self.exp.repeats
        failed = 0

        with self.system.open_backing(
            self.exp.backing, self.exp.backing_size_gib
        ) as backing:
            self._prepare(backing.path)
            dev_bytes = self.system.device_size(backing.path)
            n = 0
            for repeat in range(1, self.exp.repeats + 1):
                for point in points:
                    n += 1
                    run_id = f"{point.key()}-r{repeat}"
                    if run_id in done:
                        continue
                    print(f"[{n}/{total}] {run_id}", flush=True)
                    if not self.one(point, repeat, run_id, backing.path, dev_bytes):
                        failed += 1
        return failed

    def one(
        self, point: Point, repeat: int, run_id: str, backing: str, dev_bytes: int
    ) -> bool:
        plan = plan_io(point, dev_bytes, self.exp.region, self.exp.precondition)
        lsv = point.target != RAW
        args: list[str] = []
        try:
            if lsv:
                target = self.system.create_device(
                    ctl.DeviceSpec(
                        name=DEVICE_NAME,
                        backing=backing,
                        index_ds=point.ds,
                    )
                )
            else:
                target = backing

            if plan.precondition:
                self.system.fio(fio.precondition_args(target, plan.size))
            args = fio.job_args(point, target, self.exp.fio, plan.size, plan.io_size)
            result = self.system.fio(args)
        except (FioError, ctl.LsvError) as e:
            print(f"  failed: {e}", flush=True)
            self._failure(run_id, args, str(e))
            return False
        finally:
            if lsv:
                self.system.destroy_device(DEVICE_NAME)
            self.system.settle()

        if result is None:
            return True
        self._record(point, repeat, run_id, result)
        return True

    def _record(self, point: Point, repeat: int, run_id: str, result: dict) -> None:
        (self.out / "fio" / f"{run_id}.json").write_text(json.dumps(result))
        base = {
            "experiment": self.exp.name,
            "run_id": run_id,
            "repeat": repeat,
            "time": int(time.time()),
            **point.columns(),
            **self.exp.tags,
        }
        new = not self.runs_csv.exists()
        with self.runs_csv.open("a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=self.fields)
            if new:
                writer.writeheader()
            for row in fio.parse(result):
                writer.writerow({**base, **row})
                print(
                    f"  {row['direction']}: {row['iops']:.0f} IOPS, "
                    f"p99 {row['lat_p99_us'] or 0:.1f} us",
                    flush=True,
                )

    def _failure(self, run_id: str, args: list[str], error: str) -> None:
        if self.dry:
            return
        entry = {"run_id": run_id, "args": args, "error": error}
        with (self.out / "failures.jsonl").open("a") as f:
            f.write(json.dumps(entry) + "\n")
