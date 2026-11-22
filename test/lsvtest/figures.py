"""
The figures of the paper as gnuplot scripts: data files, a script per figure
and, when gnuplot is installed, the EPS files.

    python3 -m lsvtest figures results/paper_*-... --out figures/

Give every result directory of the experiments paper_scaling, paper_ops and
paper_lat, for every build of the module (the builds are told apart by the tags
mode and variant, see experiments/paper_*.toml). The series are named index and
build: ht-lf, ht-bl (a lock per bucket), ht-optik, sl-lf, sl-bl.

    fig3  IOPS of sequential reads against the number of request sources / queue depth
    fig4  the same for random reads
    fig5  IOPS of every operation in the optimal configuration
    fig6  99th percentile of the latency of every operation without contention
    fig7  99th percentile of the latency of every operation in the optimal configuration

How the spread of the repeats is shown (the reviewers asked for it):
  * a line (figs. 3-4) is the median of the repeats with the bootstrap 95%
    confidence interval of the median as error bars; a repeat that Tukey's rule
    calls an outlier (beyond 1.5 interquartile ranges of the quartiles) is
    drawn as a small hollow mark, so that a single bad run is visible and does
    not move the line;
  * a box plot (figs. 5-7): the box is the quartiles, the bar in it the median,
    the whiskers reach the farthest repeat that is not an outlier, outliers are
    hollow marks. `--style bars` draws bars with the confidence interval
    instead, for a print where boxes are too busy.
figures_table.csv lists, for every point, the number of repeats, the median,
the interval, the coefficient of variation and the outliers.
"""

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

from .report import bootstrap_ci, load_runs
from .spec import RAW, SpecError

# The optimal configuration found by the paper: [numjobs, iodepth].
OPTIMAL = (8, 32)
NO_CONTENTION = (1, 1)

# The index of the series and, for the rest of the legend, the build it is made of.
SERIES_ORDER = ["ht-lf", "ht-bl", "ht-optik", "sl-lf", "sl-bl", "nvme"]

# Every figure is drawn from one experiment: the points of the others were
# measured under another protocol (e.g. paper_lat has no warm-up).
EXPERIMENT = {
    "fig3": "paper_scaling",
    "fig4": "paper_scaling",
    "fig5": "paper_ops",
    "fig6": "paper_lat",
    "fig7": "paper_lat",
}

# The tags of a build -> its suffix.
BUILDS = {
    ("lf", ""): "lf",
    ("sy", ""): "bl",
    ("sy", "bucket"): "bl",
    ("sy", "optik"): "optik",
}

# (label, rw, share of reads)
OPS = [
    ("seq write", "rw", 0),
    ("seq read", "rw", 100),
    ("seq 65/35", "rw", 65),
    ("rand write", "randrw", 0),
    ("rand read", "randrw", 100),
    ("rand 65/35", "randrw", 65),
]

# gnuplot styles of the series: colour by index, dash and mark by build.
DS_COLOR = {"ht": "#c0392b", "sl": "#1f5fa8", "nvme": "#555555"}
BUILD_STYLE = {  # dash type of a line, point type, fill of a box
    "lf": ("1", 7, "solid 0.7"),
    "bl": ("2", 9, "empty"),
    "optik": ("4", 5, "pattern 4"),
    "": ("3", 2, "pattern 1"),
}


class FigureError(SpecError):
    pass


@dataclass
class Stats:
    n: int
    median: float
    ci_lo: float
    ci_hi: float
    q1: float
    q3: float
    whisker_lo: float
    whisker_hi: float
    cv: float
    outliers: list


def describe(values) -> Stats:
    """The numbers one point is drawn from."""
    v = np.asarray(values, dtype=float)
    v = v[~np.isnan(v)]
    if not len(v):
        raise FigureError("a point without measurements")
    q1, med, q3 = np.quantile(v, [0.25, 0.5, 0.75])
    fence = 1.5 * (q3 - q1)
    inside = v[(v >= q1 - fence) & (v <= q3 + fence)]
    lo, hi = bootstrap_ci(v)
    mean = v.mean()
    return Stats(
        n=len(v),
        median=float(med),
        ci_lo=lo,
        ci_hi=hi,
        q1=float(q1),
        q3=float(q3),
        whisker_lo=float(inside.min()),
        whisker_hi=float(inside.max()),
        cv=float(v.std(ddof=1) / mean) if len(v) > 1 and mean else float("nan"),
        outliers=sorted(float(x) for x in v if x < q1 - fence or x > q3 + fence),
    )


def series_name(row) -> str:
    if row["target"] == RAW:
        return "nvme"
    mode = str(row.get("mode", "") or "")
    variant = str(row.get("variant", "") or "")
    if variant == "nan":
        variant = ""
    build = BUILDS.get((mode, variant)) or BUILDS.get((mode, ""))
    if build is None:
        raise FigureError(f"unknown build: mode={mode!r} variant={variant!r}")
    return f"{row['ds']}-{build}"


def prepare(dirs: list[Path]) -> pd.DataFrame:
    """One row per run and direction, with the name of its series."""
    df = load_runs(dirs)
    for col in ("mode", "variant"):
        if col not in df:
            df[col] = ""
    df = df[df["error"].fillna(0) == 0] if "error" in df else df
    # OPTIK is the lock of the hash table; the skip list of that build is the one of "bl"
    df = df[
        ~((df["ds"] != "ht") & (df["variant"].fillna("").astype(str) == "optik"))
    ].copy()
    df["series"] = df.apply(series_name, axis=1)
    return df


def _select(
    df: pd.DataFrame, rw: str, share: int, conc: Optional[tuple] = None
) -> pd.DataFrame:
    sel = df[(df["rw"] == rw) & (df["rwmixread"] == share)]
    if conc is not None:
        sel = sel[(sel["numjobs"] == conc[0]) & (sel["iodepth"] == conc[1])]
    return sel


def _series_in(df: pd.DataFrame) -> list[str]:
    present = set(df["series"])
    return [s for s in SERIES_ORDER if s in present] + sorted(
        present - set(SERIES_ORDER)
    )


def run_total(sel: pd.DataFrame, metric: str) -> pd.DataFrame:
    """Per run: the IOPS of both directions summed; a latency is not summed."""
    if metric == "iops":
        return sel.groupby(["series", "run_id"], as_index=False)["iops"].sum()
    return sel


# --- the data of the figures ---------------------------------------------------


def scaling_points(df: pd.DataFrame, rw: str) -> dict:
    """series -> [(label, Stats)] over the concurrency pairs, for reads of kind rw."""
    sel = _select(df, rw, 100)
    pairs = sorted({(int(a), int(b)) for a, b in zip(sel["numjobs"], sel["iodepth"])})
    out = {}
    for s in _series_in(sel):
        pts = []
        for nj, qd in pairs:
            part = sel[
                (sel["series"] == s) & (sel["numjobs"] == nj) & (sel["iodepth"] == qd)
            ]
            runs = run_total(part, "iops")
            if len(runs):
                pts.append((f"{nj}/{qd}", describe(runs["iops"])))
        if pts:
            out[s] = pts
    return out


def ops_points(df: pd.DataFrame, conc: tuple, metric: str) -> dict:
    """series -> [(category, Stats)] for every operation in one configuration.

    IOPS of a mixed operation is the sum of its reads and writes; a latency is
    of one direction, so a mixed operation has a category for each.
    """
    cats = []  # (label, rw, share, direction)
    for label, rw, share in OPS:
        if metric == "iops":
            cats.append((label, rw, share, None))
        elif 0 < share < 100:
            cats += [
                (f"{label} r", rw, share, "read"),
                (f"{label} w", rw, share, "write"),
            ]
        else:
            cats.append((label, rw, share, "read" if share == 100 else "write"))

    out = {s: [] for s in _series_in(df)}
    for label, rw, share, direction in cats:
        sel = _select(df, rw, share, conc)
        for s in out:
            part = sel[sel["series"] == s]
            if direction is None:
                vals = run_total(part, "iops")["iops"]
            else:
                vals = part[part["direction"] == direction][metric]
            vals = vals.dropna()
            out[s].append((label, describe(vals) if len(vals) else None))
    return {s: pts for s, pts in out.items() if any(st for _, st in pts)}


# --- gnuplot ------------------------------------------------------------------


def _style_lines(series: list[str], mono: bool, solid: bool = False) -> list[str]:
    """Line styles by series; boxes keep a solid outline and are told apart by their fill."""
    lines = []
    for i, s in enumerate(series, start=1):
        ds, _, build = s.partition("-")
        dash, pt, _ = BUILD_STYLE.get(build, BUILD_STYLE[""])
        if solid:
            dash = "1"
        color = "#000000" if mono else DS_COLOR.get(ds, "#000000")
        lines.append(
            f'set style line {i} lc rgb "{color}" lw 2 dt {dash} pt {pt} ps 1.1'
        )
    return lines


def _header(
    out: str,
    mono: bool,
    xlabel: str,
    ylabel: str,
    series: list[str],
    ylog: bool = False,
    solid: bool = False,
) -> list[str]:
    term = "monochrome dashed" if mono else "dashed color"
    lines = [
        f'set terminal postscript eps enhanced {term} font "Helvetica,18" size 6.5,4.2',
        f'set output "{out}.eps"',
        f'set xlabel "{xlabel}"',
        f'set ylabel "{ylabel}"',
        "set key outside top center horizontal Left reverse samplen 2.5 width -1 maxcols 5",
        'set grid ytics lc rgb "#bbbbbb" dt 3',
        "set tics nomirror",
        "set border 3",
    ]
    lines += _style_lines(series, mono, solid)
    if ylog:
        lines.append("set logscale y")
    else:
        lines.append("set yrange [0:*]")
    return lines


def _write_blocks(path: Path, blocks: list[list[str]]) -> None:
    """Datasets for `index N`: gnuplot tells them apart by two empty lines."""
    path.write_text("\n\n\n".join("\n".join(b) for b in blocks) + "\n")


def _title(s: str) -> str:
    return s.replace("_", "\\\\_")


def fig_lines(name: str, points: dict, out: Path, title_y: str, mono: bool) -> Path:
    """Figs. 3-4: a line per series, error bars of the interval, outliers as marks."""
    series = list(points)
    labels = [lab for lab, _ in next(iter(points.values()))]
    main, outl = [], []
    for s in series:
        m = [f"# {s}"]
        o = [f"# {s}"]
        for i, (lab, st) in enumerate(points[s]):
            m.append(f"{i} {lab} {st.median:g} {st.ci_lo:g} {st.ci_hi:g}")
            o += [f"{i} {v:g}" for v in st.outliers]
        main.append(m)
        outl.append(o if len(o) > 1 else o + ["# none"])
    has_out = [len(o) > 2 for o in outl]
    _write_blocks(out / f"{name}.dat", main)
    _write_blocks(out / f"{name}_outliers.dat", outl)

    gp = _header(name, mono, "request sources / queue depth", title_y, series)
    gp += [
        'set format y "%.0s%c"',
        f"set xrange [-0.3:{len(labels) - 0.7}]",
        "plot \\",
    ]
    parts = []
    for i, s in enumerate(series):
        parts.append(
            f'"{name}.dat" index {i} using 1:3:4:5:xtic(2) title "{_title(s)}" with yerrorlines ls {i + 1}'
        )
        if has_out[i]:
            parts.append(
                f'"{name}_outliers.dat" index {i} using 1:2 with points ls {i + 1} pt 6 ps 0.8 notitle'
            )
    gp.append(", \\\n     ".join(parts))
    path = out / f"{name}.gp"
    path.write_text("\n".join(gp) + "\n")
    return path


def fig_boxes(
    name: str,
    points: dict,
    out: Path,
    ylabel: str,
    mono: bool,
    style: str = "box",
    ylog: bool = False,
) -> Path:
    """Figs. 5-7: operations on the x axis, a box (or a bar) per series."""
    series = list(points)
    cats = [c for c, _ in next(iter(points.values()))]
    k = len(series)
    width = 0.8 / k
    main, outl = [], []
    for i, s in enumerate(series):
        off = (i - (k - 1) / 2) * width
        m = [f"# {s}", "# x q1 whisker_lo whisker_hi q3 median ci_lo ci_hi"]
        o = [f"# {s}"]
        for c, (cat, st) in enumerate(points[s]):
            if st is None:
                continue
            x = c + off
            m.append(
                f"{x:g} {st.q1:g} {st.whisker_lo:g} {st.whisker_hi:g} {st.q3:g} {st.median:g} {st.ci_lo:g} {st.ci_hi:g}"
            )
            o += [f"{x:g} {v:g}" for v in st.outliers]
        main.append(m)
        outl.append(o if len(o) > 1 else o + ["# none"])
    has_out = [len(o) > 2 for o in outl]
    _write_blocks(out / f"{name}.dat", main)
    _write_blocks(out / f"{name}_outliers.dat", outl)

    gp = _header(name, mono, "", ylabel, series, ylog, solid=True)
    gp += [
        'set format y "%.0s%c"' if not ylog else 'set format y "10^{%L}"',
        "set xtics ("
        + ", ".join(f'"{c}" {i}' for i, c in enumerate(cats))
        + ') rotate by 30 right font "Helvetica,14"',
        f"set xrange [-0.6:{len(cats) - 0.4}]",
        f"set boxwidth {width * 0.85:g} absolute",
        "plot \\",
    ]
    parts = []
    for i, s in enumerate(series):
        build = s.partition("-")[2]
        fill = BUILD_STYLE.get(build, BUILD_STYLE[""])[2]
        if style == "bars":
            parts.append(
                f'"{name}.dat" index {i} using 1:6 title "{_title(s)}" with boxes ls {i + 1} fs {fill}'
            )
            parts.append(
                f'"{name}.dat" index {i} using 1:6:7:8 with yerrorbars ls {i + 1} lw 1 pt 0 notitle'
            )
        else:
            parts.append(
                f'"{name}.dat" index {i} using 1:2:3:4:5 title "{_title(s)}" with candlesticks ls {i + 1} '
                f"fs {fill} whiskerbars 0.6"
            )
            parts.append(
                f'"{name}.dat" index {i} using 1:6:6:6:6 with candlesticks ls {i + 1} lw 4 notitle'
            )
        if has_out[i]:
            parts.append(
                f'"{name}_outliers.dat" index {i} using 1:2 with points ls {i + 1} pt 6 ps 0.8 notitle'
            )
    gp.append(", \\\n     ".join(parts))
    path = out / f"{name}.gp"
    path.write_text("\n".join(gp) + "\n")
    return path


def table(rows: list[tuple]) -> pd.DataFrame:
    return pd.DataFrame(
        rows,
        columns=[
            "figure",
            "series",
            "point",
            "n",
            "median",
            "ci_lo",
            "ci_hi",
            "cv",
            "outliers",
        ],
    )


def _rows(fig: str, points: dict) -> list[tuple]:
    out = []
    for s, pts in points.items():
        for label, st in pts:
            if st is not None:
                out.append(
                    (
                        fig,
                        s,
                        label,
                        st.n,
                        st.median,
                        st.ci_lo,
                        st.ci_hi,
                        st.cv,
                        len(st.outliers),
                    )
                )
    return out


def render(gp: Path) -> bool:
    exe = shutil.which("gnuplot")
    if not exe:
        return False
    proc = subprocess.run([exe, gp.name], cwd=gp.parent, capture_output=True, text=True)
    if proc.returncode:
        raise FigureError(f"gnuplot {gp.name}: {proc.stderr.strip()}")
    return True


def make(
    dirs: list[Path],
    out: Path,
    mono: bool = False,
    style: str = "box",
    ylog_lat: bool = False,
    do_render: bool = True,
) -> list[Path]:
    """Writes every figure for which the results have data; returns the scripts."""
    df = prepare(dirs)
    out.mkdir(parents=True, exist_ok=True)
    scripts, rows = [], []

    def of(name: str) -> pd.DataFrame:
        return df[df["experiment"] == EXPERIMENT[name]]

    for name, rw, title in (
        ("fig3", "rw", "sequential read IOPS"),
        ("fig4", "randrw", "random read IOPS"),
    ):
        pts = scaling_points(of(name), rw) if len(of(name)) else {}
        if pts:
            scripts.append(fig_lines(name, pts, out, title, mono))
            rows += _rows(name, pts)

    specs = (
        ("fig5", OPTIMAL, "iops", "IOPS", False),
        (
            "fig6",
            NO_CONTENTION,
            "lat_p99_us",
            "99th percentile latency, {/Symbol m}s",
            ylog_lat,
        ),
        (
            "fig7",
            OPTIMAL,
            "lat_p99_us",
            "99th percentile latency, {/Symbol m}s",
            ylog_lat,
        ),
    )
    for name, conc, metric, ylabel, ylog in specs:
        sub = of(name)
        if sub.empty:
            continue
        pts = ops_points(sub, conc, metric)
        if pts:
            scripts.append(fig_boxes(name, pts, out, ylabel, mono, style, ylog))
            rows += _rows(name, pts)

    if not scripts:
        raise FigureError(
            "the results have no point of any figure: expected the experiments "
            + ", ".join(sorted(set(EXPERIMENT.values())))
        )
    table(rows).to_csv(out / "figures_table.csv", index=False)

    if do_render:
        rendered = [render(s) for s in scripts]
        if not all(rendered):
            print(
                "gnuplot is not installed: the .gp and .dat files are written, run `gnuplot figN.gp` in "
                + str(out)
            )
    return scripts
