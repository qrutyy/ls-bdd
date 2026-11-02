import csv

import numpy as np
import pandas as pd
import pytest

from lsvtest import fio, report, spec


def write_runs(path, iops_by_ds):
    fields = ["experiment", "run_id", "repeat", "time"]
    fields += list(spec.Point.__dataclass_fields__) + fio.ROW_FIELDS
    path.mkdir(parents=True, exist_ok=True)
    with (path / "runs.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for ds, values in iops_by_ds.items():
            raw = ds == spec.RAW
            for repeat, iops in enumerate(values, 1):
                writer.writerow(
                    {
                        "experiment": "t",
                        "run_id": f"{ds}-r{repeat}",
                        "repeat": repeat,
                        "time": 0,
                        "target": spec.RAW if raw else "lsv",
                        "ds": ds,
                        "cell_size": "" if raw else 4096,
                        "dedup": "" if raw else False,
                        "rw": "randwrite",
                        "bs": 4096,
                        "numjobs": 1,
                        "iodepth": 32,
                        "direction": "write",
                        "iops": iops,
                        "bw_bytes": iops * 4096,
                        "lat_p99_us": 1e6 / iops,
                    }
                )


def test_bootstrap_ci_brackets_the_median():
    values = [100, 102, 98, 101, 99, 250]
    lo, hi = report.bootstrap_ci(values)
    assert lo <= np.median(values) <= hi
    assert report.bootstrap_ci(values) == (lo, hi)


def test_bootstrap_ci_degenerate():
    assert report.bootstrap_ci([5.0]) == (5.0, 5.0)


def test_summary_one_row_per_point(tmp_path):
    write_runs(tmp_path, {"ht": [100, 110, 90], "sl": [50, 55, 45], "raw": [400] * 3})
    summary = report.summarize(report.load_runs([tmp_path]))

    assert len(summary) == 3
    by_ds = summary.set_index("ds")
    assert by_ds.loc["ht", "iops_median"] == 100
    assert by_ds.loc["ht", "n"] == 3
    assert by_ds.loc["raw", "iops_cv"] == 0
    assert by_ds.loc["sl", "bw_mib_median"] == pytest.approx(50 * 4096 / (1 << 20))
    # the baseline has no cell size and must still be grouped, not dropped
    assert pd.isna(by_ds.loc["raw", "cell_size"])


def test_report_writes_summary_and_plots(tmp_path):
    write_runs(tmp_path / "a", {"ht": [100, 110], "raw": [400, 410]})
    write_runs(tmp_path / "b", {"sl": [50, 60]})
    out = report.report([tmp_path / "a", tmp_path / "b"], tmp_path / "rep")

    assert set(pd.read_csv(out / "summary.csv")["ds"]) == {"ht", "sl", "raw"}
    assert (out / "iops.png").stat().st_size > 0
    assert (out / "lat_p99_us.png").stat().st_size > 0


def test_missing_runs(tmp_path):
    with pytest.raises(FileNotFoundError):
        report.load_runs([tmp_path])


def write_kstats(path, by_ds):
    with (path / "kstats.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["run_id", "key", "value"])
        for ds, values in by_ds.items():
            for repeat, ns in enumerate(values, 1):
                run_id = f"{ds}-r{repeat}"
                writer.writerow([run_id, "cell_size", 4096])
                writer.writerow([run_id, "lat_idx_lookup_count", 10])
                writer.writerow([run_id, "lat_idx_lookup_mean_ns", ns])
                writer.writerow([run_id, "lat_bio_count", 10])
                writer.writerow([run_id, "lat_bio_mean_ns", ns * 40])


def test_kstats_summary_and_stage_plots(tmp_path):
    write_runs(tmp_path, {"ht": [100, 110, 90], "sl": [50, 55, 45], "raw": [400] * 3})
    write_kstats(tmp_path, {"ht": [200, 220, 210], "sl": [900, 950, 1000]})

    kdf = report.load_kstats([tmp_path])
    # the kernel's cell_size must not clobber the point's
    assert {"cell_size", "k_cell_size"} <= set(kdf.columns)
    ks = report.summarize_kstats(kdf).set_index("ds")
    assert set(ks.index) == {"ht", "sl"}
    assert ks.loc["ht", "k_lat_idx_lookup_mean_ns"] == 210
    assert ks.loc["sl", "k_lat_bio_mean_ns"] == 950 * 40

    out = report.report([tmp_path], tmp_path / "rep")
    assert (out / "kstats_summary.csv").exists()
    assert (out / "stages_mean.png").stat().st_size > 0


def test_report_without_kstats(tmp_path):
    write_runs(tmp_path, {"raw": [400] * 3})
    out = report.report([tmp_path], tmp_path / "rep")
    assert report.load_kstats([tmp_path]) is None
    assert not (out / "kstats_summary.csv").exists()
