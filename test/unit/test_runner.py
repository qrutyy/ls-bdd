"""The runner against a fake machine that answers fio with a canned result."""

import csv
import json
from pathlib import Path

import pytest

from lsvtest import ctl, runner, spec

DATA = Path(__file__).parent / "data"
GIB = 1 << 30


class FakeSystem(runner.System):
    def __init__(self, fail_on=None):
        super().__init__()
        self.log = []
        self.fail_on = fail_on
        self.result = json.loads((DATA / "fio_randrw.json").read_text())

    def open_backing(self, kind, size_gib):
        self.log.append(("backing", kind))
        return ctl.Backing("/dev/ram0", lambda: self.log.append(("teardown",)))

    def device_size(self, path):
        return 4 * GIB

    def create_device(self, dspec):
        self.log.append(("create", dspec.index_ds))
        return f"/dev/{dspec.name}"

    def destroy_device(self, name):
        self.log.append(("destroy", name))

    def settle(self):
        pass

    def fio(self, args):
        name = next(a for a in args if a.startswith("--name="))
        self.log.append(("fio", name.split("=")[1]))
        if self.fail_on and any(self.fail_on in a for a in args):
            raise runner.FioError("No space left on device")
        return self.result

    def check(self, exp):
        pass


def experiment(**run):
    return spec.from_dict(
        {
            "name": "t",
            "matrix": {"ds": ["ht", "sl"], "rw": ["randread"], "bs": ["4k"]},
            "run": {"repeats": 2, "baseline": True, **run},
            "tags": {"mode": "lf"},
        },
        source="name = 't'\n",
    )


def rows(out):
    with (out / "runs.csv").open() as f:
        return list(csv.DictReader(f))


def test_run_records_every_point_and_repeat(tmp_path):
    system = FakeSystem()
    failed = runner.Runner(experiment(), tmp_path, system, tmp_path).run()

    assert failed == 0
    got = rows(tmp_path)
    # 3 points (ht, sl, raw) x 2 repeats x 2 directions in the canned result
    assert len(got) == 12
    assert {r["ds"] for r in got} == {"ht", "sl", "raw"}
    assert {r["mode"] for r in got} == {"lf"}
    assert (
        list(got[0]) == runner.Runner(experiment(), tmp_path, system, tmp_path).fields
    )
    assert len(list((tmp_path / "fio").glob("*.json"))) == 6
    assert (tmp_path / "experiment.toml").read_text() == "name = 't'\n"
    assert "kernel" in json.loads((tmp_path / "meta.json").read_text())


def test_repeats_are_the_outer_loop(tmp_path):
    system = FakeSystem()
    runner.Runner(experiment(), tmp_path, system, tmp_path).run()
    ids = [r["run_id"] for r in rows(tmp_path)][::2]
    assert [i.rsplit("-", 1)[1] for i in ids] == ["r1"] * 3 + ["r2"] * 3


def test_every_lsv_run_gets_a_fresh_device(tmp_path):
    system = FakeSystem()
    runner.Runner(experiment(), tmp_path, system, tmp_path).run()
    creates = [e for e in system.log if e[0] == "create"]
    destroys = [e for e in system.log if e[0] == "destroy"]
    assert len(creates) == len(destroys) == 4
    # reads need mapped cells: every run is preconditioned first
    fio_names = [e[1] for e in system.log if e[0] == "fio"]
    assert fio_names == ["precondition", "lsvbench"] * 6
    assert system.log[-1] == ("teardown",)


def test_resume_skips_recorded_runs(tmp_path):
    runner.Runner(experiment(), tmp_path, FakeSystem(), tmp_path).run()
    system = FakeSystem()
    runner.Runner(experiment(), tmp_path, system, tmp_path).run()
    assert not [e for e in system.log if e[0] == "fio"]
    assert len(rows(tmp_path)) == 12


def test_failure_is_logged_and_run_continues(tmp_path):
    system = FakeSystem(fail_on="lsvbd1")
    failed = runner.Runner(experiment(), tmp_path, system, tmp_path).run()
    assert failed == 4
    assert {r["ds"] for r in rows(tmp_path)} == {"raw"}
    failures = (tmp_path / "failures.jsonl").read_text().splitlines()
    assert len(failures) == 4
    assert "No space left" in json.loads(failures[0])["error"]
    # the device is torn down even when fio fails
    assert len([e for e in system.log if e[0] == "destroy"]) == 4


def test_dry_run_writes_nothing(tmp_path, capsys):
    out = tmp_path / "out"
    runner.Runner(experiment(), out, runner.DryRun(4), tmp_path).run()
    assert not out.exists()
    assert "--name=lsvbench" in capsys.readouterr().out


def point(rw, mix=None, numjobs=1):
    return spec.Point("lsv", "ht", rw, mix, 4096, numjobs, 1)


def test_plan_reads_are_time_based_and_preconditioned():
    plan = runner.plan_io(point("randread"), 4 * GIB, 0.25, "auto")
    assert plan == runner.IoPlan(GIB, True, None)


def test_plan_writes_fit_in_the_physical_space():
    dev = 4 * GIB
    plan = runner.plan_io(point("randwrite", numjobs=4), dev, 0.25, "auto")
    assert not plan.precondition
    assert plan.io_size % 4096 == 0
    assert 4 * plan.io_size <= (dev - runner.META_BYTES) * runner.WRITE_SLACK


def test_plan_mixed_budget_counts_only_writes():
    dev = 4 * GIB
    plan = runner.plan_io(point("randrw", 70), dev, 0.25, "auto")
    assert plan.precondition
    written = plan.io_size * 0.3
    assert written <= (dev - runner.META_BYTES - plan.size) * runner.WRITE_SLACK
    assert written > (dev - runner.META_BYTES - plan.size) * 0.8


def test_plan_without_budget():
    with pytest.raises(spec.SpecError, match="write budget"):
        runner.plan_io(point("randrw", 99, numjobs=64), 64 << 20, 1.0, "always")
