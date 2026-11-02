"""
Command line of the harness, run from test/ as root:

    python3 -m lsvtest load ../src/lsv.ko
    python3 -m lsvtest create lsv0 --backing /dev/ram0 --ds ht
    python3 -m lsvtest list
    python3 -m lsvtest destroy lsv0
    python3 -m lsvtest unload

    python3 -m lsvtest run experiments/smoke.toml [--dry-run]
    python3 -m lsvtest run --resume results/smoke-<time>
    python3 -m lsvtest report results/smoke-<time> [more result dirs]
"""

import argparse
import sys
from pathlib import Path

from . import ctl, spec

REPO = Path(__file__).resolve().parents[2]


def cmd_load(args) -> None:
    ctl.load_module(args.ko, {"stats_enabled": int(not args.no_stats)})


def cmd_unload(args) -> None:
    ctl.unload_module()


def cmd_create(args) -> None:
    spec = ctl.DeviceSpec(
        name=args.name,
        backing=args.backing,
        index_ds=args.ds,
        cell_size=args.cell_size,
        segment_size=args.segment_size,
        dedup=args.dedup,
    )
    dev = ctl.Device(spec).create()
    print(dev.path)


def cmd_destroy(args) -> None:
    ctl.Device(ctl.DeviceSpec(args.name, "", "")).destroy()


def cmd_stats(args) -> None:
    stats = ctl.read_stats(args.name)
    if stats is None:
        raise ctl.LsvError(f"no stats for {args.name}: built with stats=0?")
    for key, value in stats.items():
        print(key, value)


def cmd_list(args) -> None:
    for name in ctl.device_names():
        print(name)


def cmd_run(args) -> int:
    from . import runner

    if args.resume:
        out = args.resume
        exp = spec.load(out / "experiment.toml")
    elif args.experiment:
        exp = spec.load(args.experiment)
        out = runner.result_dir(args.out, exp)
    else:
        raise spec.SpecError("give an experiment file or --resume DIR")

    system = runner.DryRun(exp.backing_size_gib) if args.dry_run else runner.System()
    failed = runner.Runner(exp, out, system, REPO).run()
    if not args.dry_run:
        print(f"results: {out}")
    if failed:
        print(f"{failed} runs failed, see {out / 'failures.jsonl'}")
    return 1 if failed else 0


def cmd_report(args) -> None:
    from . import report

    out = args.out or args.dirs[0] / "report"
    report.report(args.dirs, out, args.metric or ("iops", "lat_p99_us"))
    print(f"report: {out}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lsvtest")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("load", help="insmod the module, mounting configfs if needed")
    p.add_argument("ko", type=Path, nargs="?", default=Path("../src/lsv.ko"))
    p.add_argument(
        "--no-stats", action="store_true", help="load with the stats probes off"
    )
    p.set_defaults(func=cmd_load)

    p = sub.add_parser("unload", help="destroy every device and rmmod the module")
    p.set_defaults(func=cmd_unload)

    p = sub.add_parser("create", help="create a device through configfs")
    p.add_argument("name")
    p.add_argument("--backing", required=True, help="backing device path")
    p.add_argument("--ds", required=True, help="mapping index (bt, ht, sl, rb)")
    p.add_argument("--cell-size", type=int, default=4096)
    p.add_argument("--segment-size", type=int)
    p.add_argument("--dedup", action="store_true")
    p.set_defaults(func=cmd_create)

    p = sub.add_parser("destroy", help="tear a device down")
    p.add_argument("name")
    p.set_defaults(func=cmd_destroy)

    p = sub.add_parser("stats", help="print a device's kernel stats")
    p.add_argument("name")
    p.set_defaults(func=cmd_stats)

    p = sub.add_parser("list", help="list the configured devices")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("run", help="run an experiment")
    p.add_argument("experiment", type=Path, nargs="?")
    p.add_argument("--out", type=Path, default=Path("results"))
    p.add_argument("--resume", type=Path, help="continue an interrupted run")
    p.add_argument("--dry-run", action="store_true", help="print, do not run")
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("report", help="summarise and plot result directories")
    p.add_argument("dirs", type=Path, nargs="+")
    p.add_argument("--out", type=Path)
    p.add_argument("--metric", action="append", help="metric to plot, repeatable")
    p.set_defaults(func=cmd_report)

    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args) or 0
    except (ctl.LsvError, spec.SpecError) as e:
        print(f"lsvtest: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
