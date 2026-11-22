"""The data and the gnuplot scripts of the figures, from results made up for the test."""

import csv

import numpy as np
import pytest

from lsvtest import figures

FIELDS = [
    "experiment", "run_id", "repeat", "time", "target", "ds", "rw", "rwmixread", "bs",
    "numjobs", "iodepth", "mode", "variant", "direction", "iops", "bw_bytes", "lat_p99_us", "error",
]  # fmt: skip
PAIRS = [(1, 4), (2, 8), (4, 16), (8, 32), (16, 64), (32, 128)]
WORKLOADS = [
    ("rw", 100),
    ("rw", 0),
    ("rw", 65),
    ("randrw", 100),
    ("randrw", 0),
    ("randrw", 65),
]


def make_results(path, mode, variant=None, speed=1.0, repeats=10, seed=0, outlier=True):
    """The three experiments of one build: IOPS grows with the sources, a bit slower per build."""
    rng = np.random.default_rng(seed)
    path.mkdir(parents=True, exist_ok=True)
    cols = FIELDS if variant is not None else [c for c in FIELDS if c != "variant"]
    experiments = (
        ("paper_scaling", [("rw", 100), ("randrw", 100)], PAIRS),
        ("paper_ops", WORKLOADS, [(8, 32)]),
        ("paper_lat", WORKLOADS, [(1, 1), (8, 32)]),
    )
    with (path / "runs.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        rid = 0
        for name, workloads, pairs in experiments:
            for ds, ds_speed in (("ht", 1.0), ("sl", 1.2)):
                for rw, share in workloads:
                    for nj, qd in pairs:
                        for rep in range(repeats):
                            base = (
                                20000
                                * min(nj, 8)
                                * ds_speed
                                * speed
                                * (1.0 if rw == "rw" else 0.8)
                            )
                            iops = base * rng.normal(1, 0.03)
                            if outlier and rep == 3 and (nj, qd) == (8, 32):
                                iops *= 0.6  # one bad run
                            for direction in ("read", "write"):
                                part = (
                                    share / 100
                                    if direction == "read"
                                    else 1 - share / 100
                                )
                                if part == 0:
                                    continue
                                row = {
                                    "experiment": name, "run_id": rid, "repeat": rep, "time": 0, "target": "lsv",
                                    "ds": ds, "rw": rw, "rwmixread": share, "bs": 8192, "numjobs": nj, "iodepth": qd,
                                    "mode": mode, "direction": direction, "iops": iops * part,
                                    "bw_bytes": iops * part * 8192,
                                    "lat_p99_us": (50 + 10 * nj * qd / 8) / speed * rng.normal(1, 0.05),
                                    "error": 0,
                                }  # fmt: skip
                                if variant is not None:
                                    row["variant"] = variant
                                w.writerow(row)
                            rid += 1


@pytest.fixture
def dirs(tmp_path):
    make_results(tmp_path / "lf", "lf", variant=None, speed=1.0)
    make_results(tmp_path / "bl", "sy", variant="bucket", speed=0.8, seed=1)
    make_results(tmp_path / "optik", "sy", variant="optik", speed=0.9, seed=2)
    return [tmp_path / "lf", tmp_path / "bl", tmp_path / "optik"]


def test_describe_finds_the_outlier_and_keeps_it_out_of_the_whiskers():
    st = figures.describe([10.0] * 9 + [100.0])
    assert st.outliers == [100.0]
    assert st.whisker_hi == 10.0
    assert st.median == 10.0
    assert st.n == 10


def test_describe_without_outliers():
    st = figures.describe([1, 2, 3, 4, 5])
    assert st.outliers == [] and (st.whisker_lo, st.whisker_hi) == (1, 5)


@pytest.mark.parametrize(
    "mode, variant, ds, expected",
    [
        ("lf", "", "ht", "ht-lf"),
        ("sy", "", "sl", "sl-bl"),
        ("sy", "bucket", "ht", "ht-bl"),
        ("sy", "optik", "ht", "ht-optik"),
    ],
)
def test_series_names(mode, variant, ds, expected):
    row = {"target": "lsv", "ds": ds, "mode": mode, "variant": variant}
    assert figures.series_name(row) == expected


def test_series_name_of_the_bare_device_and_of_an_unknown_build():
    assert (
        figures.series_name({"target": "raw", "ds": "ht", "mode": "lf", "variant": ""})
        == "nvme"
    )
    with pytest.raises(figures.FigureError):
        figures.series_name({"target": "lsv", "ds": "ht", "mode": "xx", "variant": ""})


def test_builds_come_from_the_tags_even_when_one_directory_has_no_variant_column(dirs):
    df = figures.prepare(dirs)
    assert set(df["series"]) == {
        "ht-lf",
        "sl-lf",
        "ht-bl",
        "sl-bl",
        "ht-optik",
    }  # OPTIK is only of the hash table


def test_scaling_points_have_every_pair_and_see_the_bad_run(dirs):
    df = figures.prepare(dirs)
    pts = figures.scaling_points(df[df["experiment"] == "paper_scaling"], "rw")
    assert [lab for lab, _ in pts["ht-lf"]] == [f"{a}/{b}" for a, b in PAIRS]
    at_8_32 = dict(pts["ht-lf"])["8/32"]
    assert at_8_32.n == 10 and len(at_8_32.outliers) == 1
    assert at_8_32.outliers[0] < 0.7 * at_8_32.median


def test_mixed_operation_iops_is_reads_plus_writes(dirs):
    df = figures.prepare(dirs)
    pts = dict(
        figures.ops_points(
            df[df["experiment"] == "paper_ops"], figures.OPTIMAL, "iops"
        )["ht-lf"]
    )
    # in the made-up data a run of "rw:65" has the same total as one of "rw:100"
    assert pts["seq 65/35"].median == pytest.approx(pts["seq read"].median, rel=0.1)


def test_latency_of_a_mixed_operation_has_a_category_per_direction(dirs):
    df = figures.prepare(dirs)
    lat = df[df["experiment"] == "paper_lat"]
    cats = [
        c
        for c, _ in figures.ops_points(lat, figures.NO_CONTENTION, "lat_p99_us")[
            "sl-bl"
        ]
    ]
    assert cats == [
        "seq write", "seq read", "seq 65/35 r", "seq 65/35 w",
        "rand write", "rand read", "rand 65/35 r", "rand 65/35 w",
    ]  # fmt: skip


def test_make_writes_every_figure_without_gnuplot(dirs, tmp_path):
    out = tmp_path / "figs"
    scripts = figures.make(dirs, out, do_render=False)
    assert [s.stem for s in scripts] == ["fig3", "fig4", "fig5", "fig6", "fig7"]
    for name in ("fig3", "fig5", "fig7"):
        assert (out / f"{name}.dat").exists() and (
            out / f"{name}_outliers.dat"
        ).exists()
    gp = (out / "fig3.gp").read_text()
    assert "terminal postscript eps enhanced dashed" in gp
    assert 'title "ht-lf"' in gp and 'title "ht-bl"' in gp and 'title "sl-bl"' in gp
    table = (out / "figures_table.csv").read_text()
    assert "fig3,ht-lf,8/32,10" in table


def test_make_mono_and_bars(dirs, tmp_path):
    figures.make(dirs, tmp_path / "m", mono=True, style="bars", do_render=False)
    assert "monochrome dashed" in (tmp_path / "m" / "fig5.gp").read_text()
    assert "with boxes" in (tmp_path / "m" / "fig5.gp").read_text()


def test_other_experiments_make_no_figure(tmp_path):
    make_results(tmp_path / "d", "lf", variant=None)
    text = (tmp_path / "d" / "runs.csv").read_text()
    for name in ("paper_scaling", "paper_ops", "paper_lat"):
        text = text.replace(name, "smoke")
    (tmp_path / "d" / "runs.csv").write_text(text)
    with pytest.raises(figures.FigureError):
        figures.make([tmp_path / "d"], tmp_path / "out", do_render=False)


def test_figures_do_not_mix_experiments(dirs):
    df = figures.prepare(dirs)
    scaling = figures.scaling_points(df[df["experiment"] == "paper_scaling"], "rw")
    assert "1/1" not in [lab for lab, _ in scaling["ht-lf"]]
