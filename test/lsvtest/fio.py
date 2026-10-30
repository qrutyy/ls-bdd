"""
fio command lines and the parsing of fio's JSON output into flat rows.

Latencies are taken from fio itself (clat/slat/lat percentiles), never
recomputed from the per-IO logs: fio keeps every sample in its histograms,
so its percentiles are exact up to the bin width.
"""

from typing import Optional

from .spec import Point

PERCENTILES = ("50", "90", "99", "99.9", "99.99")
LAT_KINDS = ("slat", "clat", "lat")
DIRECTIONS = ("read", "write")


def _metric_name(p: str) -> str:
    return "p" + p.replace(".", "")


ROW_FIELDS = [
    "direction",
    "iops",
    "bw_bytes",
    "io_bytes",
    "total_ios",
    "runtime_ms",
    "usr_cpu",
    "sys_cpu",
    "error",
] + [
    f"{kind}_{stat}_us"
    for kind in LAT_KINDS
    for stat in ["mean", "max"] + [_metric_name(p) for p in PERCENTILES]
]


def job_args(
    point: Point,
    filename: str,
    fio_cfg: dict,
    size: int,
    io_size: Optional[int] = None,
) -> list[str]:
    """
    |size| bounds the region the workload touches. With |io_size| the job
    stops after that many bytes per job (or at runtime, whichever is first);
    without it the job runs for the whole runtime. String values of |extra|
    may refer to {numjobs}, {iodepth} and {last_cpu} of the point.
    """
    args = [
        "fio",
        "--name=lsvbench",
        f"--filename={filename}",
        "--direct=1",
        f"--ioengine={fio_cfg.get('ioengine', 'io_uring')}",
        f"--rw={point.rw}",
        f"--bs={point.bs}",
        f"--numjobs={point.numjobs}",
        f"--iodepth={point.iodepth}",
        f"--size={size}",
        f"--runtime={fio_cfg.get('runtime', 30)}",
        "--group_reporting=1",
        "--output-format=json",
        "--clat_percentiles=1",
        "--lat_percentiles=1",
        "--slat_percentiles=1",
        f"--percentile_list={':'.join(PERCENTILES)}",
    ]
    if point.rwmixread is not None:
        args.append(f"--rwmixread={point.rwmixread}")
    if io_size is None:
        args.append("--time_based=1")
    else:
        args.append(f"--io_size={io_size}")
    for key, value in fio_cfg.get("extra", {}).items():
        if isinstance(value, str):
            # e.g. cpus_allowed = "0-{last_cpu}": one CPU per job
            value = value.format(
                numjobs=point.numjobs,
                iodepth=point.iodepth,
                last_cpu=point.numjobs - 1,
            )
        args.append(f"--{key}={value}")
    return args


def precondition_args(filename: str, size: int, bs: int = 128 << 10) -> list[str]:
    """One sequential pass over the region, so that reads hit mapped cells."""
    return [
        "fio",
        "--name=precondition",
        f"--filename={filename}",
        "--direct=1",
        "--ioengine=io_uring",
        "--rw=write",
        f"--bs={bs}",
        "--iodepth=32",
        f"--size={size}",
        "--output-format=json",
    ]


def _percentile(stats: dict, p: str) -> Optional[float]:
    table = stats.get("percentile") or {}
    for key, value in table.items():
        if float(key) == float(p):
            return value / 1000
    return None


def parse(result: dict) -> list[dict]:
    """
    One row per direction that saw IO. Latencies are in microseconds,
    bandwidth in bytes per second. Expects --group_reporting, so there is
    a single job entry.
    """
    jobs = result.get("jobs") or []
    if not jobs:
        raise ValueError("fio output has no jobs")
    job = jobs[0]

    rows = []
    for direction in DIRECTIONS:
        d = job.get(direction) or {}
        if not d.get("total_ios"):
            continue
        row = {
            "direction": direction,
            "iops": d.get("iops", 0.0),
            "bw_bytes": d.get("bw_bytes", 0),
            "io_bytes": d.get("io_bytes", 0),
            "total_ios": d.get("total_ios", 0),
            "runtime_ms": d.get("runtime", 0),
            "usr_cpu": job.get("usr_cpu"),
            "sys_cpu": job.get("sys_cpu"),
            "error": job.get("error", 0),
        }
        for kind in LAT_KINDS:
            stats = d.get(f"{kind}_ns") or {}
            row[f"{kind}_mean_us"] = stats.get("mean", 0) / 1000
            row[f"{kind}_max_us"] = stats.get("max", 0) / 1000
            for p in PERCENTILES:
                row[f"{kind}_{_metric_name(p)}_us"] = _percentile(stats, p)
        rows.append(row)
    return rows
