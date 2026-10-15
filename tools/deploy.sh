#!/bin/bash
# Copy the builds onto a Switch SD card mounted on this computer.
#   tools/deploy.sh             -> MissionControl fork + DualsenseFX module, overlay and game profiles
#   tools/deploy.sh --rollback  -> restore your original MissionControl and remove adaptive_triggers
#
# SD     mount point of the SD card (default /Volumes/NS_SAMSUNG on macOS)
# MC_BACKUP  folder with your original atmosphere/contents/010000000000bd00 (for --rollback),
#            default: the newest backups/MissionControl-* folder in this repository (git-ignored)
#
# Only touches atmosphere/contents/010000000000bd00, atmosphere/contents/42000000000d5e00
# switch/.overlays/dualsense_fx.ovl and config/dualsensefx (config.ini only when missing: it holds your settings).
# Back up your current MissionControl before the first deploy.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${SD:-/Volumes/NS_SAMSUNG}"
MC_TID=010000000000bd00
AT_TID=42000000000d5e00
MC_DST="$SD/atmosphere/contents/$MC_TID"
AT_DST="$SD/atmosphere/contents/$AT_TID"

[ -d "$SD/atmosphere" ] || { echo "SD card not mounted at $SD (set SD=...)"; exit 1; }

sum() { shasum -a 256 "$1" 2>/dev/null | cut -c1-16 || echo none; }

copy_mc() {
    local src="$1"
    [ -f "$src/exefs.nsp" ] || { echo "No MissionControl build at $src"; exit 1; }
    echo "MissionControl: $(sum "$MC_DST/exefs.nsp") -> $(sum "$src/exefs.nsp")"
    mkdir -p "$MC_DST/flags"
    # -X: no macOS extended attributes (avoids ._ files on the FAT card)
    cp -X "$src/exefs.nsp" "$src/mitm.lst" "$src/toolbox.json" "$MC_DST/"
    touch "$MC_DST/flags/boot2.flag"
}

if [ "${1:-}" = "--rollback" ]; then
    BACKUP="${MC_BACKUP:-$(ls -d "$ROOT"/backups/MissionControl-* 2>/dev/null | tail -1)/atmosphere/contents/$MC_TID}"
    copy_mc "$BACKUP"
    rm -rf "$AT_DST"
    echo "adaptive_triggers removed"
else
    copy_mc "$ROOT/MissionControl/dist/atmosphere/contents/$MC_TID"

    AT_SRC="$ROOT/dist/atmosphere/contents/$AT_TID"
    [ -f "$AT_SRC/exefs.nsp" ] || { echo "No adaptive_triggers build at $AT_SRC (run: tools/dk 'make -j8 dist')"; exit 1; }
    echo "adaptive_triggers: $(sum "$AT_DST/exefs.nsp") -> $(sum "$AT_SRC/exefs.nsp")"
    mkdir -p "$AT_DST/flags" "$SD/config/dualsensefx"
    cp -X "$AT_SRC/exefs.nsp" "$AT_SRC/toolbox.json" "$AT_DST/"
    touch "$AT_DST/flags/boot2.flag"
    [ -f "$SD/config/dualsensefx/config.ini" ] || cp -X "$ROOT/sd/config.ini" "$SD/config/dualsensefx/config.ini"
    cp -RX "$ROOT/sd/games" "$SD/config/dualsensefx/"
    if [ -f "$ROOT/overlay/dualsense_fx.ovl" ]; then
        mkdir -p "$SD/switch/.overlays"
        cp -X "$ROOT/overlay/dualsense_fx.ovl" "$SD/switch/.overlays/"
    fi
fi

find "$MC_DST" "$SD/config/dualsensefx" -name '._*' -delete 2>/dev/null || true
[ -d "$AT_DST" ] && find "$AT_DST" -name '._*' -delete
sync
echo "on SD: MissionControl $(sum "$MC_DST/exefs.nsp"), adaptive_triggers $(sum "$AT_DST/exefs.nsp")"
echo "Done. Eject the card before removing it."
