import json
from pathlib import Path

import pytest

from lsvtest import fio, spec

DATA = Path(__file__).parent / "data"


def point(**kw):
    values = dict(
        target="lsv",
        ds="ht",
        rw="randwrite",
        rwmixread=None,
        bs=4096,
        numjobs=4,
        iodepth=32,
    )
    values.update(kw)
    return spec.Point(**values)


def opts(args):
    return dict(a[2:].split("=", 1) for a in args[1:])


def test_time_based_without_io_size():
    args = opts(fio.job_args(point(rw="randread"), "/dev/lsvbd1", {}, size=1 << 30))
    assert args["time_based"] == "1"
    assert "io_size" not in args
    assert args["filename"] == "/dev/lsvbd1"
    assert args["size"] == str(1 << 30)
    assert args["output-format"] == "json"
    assert args["group_reporting"] == "1"


def test_bounded_by_io_size():
    args = opts(fio.job_args(point(), "/dev/x", {"runtime": 7}, 1 << 30, 1 << 20))
    assert args["io_size"] == str(1 << 20)
    assert args["runtime"] == "7"
    assert "time_based" not in args


def test_mix_and_extra_options():
    args = opts(
        fio.job_args(
            point(rw="randrw", rwmixread=70),
            "/dev/x",
            {"ioengine": "libaio", "extra": {"cpus_allowed": "0-3"}},
            1 << 30,
        )
    )
    assert args["rwmixread"] == "70"
    assert args["ioengine"] == "libaio"
    assert args["cpus_allowed"] == "0-3"


def test_parse_mixed_run():
    result = json.loads((DATA / "fio_randrw.json").read_text())
    rows = {r["direction"]: r for r in fio.parse(result)}

    assert set(rows) == {"read", "write"}
    read = rows["read"]
    assert read["iops"] == 35840.0
    assert read["bw_bytes"] == 146800640
    assert read["sys_cpu"] == 61.25
    assert read["clat_p99_us"] == pytest.approx(220.16)
    assert read["lat_p999_us"] == pytest.approx(407.552)
    assert read["lat_mean_us"] == pytest.approx(106.1)
    # no slat percentiles in the read half of the sample
    assert read["slat_p99_us"] is None
    assert rows["write"]["slat_p99_us"] == pytest.approx(7.008)
    assert rows["write"]["lat_p9999_us"] is None


def test_rows_match_declared_fields():
    result = json.loads((DATA / "fio_randrw.json").read_text())
    for row in fio.parse(result):
        assert list(row) == fio.ROW_FIELDS


def test_parse_skips_idle_direction():
    result = json.loads((DATA / "fio_randrw.json").read_text())
    result["jobs"][0]["write"]["total_ios"] = 0
    assert [r["direction"] for r in fio.parse(result)] == ["read"]


def test_parse_without_jobs():
    with pytest.raises(ValueError):
        fio.parse({"jobs": []})


def test_extra_templates_follow_the_point():
    cfg = {"extra": {"cpus_allowed": "0-{last_cpu}", "registerfiles": 1}}
    args = opts(fio.job_args(point(numjobs=8), "/dev/lsvbd1", cfg, size=1 << 30))
    assert args["cpus_allowed"] == "0-7"
    assert args["registerfiles"] == "1"
