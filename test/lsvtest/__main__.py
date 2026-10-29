"""
Command line of the harness, run from test/ as root:

    python3 -m lsvtest load ../src/lsv.ko
    python3 -m lsvtest create lsv0 --backing /dev/ram0 --ds ht
    python3 -m lsvtest list
    python3 -m lsvtest destroy lsv0
    python3 -m lsvtest unload
"""

import argparse
import sys
from pathlib import Path

from . import ctl


def cmd_load(args) -> None:
    ctl.load_module(args.ko)


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


def cmd_list(args) -> None:
    for name in ctl.device_names():
        print(name)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="lsvtest")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("load", help="insmod the module, mounting configfs if needed")
    p.add_argument("ko", type=Path, nargs="?", default=Path("../src/lsv.ko"))
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

    p = sub.add_parser("list", help="list the configured devices")
    p.set_defaults(func=cmd_list)

    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except ctl.LsvError as e:
        print(f"lsvtest: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
