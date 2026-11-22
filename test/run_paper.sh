#!/bin/bash
# The experiments of the paper figures for every build of the module, and the
# figures. Run from test/ as root:
#
#   sudo ./run_paper.sh            # build, measure, plot
#   sudo ./run_paper.sh --dry-run  # print what would be run
#
# Builds: lf (lock-free), bl (blocking, a spinlock per bucket), optik (blocking,
# an OPTIK lock per bucket; only the hash table differs, so only it is run).
# Experiments: paper_scaling (figs. 3-4), paper_ops (fig. 5), paper_lat (figs. 6-7).
# The builds take turns within an experiment, so a drift of the machine (a warm
# drive, the temperature) is shared by all of them.
#
# Results: $OUT/<build>/<experiment>-<time>/. An interrupted experiment goes on with
#   python3 -m lsvtest run --resume $OUT/<build>/<experiment>-<time> --ko build/lsbdd-<build>.ko
# and the figures are made again with the last command of this script.
#
# Environment: OUT (results/paper), FIG (figures), KO_DIR (build), KDIR (the kernel
# build tree), PYTHON (python3).

set -euo pipefail
cd "$(dirname "$0")"

DRY=""
[ "${1:-}" = "--dry-run" ] && DRY="--dry-run"

OUT=${OUT:-results/paper}
FIG=${FIG:-figures}
KO_DIR=${KO_DIR:-build}
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}
PY=${PYTHON:-python3}
SRC=$(cd ../src && pwd)

build() { # build name, make arguments
	local name=$1
	shift
	make -C "$KDIR" M="$SRC" clean >/dev/null
	make -C "$SRC" "$@" modules
	cp "$SRC/lsbdd.ko" "$KO_DIR/lsbdd-$name.ko"
}

run() { # build name, experiment, lsvtest arguments
	local name=$1 exp=$2
	shift 2
	$PY -m lsvtest run "experiments/$exp.toml" --ko "$KO_DIR/lsbdd-$name.ko" --out "$OUT/$name" $DRY "$@"
}

if [ -z "$DRY" ]; then
	mkdir -p "$KO_DIR"
	build lf type=lf
	build bl type=sy ht=bucket
	build optik type=sy ht=optik
fi

for exp in paper_scaling paper_ops paper_lat; do
	run lf "$exp" --tag mode=lf
	run bl "$exp" --tag mode=sy --tag variant=bucket
	run optik "$exp" --tag mode=sy --tag variant=optik --ds ht
done

if [ -z "$DRY" ]; then
	$PY -m lsvtest figures "$OUT"/*/* --out "$FIG"
fi
