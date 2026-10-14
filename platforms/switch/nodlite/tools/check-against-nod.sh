#!/usr/bin/env bash
# Runs nodlite's test suite against the real nod library, so the API
# behaviour nodlite copies (results, seek rules, error codes) is checked
# against the original instead of against itself.
#
#   tools/check-against-nod.sh [nod checkout]
#
# Without a checkout, nod is cloned at the tag Aurora pins
# (AURORA_NOD_VERSION in extern/aurora/CMakeLists.txt). Needs cargo, a C++20
# compiler and the zlib, bzip2, zstd and xz development packages.
# Checks marked nodlite-only in tests/nodlite_tests.cpp are left out.
set -euo pipefail

tag="v2.0.0-alpha.10"
here="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

nod="${1:-}"
if [[ -z "$nod" ]]; then
    nod="$work/nod"
    git clone --quiet --depth 1 --branch "$tag" https://github.com/encounter/nod "$nod"
fi

CARGO_TARGET_DIR="$work/target" cargo build --quiet --release --locked \
    --manifest-path "$nod/Cargo.toml" -p nod-ffi

# The suite must agree with the header nodlite vendors.
diff -q "$nod/nod-ffi/include/nod.h" "$here/include/nod.h"

"${CXX:-c++}" -std=c++20 -O1 -DNODLITE_TESTS_AGAINST_NOD \
    -I"$here/tests" -I"$here/src" -I"$nod/nod-ffi/include" \
    "$here/tests/nodlite_tests.cpp" "$here/src/lfg.cpp" "$work/target/release/libnod.a" \
    -lzstd -lz -lbz2 -llzma -lpthread -ldl -lm -o "$work/nod_reference_tests"

cd "$work"
./nod_reference_tests "$here/tests/fixtures"
