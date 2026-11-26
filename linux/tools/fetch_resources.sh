#!/usr/bin/env bash
# Downloads the official Windows portable release, checks its SHA-256 and unpacks it, so the Linux
# build can read its resources. The game artwork inside is not covered by the project's MIT license
# (see CREDITS.md) and stays outside the repository.
set -euo pipefail

release=1.3.1
archive="EndfieldHUD-1.3.1-build21-windows-x64-portable.zip"
sha256=7329cb752ce6d06b8f6b5450713edf1393902ec404ba54079d0c1809c8eaa628
url="https://github.com/DDDuoDuo/EndfieldHUD/releases/download/v$release/$archive"

base="${XDG_DATA_HOME:-$HOME/.local/share}/endfieldhud-linux"
target="$base/release-1.3.1-build21"

if [ ! -d "$target/package" ]; then
    mkdir -p "$target/download"
    [ -f "$target/download/$archive" ] || curl --fail --location --output "$target/download/$archive" "$url"
    echo "$sha256  $target/download/$archive" | sha256sum --check
    # Unpacked into a new directory, renamed only once complete.
    rm -rf "$target/unpacking"
    mkdir "$target/unpacking"
    bsdtar --extract --file "$target/download/$archive" --directory "$target/unpacking"
    mv "$target/unpacking" "$target/package"
fi
ln -sfn "$target/package/EndfieldHUD/resources" "$base/resources"
echo "Resources ready: $base/resources"
