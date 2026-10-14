#!/usr/bin/env bash
# Regenerates tests/fixtures/*.rvz from the test disc with nod's own nodtool.
#
#   tools/make-fixtures.sh <nodlite build dir> <nodtool binary>
#
# nodtool is nod v2.0.0-alpha.10's CLI (cargo build --release -p nodtool in a
# checkout of https://github.com/encounter/nod at that tag). It stores junk as
# seeds only when the junk matches its own generator, so a small
# test-none.rvz shows nodlite's generator and nod's agree.
set -euo pipefail

build="${1:?nodlite build dir}"
nodtool="${2:?nodtool binary}"
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/tests/fixtures"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"$build/nodlite_tests" --write-iso "$tmp/test.iso"
"$nodtool" convert -c zstd:19 "$tmp/test.iso" "$out/test-zstd.rvz"
"$nodtool" convert -c none "$tmp/test.iso" "$out/test-none.rvz"
"$nodtool" convert -c bzip2:9 "$tmp/test.iso" "$out/test-bzip2.rvz"
ls -l "$out"
