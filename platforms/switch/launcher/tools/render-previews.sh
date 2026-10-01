#!/usr/bin/env bash
# Renders the launcher's reference screenshots with the headless preview.
#
#   tools/render-previews.sh [host build dir] [output dir]
#
# Each run builds a throwaway demo SD card (synthetic discs only) and drives
# the real UI through a script; see host/preview_main.cpp for the commands.
set -euo pipefail

build="${1:-build/launcher-host}"
out="${2:-build/launcher-screens}"
preview="$build/partyboard_launcher_preview"
sd="$(mktemp -d)"
trap 'rm -rf "$sd"' EXIT
mkdir -p "$out" "$sd/empty"

"$preview" --demo-sd "$sd/full" --out "$out" --script "\
  wait 1.2; shot 01-shelf; \
  press right; wait 0.6; shot 02-shelf-pal; \
  press x; wait 0.6; shot 03-options; press b; wait 0.5; \
  press y; wait 0.6; shot 04-controllers; press b; wait 0.5; \
  press a; wait 1.6; shot 05-boot-trace; wait 1.0; shot 06-boot-slam; wait 1.3; shot 07-boot-logo"

"$preview" --demo-sd "$sd/japan" --out "$out" --script "\
  lang en; players 2; wait 1.0; \
  press right; wait 0.2; press right; wait 0.6; shot 08-shelf-english; \
  press a; wait 0.5; shot 09-unsupported"

"$preview" --demo-sd "$sd/noengine" --no-engine --out "$out" --script "\
  wait 1.0; press a; wait 0.5; shot 10-engine-missing"

"$preview" --root "$sd/empty" --out "$out" --script "wait 1.0; shot 11-empty"

"$preview" --demo-sd "$sd/surprise" --out "$out" --script "\
  wait 1.0; press a; hold zr 1.0; wait 2.6; shot 12-boot-surprise"

ls "$out"
