#!/usr/bin/env bash
set -euo pipefail

# ── TES4MP Client Installer ───────────────────────────────────────────────────
# Copies TES4MP.dll + TES4MP.ini into the Oblivion OBSE plugin directory and,
# on Linux, applies the two binary patches needed for OBSE to load under Steam.
#
# Usage:
#   ./install.sh                         # auto-detect Oblivion path
#   ./install.sh /path/to/Oblivion       # explicit path

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$SCRIPT_DIR/client/plugin"
DLL="$PLUGIN_DIR/build/TES4MP.dll"
INI="$PLUGIN_DIR/TES4MP.ini"

# ── Colour helpers ────────────────────────────────────────────────────────────
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
bold()   { printf '\033[1m%s\033[0m\n'  "$*"; }

# ── Locate Oblivion ───────────────────────────────────────────────────────────
find_oblivion() {
    # Common Steam library locations
    local candidates=(
        "$HOME/.local/share/Steam/steamapps/common/Oblivion"
        "$HOME/.steam/steam/steamapps/common/Oblivion"
        "/mnt/c/Program Files (x86)/Steam/steamapps/common/Oblivion"
        "C:/Program Files (x86)/Steam/steamapps/common/Oblivion"
    )
    for p in "${candidates[@]}"; do
        if [[ -f "$p/Oblivion.exe" ]]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

if [[ $# -ge 1 ]]; then
    OBLIVION_DIR="$1"
else
    if ! OBLIVION_DIR="$(find_oblivion)"; then
        red "Could not auto-detect Oblivion installation."
        echo "Pass the path explicitly:  ./install.sh /path/to/Oblivion"
        exit 1
    fi
fi

if [[ ! -f "$OBLIVION_DIR/Oblivion.exe" ]]; then
    red "Oblivion.exe not found in: $OBLIVION_DIR"
    exit 1
fi

bold "TES4MP Installer"
echo "Oblivion: $OBLIVION_DIR"
echo

# ── Check DLL is built ────────────────────────────────────────────────────────
if [[ ! -f "$DLL" ]]; then
    red "TES4MP.dll not found.  Build it first:"
    echo "  cd client/plugin && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/mingw-toolchain.cmake && cmake --build build"
    exit 1
fi

# ── Check OBSE is installed ───────────────────────────────────────────────────
if [[ ! -f "$OBLIVION_DIR/obse_1_2_416.dll" ]]; then
    yellow "OBSE not detected in Oblivion folder."
    echo "Download OBSE 0021 from https://obse.silverlock.org/ and copy these files"
    echo "into $OBLIVION_DIR :"
    echo "  obse_1_2_416.dll"
    echo "  obse_editor_1_2.dll"
    echo "  obse_loader.exe"
    echo "  obse_steam_loader.dll"
    echo
    read -rp "Continue anyway? [y/N] " yn
    [[ "$yn" =~ ^[Yy]$ ]] || exit 1
fi

# ── Install plugin files ──────────────────────────────────────────────────────
PLUGIN_DEST="$OBLIVION_DIR/Data/OBSE/Plugins"
mkdir -p "$PLUGIN_DEST"

echo "Copying TES4MP.dll → $PLUGIN_DEST/"
cp "$DLL" "$PLUGIN_DEST/TES4MP.dll"

if [[ -f "$PLUGIN_DEST/TES4MP.ini" ]]; then
    echo "TES4MP.ini already exists — skipping (preserving your settings)"
else
    echo "Copying TES4MP.ini → $PLUGIN_DEST/"
    cp "$INI" "$PLUGIN_DEST/TES4MP.ini"
fi

# ── Install chat overlay XML ──────────────────────────────────────────────────
XML_SRC="$PLUGIN_DIR/Data/Menus/generic"
XML_DEST="$OBLIVION_DIR/Data/Menus/generic"
if [[ -d "$XML_SRC" ]]; then
    mkdir -p "$XML_DEST"
    for f in "$XML_SRC"/*.xml; do
        [[ -f "$f" ]] || continue
        echo "Copying $(basename "$f") → $XML_DEST/"
        cp "$f" "$XML_DEST/"
    done
fi

# ── Linux: apply OBSE Steam patches ──────────────────────────────────────────
if [[ "$(uname -s)" == "Linux" ]]; then
    echo
    bold "Linux detected — applying OBSE Steam compatibility patches..."

    LOADER="$OBLIVION_DIR/obse_loader.exe"
    LAUNCHER="$OBLIVION_DIR/OblivionLauncher.exe"

    if [[ ! -f "$LOADER" ]]; then
        yellow "obse_loader.exe not found — skipping patches (install OBSE first)"
    else
        # Patch 1: NOP the Steam ownership check in obse_loader.exe at 0x14CB
        LOADER_BYTE="$(dd if="$LOADER" bs=1 skip=$((0x14cb)) count=1 2>/dev/null | xxd -p)"
        if [[ "$LOADER_BYTE" == "75" ]]; then
            cp "$LOADER" "$LOADER.bak"
            printf '\x90\x90\x90' | dd conv=notrunc of="$LOADER" bs=1 seek=$((0x14cb)) 2>/dev/null
            green "  obse_loader.exe patched (Steam check NOP'd)"
        elif [[ "$LOADER_BYTE" == "90" ]]; then
            green "  obse_loader.exe already patched — skipping"
        else
            yellow "  obse_loader.exe: unexpected byte at 0x14CB ($LOADER_BYTE) — skipping patch"
        fi

        # Patch 2: Redirect OblivionLauncher.exe to call obse_loader at 0x1347C
        if [[ ! -f "$LAUNCHER" ]]; then
            yellow "  OblivionLauncher.exe not found — skipping launcher patch"
        else
            LAUNCHER_CHECK="$(dd if="$LAUNCHER" bs=1 skip=$((0x1347c)) count=11 2>/dev/null | xxd -p)"
            if [[ "$LAUNCHER_CHECK" == "6f6273655f6c6f6164657200" ]] || \
               [[ "$LAUNCHER_CHECK" == "6f6273655f6c6f61646572" ]]; then
                green "  OblivionLauncher.exe already patched — skipping"
            else
                cp "$LAUNCHER" "$LAUNCHER.bak"
                printf 'obse_loader\x00' | dd conv=notrunc of="$LAUNCHER" bs=1 seek=$((0x1347c)) 2>/dev/null
                green "  OblivionLauncher.exe patched (launcher → obse_loader)"
            fi
        fi
    fi

    echo
    bold "Steam Launch Options (Linux only):"
    echo "  Right-click Oblivion in Steam → Properties → Launch Options:"
    echo
    echo '  WINEDLLOVERRIDES="obse_steam_loader=n,b" %command%'
    echo
fi

# ── Done ──────────────────────────────────────────────────────────────────────
echo
green "Installation complete!"
echo
echo "Next steps:"
echo "  1. Edit $PLUGIN_DEST/TES4MP.ini and set Host= to your server address"
echo "  2. Launch Oblivion through Steam, load a save — the login dialog will appear"
