"""
Aggregation of runs.csv into per-point statistics and plots.

Every point is summarised by the median over its repeats with a bootstrap
95% confidence interval of that median: the run-to-run distribution of IOPS
and tail latency is rarely normal, so mean +- stddev would mislead.
The coefficient of variation flags points that need more repeats.
"""

from pathlib import Path

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
    if col == "bs":
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


def report(dirs: list[Path], out: Path, metrics=("iops", "lat_p99_us")) -> Path:
    df = load_runs(dirs)
    summary = summarize(df)
    out.mkdir(parents=True, exist_ok=True)
    summary.to_csv(out / "summary.csv", index=False)
    for metric in metrics:
        plot(summary, metric, out)
    return out
