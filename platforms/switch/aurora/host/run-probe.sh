#!/usr/bin/env bash
# Runs the host build of the Aurora probe on Mesa's software rasterisers, under
# Xvfb when there is no display: OpenGL ES at Dawn's Compatibility level (what
# the Switch runs) and Vulkan as the reference. For each backend it
#   - runs 300 frames, which must exit 0 with no Aurora error or fatal log;
#   - runs again and takes a screenshot (<out>/aurora-probe-<backend>.png).
#
#   run-probe.sh <build dir> <out dir>
set -euo pipefail

build="$(cd "${1:?build dir}" && pwd)"
out="${2:?out dir}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
probe="$build/aurora_probe"

if [[ -z "${DISPLAY:-}" ]]; then
    export DISPLAY=:97
    Xvfb "$DISPLAY" -screen 0 1280x960x24 >/dev/null 2>&1 &
    xvfb=$!
    trap 'kill $xvfb 2>/dev/null || true' EXIT
    sleep 2
fi

status=0
for backend in gles vulkan; do
    log="$out/aurora-probe-$backend.log"
    if ! timeout 180 "$probe" --backend "$backend" --frames 300 >"$log" 2>&1; then
        echo "FAIL $backend: the probe did not finish 300 frames cleanly"
        status=1
    fi
    if grep -E '^\[(ERROR|FATAL)' "$log"; then
        echo "FAIL $backend: Aurora logged errors"
        status=1
    fi
    grep -E 'API:|Device:' "$log" | sed "s/^/$backend: /" || true

    timeout 60 "$probe" --backend "$backend" --frames 1000000 >/dev/null 2>&1 &
    pid=$!
    sleep 10
    import -window root "$out/aurora-probe-$backend.png" || status=1
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
done
exit $status
