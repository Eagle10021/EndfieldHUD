#!/usr/bin/env bash
# Adds EndfieldHUD to the application menu, running from this build directory.
#   install.sh              menu entry
#   install.sh --autostart  menu entry, and start at login
#   install.sh --remove     removes both
set -euo pipefail

linux_dir="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
program="$linux_dir/build/endfieldhud"
name=org.endfieldhud.linux.desktop
menu="${XDG_DATA_HOME:-$HOME/.local/share}/applications/$name"
autostart="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/$name"

case "${1:-}" in
    --remove) rm -f "$menu" "$autostart"; echo "Removed."; exit 0 ;;
    ""|--autostart) ;;
    *) echo "usage: install.sh [--autostart|--remove]" >&2; exit 2 ;;
esac
[ -x "$program" ] || { echo "Build first: $program is missing." >&2; exit 1; }

entry() {
    cat <<ENTRY
[Desktop Entry]
Type=Application
Name=EndfieldHUD
Comment=Full-screen HUD, opened with Ctrl+Tab
Exec="$program"
Icon=utilities-system-monitor
Categories=Utility;
Terminal=false
ENTRY
}
mkdir -p "$(dirname "$menu")"
entry > "$menu"
echo "Menu entry: $menu"
if [ "${1:-}" = --autostart ]; then
    mkdir -p "$(dirname "$autostart")"
    entry > "$autostart"
    echo "Starts at login: $autostart"
fi
