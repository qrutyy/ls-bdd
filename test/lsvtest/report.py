"""
Aggregation of runs.csv into per-point statistics and plots.

Every point is summarised by the median over its repeats with a bootstrap
95% confidence interval of that median: the run-to-run distribution of IOPS
and tail latency is rarely normal, so mean +- stddev would mislead.
The coefficient of variation flags points that need more repeats.
"""

from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

from .fio import ROW_FIELDS
from .spec import RAW, Point

METRICS = {
    "iops": "IOPS",
    "bw_mib": "Bandwidth, MiB/s",
    "lat_mean_us": "Mean latency, us",
    "lat_p99_us": "p99 latency, us",
    "lat_p999_us": "p99.9 latency, us",
    "clat_p99_us": "p99 completion latency, us",
    "sys_cpu": "System CPU, %",
}

DS_COLORS = {
    "sl": "steelblue",
    "ht": "indianred",
    "rb": "seagreen",
    "bt": "darkkhaki",
    RAW: "dimgray",
}

# Driver stages in the order a request meets them, see src/core/stats/stats.h.
STAGES = [
    "bio",
    "submit",
    "remap_read",
    "remap_write",
    "idx_lookup",
    "idx_insert",
    "dedup_digest",
    "dedup_verify",
]

KSTAT_PREFIX = "k_"

BOOTSTRAP_RESAMPLES = 2000
CONFIDENCE = 0.95


def load_runs(dirs: list[Path]) -> pd.DataFrame:
    frames = []
    for d in dirs:
        runs = Path(d) / "runs.csv"
        if not runs.exists():
            raise FileNotFoundError(f"{runs} does not exist")
        frames.append(pd.read_csv(runs))
    df = pd.concat(frames, ignore_index=True)
    df["bw_mib"] = df["bw_bytes"] / (1 << 20)
    return df


def load_kstats(dirs: list[Path]) -> Optional[pd.DataFrame]:
    """
    The driver's stats, one k_<key> column per key, joined with the point of
    each run. None when no run recorded any (stats=0 build, baseline only).
    """
    frames = []
    for d in dirs:
        path = Path(d) / "kstats.csv"
        if path.exists():
            frames.append(pd.read_csv(path))
    if not frames:
        return None
    wide = pd.concat(frames).pivot_table(
        index="run_id", columns="key", values="value", aggfunc="first"
    )
    wide.columns = [f"{KSTAT_PREFIX}{c}" for c in wide.columns]
    runs = load_runs(dirs)
    keys = [c for c in group_columns(runs) if c != "direction"]
    points = runs[["run_id"] + keys].drop_duplicates("run_id")
    return points.merge(wide.reset_index(), on="run_id")


def summarize_kstats(kdf: pd.DataFrame) -> pd.DataFrame:
    """Median over repeats of every driver stat, per point."""
    values = [c for c in kdf.columns if c.startswith(KSTAT_PREFIX)]
    keys = [c for c in kdf.columns if c not in values and c != "run_id"]
    return kdf.groupby(keys, dropna=False)[values].median().reset_index()


def bootstrap_ci(values, seed: int = 0) -> tuple[float, float]:
    values = np.asarray(values, dtype=float)
    values = values[~np.isnan(values)]
    if len(values) < 2:
        v = values[0] if len(values) else np.nan
        return v, v
    rng = np.random.default_rng(seed)
    samples = rng.choice(values, size=(BOOTSTRAP_RESAMPLES, len(values)))
    medians = np.median(samples, axis=1)
    alpha = (1 - CONFIDENCE) / 2
    return float(np.quantile(medians, alpha)), float(np.quantile(medians, 1 - alpha))


def group_columns(df: pd.DataFrame) -> list[str]:
    """Point fields, tags and direction: everything but the measurements."""
    skip = {"run_id", "repeat", "time", "bw_mib"} | set(_measurements(df))
    return [c for c in df.columns if c not in skip]


def _measurements(df: pd.DataFrame) -> list[str]:
    return [c for c in ROW_FIELDS if c != "direction" and c in df.columns]


def summarize(df: pd.DataFrame) -> pd.DataFrame:
    keys = group_columns(df)
    rows = []
    for key, group in df.groupby(keys, dropna=False, sort=True):
        row = dict(zip(keys, key))
        row["n"] = len(group)
        for metric in METRICS:
            if metric not in group:
                continue
            values = group[metric].astype(float)
            lo, hi = bootstrap_ci(values)
            row[f"{metric}_median"] = float(values.median())
            row[f"{metric}_ci_lo"] = lo
            row[f"{metric}_ci_hi"] = hi
        mean = group["iops"].mean()
        row["iops_cv"] = float(group["iops"].std(ddof=1) / mean) if mean else np.nan
        rows.append(row)
    return pd.DataFrame(rows)


def _workload(row) -> str:
    rw = row["rw"]
    if pd.notna(row.get("rwmixread")):
        rw += f":{int(row['rwmixread'])}"
    return f"{rw} / {row['direction']}"


def _fmt(col: str, value) -> str:
    if col == "bs" or col == "cell_size":
        value = int(value)
        return f"{col}={value >> 10}k" if value >= 1024 else f"{col}={value}"
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    return f"{col}={value}"


def plot(summary: pd.DataFrame, metric: str, out: Path) -> Path:
    """
    One panel per workload and direction; bars are data structures (the
    baseline in grey), grouped by whatever else varies in the experiment.
    """
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    df = summary.copy()
    df["workload"] = df.apply(_workload, axis=1)
    fixed = {"ds", "target", "rw", "rwmixread", "direction", "workload", "experiment"}
    candidates = [c for c in Point.__dataclass_fields__ if c not in fixed]
    varying = [c for c in candidates if df[c].dropna().nunique() > 1]

    def label(row) -> str:
        parts = [_fmt(c, row[c]) for c in varying if pd.notna(row[c])]
        return "\n".join(parts) or "all"

    df["x"] = df.apply(label, axis=1)

    workloads = sorted(df["workload"].unique())
    cols = min(2, len(workloads))
    rows = -(-len(workloads) // cols)
    fig, axes = plt.subplots(
        rows, cols, figsize=(7 * cols, 4 * rows), squeeze=False, constrained_layout=True
    )
    ds_order = sorted(df["ds"].unique(), key=lambda d: (d == RAW, d))

    for ax, workload in zip(axes.flat, workloads):
        part = df[df["workload"] == workload]
        xs = sorted(part["x"].unique())
        width = 0.8 / len(ds_order)
        for i, ds in enumerate(ds_order):
            sel = part[part["ds"] == ds].set_index("x").reindex(xs)
            med = sel[f"{metric}_median"]
            err = np.vstack(
                [med - sel[f"{metric}_ci_lo"], sel[f"{metric}_ci_hi"] - med]
            )
            ax.bar(
                np.arange(len(xs)) + (i - (len(ds_order) - 1) / 2) * width,
                med,
                width,
                yerr=err,
                capsize=3,
                label=ds,
                color=DS_COLORS.get(ds),
            )
        ax.set_xticks(np.arange(len(xs)), xs)
        ax.set_title(workload)
        ax.set_ylabel(METRICS[metric])
        ax.grid(axis="y", alpha=0.3)
    for ax in list(axes.flat)[len(workloads) :]:
        ax.set_visible(False)
    axes.flat[0].legend(title="index")

    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{metric}.png"
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def plot_stages(
    ksummary: pd.DataFrame, out: Path, stat: str = "mean"
) -> Optional[Path]:
    """
    Where the time goes inside the driver: per workload, the median over
    repeats of every stage's mean (or percentile) latency, by index.
    Stages nest (bio contains submit, which contains the remaps, which
    contain the index operations), so the bars are not to be summed.
    """
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    df = ksummary.copy()
    df["workload"] = df.apply(
        lambda r: r["rw"]
        + (f":{int(r['rwmixread'])}" if pd.notna(r.get("rwmixread")) else ""),
        axis=1,
    )
    stages = [
        s
        for s in STAGES
        if f"k_lat_{s}_{stat}_ns" in df and df[f"k_lat_{s}_count"].fillna(0).sum() > 0
    ]
    if not stages:
        return None
    workloads = sorted(df["workload"].unique())
    ds_order = sorted(df["ds"].unique())

    fig, axes = plt.subplots(
        len(workloads),
        1,
        figsize=(max(6, 1.4 * len(stages)), 3.5 * len(workloads)),
        squeeze=False,
        constrained_layout=True,
    )
    width = 0.8 / len(ds_order)
    for ax, workload in zip(axes.flat, workloads):
        part = df[df["workload"] == workload]
        for i, ds in enumerate(ds_order):
            sel = part[part["ds"] == ds]
            values = [sel[f"k_lat_{s}_{stat}_ns"].median() for s in stages]
            ax.bar(
                np.arange(len(stages)) + (i - (len(ds_order) - 1) / 2) * width,
                values,
                width,
                label=ds,
                color=DS_COLORS.get(ds),
            )
        ax.set_xticks(np.arange(len(stages)), stages)
        ax.set_yscale("log")
        ax.set_ylabel(f"{stat} latency, ns")
        ax.set_title(workload)
        ax.grid(axis="y", alpha=0.3, which="both")
    axes.flat[0].legend(title="index")

    out.mkdir(parents=True, exist_ok=True)
    path = out / f"stages_{stat}.png"
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def report(dirs: list[Path], out: Path, metrics=("iops", "lat_p99_us")) -> Path:
    df = load_runs(dirs)
    summary = summarize(df)
    out.mkdir(parents=True, exist_ok=True)
    summary.to_csv(out / "summary.csv", index=False)
    for metric in metrics:
        plot(summary, metric, out)

    kdf = load_kstats(dirs)
    if kdf is not None:
        ksummary = summarize_kstats(kdf)
        ksummary.to_csv(out / "kstats_summary.csv", index=False)
        for stat in ("mean", "p99"):
            plot_stages(ksummary, out, stat)
    return out
