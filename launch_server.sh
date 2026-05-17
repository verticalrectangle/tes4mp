#!/usr/bin/env bash
# Start the TES4MP dedicated server.
# Opens in a Konsole window so you can see live logs and type Ctrl+C to stop.

cd "$(dirname "$0")"

if command -v konsole &>/dev/null; then
    exec konsole --noclose -e bash -c 'echo "[TES4MP] Server starting..."; lua server/main.lua; echo; echo "[TES4MP] Server stopped. Press Enter to close."; read'
elif command -v xterm &>/dev/null; then
    exec xterm -hold -e bash -c 'lua server/main.lua'
else
    exec lua server/main.lua
fi
