#!/usr/bin/env bash
# Runs checkpatch.pl on the given files the way the lint workflow does: errors
# fail, warnings are only shown.
#
# checkpatch.pl is looked for in $CHECKPATCH, then src/checkpatch.pl and
# ./checkpatch.pl (both are git-ignored, put your own copy there), and
# otherwise downloaded once to ~/.cache/ls-bdd, from $CHECKPATCH_URL (the
# master of torvalds/linux by default, the one the CI uses).

set -u

url="${CHECKPATCH_URL:-https://raw.githubusercontent.com/torvalds/linux/master/scripts/checkpatch.pl}"
root="$(git rev-parse --show-toplevel)"

find_checkpatch() {
	local cand

	for cand in "${CHECKPATCH:-}" "$root/src/checkpatch.pl" "$root/checkpatch.pl"; do
		if [ -n "$cand" ] && [ -f "$cand" ]; then
			echo "$cand"
			return 0
		fi
	done

	cand="${XDG_CACHE_HOME:-$HOME/.cache}/ls-bdd/checkpatch.pl"
	if [ ! -f "$cand" ]; then
		local tmp

		mkdir -p "$(dirname "$cand")" || return 1
		tmp="$(mktemp "$cand.XXXXXX")" || return 1
		if ! curl -fsSL --retry 3 --retry-delay 2 "$url" -o "$tmp"; then
			rm -f "$tmp"
			return 1
		fi
		mv "$tmp" "$cand"
	fi

	echo "$cand"
}

[ "$#" -gt 0 ] || exit 0

if ! checkpatch="$(find_checkpatch)"; then
	echo "checkpatch.sh: no checkpatch.pl, and it could not be downloaded from $url" >&2
	echo "checkpatch.sh: set CHECKPATCH=/path/to/checkpatch.pl" >&2
	exit 1
fi

# Without spelling.txt and const_structs.checkpatch next to it, it only says so.
out="$(perl "$checkpatch" --no-tree --quiet --file "$@" 2>&1 | grep -vE '^No (typos|structs that should be const) will be found')"

[ -z "$out" ] || echo "$out"

if echo "$out" | grep -qE 'total: [1-9][0-9]* errors'; then
	echo "checkpatch.sh: errors found" >&2
	exit 1
fi

exit 0
