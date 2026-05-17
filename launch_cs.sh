#!/usr/bin/env bash
# Launch the Oblivion Construction Set via Proton + OBSE.
# OBSE is required so TES4MP script functions are recognised by the CS compiler.

OBLIVION="$HOME/.local/share/Steam/steamapps/common/Oblivion"
PROTON="$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton"
COMPAT_DATA="$HOME/.local/share/Steam/steamapps/compatdata/22330"

# Generate the .esp skeleton first (idempotent — safe to run every time)
echo "[TES4MP] Generating ESP skeleton..."
python3 "$(dirname "$0")/tools/generate_esp.py"

echo "[TES4MP] Launching Construction Set..."
export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam"

exec "$PROTON" run "$OBLIVION/obse_loader.exe" -editor
