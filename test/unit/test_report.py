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
