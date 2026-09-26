#!/usr/bin/env bash
# Publishes a signed APK as the update the Android app offers (src/port/app_update.cpp).
#
#   publish-update.sh <signed apk> <abi> <versionCode> <versionName> [channel]
#
# channel: android (the phone APK, the default) or quest (the Meta Quest APK,
# which installs its updates in the headset: quest/QuestUpdater.java).
#
# Everything goes to one GitHub release, partyboard-android-latest:
#   PartyBoard-<channel>-<abi>-<code>.apk  this build; the 5 newest stay, older ones are removed
#   PartyBoard-<channel>-<abi>.apk         the same file under a name that never changes, to
#                                          install the app the first time
#   <channel>-update.json                  the manifest the app reads, uploaded last so it never
#                                          names an APK that is not there yet
#
# The notes are the commits since the build the previous manifest named, so the app shows what
# changed. Needs gh (GH_TOKEN), jq, sha256sum and the full git history.
set -euo pipefail

if [ "$#" -ne 4 ] && [ "$#" -ne 5 ]; then
    echo "usage: $0 <signed apk> <abi> <versionCode> <versionName> [android|quest]" >&2
    exit 2
fi

APK=$1
ABI=$2
CODE=$3
NAME=$4
CHANNEL=${5:-android}
MANIFEST="$CHANNEL-update.json"
TAG=partyboard-android-latest
REPO=${GITHUB_REPOSITORY:?}
COMMIT=$(git rev-parse HEAD)
KEEP=5
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

VERSIONED="PartyBoard-$CHANNEL-$ABI-$CODE.apk"
STABLE="PartyBoard-$CHANNEL-$ABI.apk"
cp "$APK" "$WORK/$VERSIONED"
cp "$APK" "$WORK/$STABLE"
SHA256=$(sha256sum "$APK" | cut -d' ' -f1)

# What changed since the build players have now.
PREVIOUS=""
if gh release download "$TAG" --repo "$REPO" --pattern "$MANIFEST" --dir "$WORK/previous" 2>/dev/null; then
    PREVIOUS=$(jq -r '.commit // empty' "$WORK/previous/$MANIFEST")
fi
if [ -n "$PREVIOUS" ] && git cat-file -e "$PREVIOUS^{commit}" 2>/dev/null && git merge-base --is-ancestor "$PREVIOUS" HEAD; then
    CHANGES=$(git log --no-merges --max-count=40 --format='- %s' "$PREVIOUS..HEAD")
else
    CHANGES=$(git log --no-merges --format='- %s' -n 15 HEAD)
fi
if [ -z "$CHANGES" ]; then
    CHANGES="- Same code as the previous build, built again."
fi

jq -n \
    --argjson versionCode "$CODE" \
    --arg version "$NAME (build $CODE)" \
    --arg abi "$ABI" \
    --arg commit "$COMMIT" \
    --arg downloadUrl "https://github.com/$REPO/releases/download/$TAG/$VERSIONED" \
    --arg sha256 "$SHA256" \
    --arg notes "$CHANGES" \
    '{versionCode: $versionCode, version: $version, abi: $abi, commit: $commit, downloadUrl: $downloadUrl, sha256: $sha256, notes: $notes}' \
    > "$WORK/$MANIFEST"
cat "$WORK/$MANIFEST"

BODY="Party Board for Android and Meta Quest, build $CODE ($COMMIT).

Phone: download PartyBoard-android-$ABI.apk on the phone and open it.
Meta Quest: on a PC, with the headset plugged in and developer mode on, run platforms/android/scripts/install-quest.cmd (it installs PartyBoard-quest-$ABI.apk).
Once installed, the app finds the next builds itself (Settings > Interface > Check for Updates); the headset installs them in place.

Changes:
$CHANGES"

if ! gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    gh release create "$TAG" --repo "$REPO" --target "$COMMIT" --prerelease \
        --title "Party Board Android - latest build" --notes "$BODY"
fi
gh release upload "$TAG" --repo "$REPO" --clobber "$WORK/$VERSIONED" "$WORK/$STABLE"
gh release upload "$TAG" --repo "$REPO" --clobber "$WORK/$MANIFEST"
gh release edit "$TAG" --repo "$REPO" --notes "$BODY"

# Keep the newest builds of this ABI; the manifest only ever names the newest.
gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets[].name' \
    | { grep -E "^PartyBoard-$CHANNEL-$ABI-[0-9]+\.apk$" || true; } \
    | sed -E 's/.*-([0-9]+)\.apk$/\1 &/' \
    | sort -rn \
    | tail -n +$((KEEP + 1)) \
    | while read -r _ asset; do
        gh release delete-asset "$TAG" "$asset" --repo "$REPO" --yes
    done
