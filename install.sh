#!/usr/bin/env bash
set -euo pipefail

# ── TES4MP Client Installer ───────────────────────────────────────────────────
# Installs OBSE 0021 (if missing) and TES4MP into the Oblivion directory.
#
# Usage:
#   ./install.sh                         # auto-detect Oblivion path
#   ./install.sh /path/to/Oblivion       # explicit path

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="$SCRIPT_DIR/client/plugin"
# Prefer pre-built DLL in dist/; fall back to build output
if [[ -f "$SCRIPT_DIR/dist/TES4MP.dll" ]]; then
    DLL="$SCRIPT_DIR/dist/TES4MP.dll"
else
    DLL="$PLUGIN_DIR/build/TES4MP.dll"
fi
INI="$PLUGIN_DIR/TES4MP.ini"

OBSE_URL="https://obse.silverlock.org/download/obse_0021.zip"
OBSE_ZIP="/tmp/obse_0021.zip"

# ── Colour helpers ────────────────────────────────────────────────────────────
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
bold()   { printf '\033[1m%s\033[0m\n'  "$*"; }

# ── Locate Oblivion ───────────────────────────────────────────────────────────
find_oblivion() {
    local candidates=(
        "$HOME/.local/share/Steam/steamapps/common/Oblivion"
        "$HOME/.steam/steam/steamapps/common/Oblivion"
        "/mnt/c/Program Files (x86)/Steam/steamapps/common/Oblivion"
        "C:/Program Files (x86)/Steam/steamapps/common/Oblivion"
    )
    for p in "${candidates[@]}"; do
        [[ -f "$p/Oblivion.exe" ]] && echo "$p" && return 0
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

# ── Install OBSE if missing ───────────────────────────────────────────────────
if [[ ! -f "$OBLIVION_DIR/obse_1_2_416.dll" ]]; then
    yellow "OBSE not detected — downloading OBSE 0021..."

    if command -v curl &>/dev/null; then
        curl -fL "$OBSE_URL" -o "$OBSE_ZIP"
    elif command -v wget &>/dev/null; then
        wget -q "$OBSE_URL" -O "$OBSE_ZIP"
    else
        red "Neither curl nor wget found. Install one and retry, or manually install OBSE from:"
        echo "  https://obse.silverlock.org/"
        exit 1
    fi

    if ! command -v unzip &>/dev/null; then
        red "unzip not found. Install it and retry."
        exit 1
    fi

    OBSE_TMP="$(mktemp -d)"
    unzip -q "$OBSE_ZIP" -d "$OBSE_TMP"
    rm -f "$OBSE_ZIP"

    # The zip contains a single subdirectory — find it
    OBSE_SRC="$(find "$OBSE_TMP" -maxdepth 2 -name "obse_1_2_416.dll" | head -1 | xargs dirname)"
    if [[ -z "$OBSE_SRC" ]]; then
        red "Could not find OBSE files in downloaded archive."
        exit 1
    fi

    for f in obse_1_2_416.dll obse_editor_1_2.dll obse_loader.exe obse_steam_loader.dll; do
        [[ -f "$OBSE_SRC/$f" ]] && cp "$OBSE_SRC/$f" "$OBLIVION_DIR/$f"
    done
    rm -rf "$OBSE_TMP"

    green "OBSE installed."
else
    green "OBSE already installed — skipping."
fi

# ── Check DLL exists ──────────────────────────────────────────────────────────
if [[ ! -f "$DLL" ]]; then
    red "TES4MP.dll not found. Build it first:"
    echo "  cd client/plugin && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/mingw-toolchain.cmake && cmake --build build"
    exit 1
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
        yellow "obse_loader.exe not found — skipping patches"
    else
        PATCH_RESULT="$(python3 - "$LOADER" <<'PYEOF'
import sys, shutil, os

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())

# Pattern immediately before the Steam ownership JNZ:
#   b0 01          MOV AL, 1
#   88 44 24 13    MOV [ESP+13h], AL
#   75 ??          JNZ <fail>   ← we NOP this
pat = bytes([0xb0, 0x01, 0x88, 0x44, 0x24, 0x13])
idx = data.find(pat)
if idx == -1:
    print("NOTFOUND")
    sys.exit(0)

jnz_off = idx + len(pat)
if data[jnz_off] == 0x90:
    print("ALREADY")
    sys.exit(0)

if data[jnz_off] != 0x75:
    print(f"UNEXPECTED {data[jnz_off]:02x} at {jnz_off:#x}")
    sys.exit(0)

shutil.copy2(path, path + ".bak")
data[jnz_off]     = 0x90  # NOP the JNZ opcode
data[jnz_off + 1] = 0x90  # NOP the offset byte
open(path, 'wb').write(data)
print(f"PATCHED {jnz_off:#x}")
PYEOF
)"
        case "$PATCH_RESULT" in
            PATCHED*)  green "  obse_loader.exe patched at ${PATCH_RESULT#PATCHED } (Steam check NOP'd)" ;;
            ALREADY)   green "  obse_loader.exe already patched — skipping" ;;
            NOTFOUND)  yellow "  obse_loader.exe: Steam check pattern not found — skipping patch" ;;
            *)         yellow "  obse_loader.exe: $PATCH_RESULT — skipping patch" ;;
        esac
    fi

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
echo "  2. Launch Oblivion through Steam — the mod connects automatically on load"
