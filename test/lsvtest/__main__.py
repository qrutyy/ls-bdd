"""
Command line of the harness, run from test/ as root:

    python3 -m lsvtest load ../src/lsbdd.ko
    python3 -m lsvtest create lsvbd1 --backing /dev/ram0 --ds ht
    python3 -m lsvtest list
    python3 -m lsvtest destroy lsvbd1
    python3 -m lsvtest unload

    python3 -m lsvtest run experiments/smoke.toml [--dry-run] [--ko ../src/lsbdd.ko] [--tag mode=sy]
    python3 -m lsvtest run --resume results/smoke-<time>
    python3 -m lsvtest report results/smoke-<time> [more result dirs]
    python3 -m lsvtest figures results/paper_*-<time> --out figures    # gnuplot, EPS
"""

import argparse
import json
import sys
from pathlib import Path

from . import ctl, spec

REPO = Path(__file__).resolve().parents[2]


def cmd_load(args) -> None:
    ctl.load_module(args.ko)


def cmd_unload(args) -> None:
    ctl.unload_module()


def cmd_create(args) -> None:
    spec = ctl.DeviceSpec(
        name=args.name,
        backing=args.backing,
        index_ds=args.ds,
    )
    dev = ctl.Device(spec).create()
    print(dev.path)


def cmd_destroy(args) -> None:
    ctl.Device(ctl.DeviceSpec(args.name, "", "")).destroy()


def cmd_list(args) -> None:
    for name in ctl.device_names():
        print(name)


def cmd_run(args) -> int:
    from . import runner

    if args.resume:
        out = args.resume
        saved = out / "overrides.json"
        overrides = json.loads(saved.read_text()) if saved.exists() else {}
        exp = spec.load(
            out / "experiment.toml", overrides.get("tags"), overrides.get("ds")
        )
    elif args.experiment:
        exp = spec.load(args.experiment, spec.parse_tag_args(args.tag), args.ds)
        out = runner.result_dir(args.out, exp)
    else:
        raise spec.SpecError("give an experiment file or --resume DIR")

    if args.dry_run:
        system = runner.DryRun(exp.backing_size_gib)
    else:
        system = runner.System(args.ko)
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


def cmd_figures(args) -> None:
    from . import figures

    scripts = figures.make(
        args.dirs,
        args.out,
        mono=args.mono,
        style=args.style,
        ylog_lat=args.log_lat,
        do_render=not args.no_render,
    )
    print(f"figures: {', '.join(s.stem for s in scripts)} in {args.out}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lsvtest")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("load", help="insmod the module")
    p.add_argument("ko", type=Path, nargs="?", default=Path("../src/lsbdd.ko"))
    p.set_defaults(func=cmd_load)

    p = sub.add_parser("unload", help="destroy every device and rmmod the module")
    p.set_defaults(func=cmd_unload)

    p = sub.add_parser("create", help="create a device through module parameters")
    p.add_argument("name", help="lsvbd<N>")
    p.add_argument("--backing", required=True, help="backing device path")
    p.add_argument("--ds", required=True, help="mapping index (bt, ht, sl, rb)")
    p.set_defaults(func=cmd_create)

    p = sub.add_parser("destroy", help="tear a device down")
    p.add_argument("name")
    p.set_defaults(func=cmd_destroy)

    p = sub.add_parser("list", help="list the configured devices")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("run", help="run an experiment")
    p.add_argument("experiment", type=Path, nargs="?")
    p.add_argument("--out", type=Path, default=Path("results"))
    p.add_argument("--resume", type=Path, help="continue an interrupted run")
    p.add_argument("--dry-run", action="store_true", help="print, do not run")
    p.add_argument(
        "--ds",
        action="append",
        default=[],
        help="run only this index of the experiment, repeatable",
    )
    p.add_argument(
        "--tag",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="set a tag of the experiment, repeatable (e.g. the build: --tag mode=sy)",
    )
    p.add_argument(
        "--ko",
        type=Path,
        default=Path("../src/lsbdd.ko"),
        help="module reloaded before every run",
    )
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("report", help="summarise and plot result directories")
    p.add_argument("dirs", type=Path, nargs="+")
    p.add_argument("--out", type=Path)
    p.add_argument("--metric", action="append", help="metric to plot, repeatable")
    p.set_defaults(func=cmd_report)

    p = sub.add_parser(
        "figures", help="the figures of the paper as gnuplot scripts (and EPS)"
    )
    p.add_argument(
        "dirs",
        type=Path,
        nargs="+",
        help="result directories, every build of every paper_* experiment",
    )
    p.add_argument("--out", type=Path, default=Path("figures"))
    p.add_argument(
        "--mono",
        action="store_true",
        help="black and white, told apart by dashes and marks",
    )
    p.add_argument(
        "--style",
        choices=("box", "bars"),
        default="box",
        help="figs. 5-7: box plots or bars with the interval",
    )
    p.add_argument(
        "--log-lat", action="store_true", help="logarithmic axis of the latency figures"
    )
    p.add_argument(
        "--no-render", action="store_true", help="write the scripts and data only"
    )
    p.set_defaults(func=cmd_figures)

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
