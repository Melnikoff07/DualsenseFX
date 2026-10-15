#!/bin/bash
# Build the release zip from the built parts: tools/package.sh VERSION [OUT_DIR]
# Needs MissionControl/dist (make dist in MissionControl), dist (make dist) and overlay/dualsense_fx.ovl.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$1"
OUT_DIR="${2:-$ROOT/release}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

for f in "$ROOT/MissionControl/dist/atmosphere/contents/010000000000bd00/exefs.nsp" \
         "$ROOT/dist/atmosphere/contents/42000000000d5e00/exefs.nsp" "$ROOT/overlay/dualsense_fx.ovl"; do
    [ -f "$f" ] || { echo "missing $f, build first"; exit 1; }
done

cp -R "$ROOT/MissionControl/dist/atmosphere" "$STAGE/"
cp -R "$ROOT/dist/atmosphere/contents/42000000000d5e00" "$STAGE/atmosphere/contents/"
mkdir -p "$STAGE/switch/.overlays" "$STAGE/config/adaptive_triggers"
cp "$ROOT/overlay/dualsense_fx.ovl" "$STAGE/switch/.overlays/"
cp "$ROOT/sd/config.ini" "$ROOT/sd/patterns.ini" "$ROOT/sd/sounds.ini" "$STAGE/config/adaptive_triggers/"
find "$STAGE" -name '.DS_Store' -delete

mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/DualsenseFX-BotW-$VERSION.zip"
rm -f "$OUT"
(cd "$STAGE" && zip -qrX "$OUT" atmosphere switch config)
echo "$OUT"
unzip -l "$OUT" | grep -v exefs_patches | tail -n +4
