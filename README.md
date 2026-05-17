# TES4MP — Oblivion Multiplayer

Adds basic multiplayer to The Elder Scrolls IV: Oblivion (v1.2.416). Players share the same world: position sync, cell transitions, character stats, dungeon clears, and faction ranks.

> Early development — expect rough edges.

---

## Requirements

**To play (client)**
- Oblivion 1.2.416 (Steam or retail)
- [OBSE 0021](https://obse.silverlock.org/)

**To host (server)**
- Lua 5.1
- [LuaSocket](https://lunarmodules.github.io/luasocket/)
- [lua-cjson](https://github.com/openresty/lua-cjson)
- [LuaSQLite3](http://lua.sqlite.org/)

**To build the client DLL from source**
- MinGW-w64 (cross-compiler, any platform)
- CMake 3.20+

---

## Quick Install (client)

A prebuilt `TES4MP.dll` is included in `dist/`. The installer handles everything including downloading OBSE.

```bash
./install.sh
# or pass your Oblivion path explicitly:
./install.sh "/path/to/Oblivion"
```

This will:
1. Download and install OBSE 0021 if it isn't already present
2. Copy `TES4MP.dll` into `Data/OBSE/Plugins/`
3. Copy `TES4MP.ini` (only if not already there, so your settings are preserved)
4. On Linux: patch `obse_loader.exe` for Steam compatibility

**Linux (Steam/Proton) — extra step:**  
Set this in Steam → Oblivion → Properties → Launch Options:
```
WINEDLLOVERRIDES="obse_steam_loader=n,b" %command%
```
The installer patches `obse_loader.exe` automatically.

### 3. Configure

Edit `Data/OBSE/Plugins/TES4MP.ini` and set the server address:

```ini
[Network]
Host=your.server.address
Port=7777
```

### 4. Play

Launch Oblivion through Steam. Start a new game or load a save — the mod connects automatically and shows `[TES4MP] Connected as <name>` in the HUD.

Your character is identified by a persistent token stored in `%APPDATA%\TES4MP\token.txt`. No account needed.

---

## Hosting a Server

### Install Lua dependencies

**Debian/Ubuntu:**
```bash
sudo apt install lua5.1 lua-socket lua-cjson
# lua-sqlite3 may need luarocks:
sudo luarocks install lsqlite3
```

**Arch:**
```bash
sudo pacman -S lua51 lua51-socket
sudo luarocks-5.1 install lua-cjson
sudo luarocks-5.1 install lsqlite3
```

### Configure

Edit `server/config.json`:

```json
{
  "host": "0.0.0.0",
  "port": 7777,
  "server_name": "My TES4MP Server",
  "max_players": 10
}
```

### Start

```bash
./launch_server.sh
# or directly:
lua server/main.lua
```

The database is created automatically at `server/data/tes4mp.db` on first run.

---

## Building from Source

```bash
cd client/plugin
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/mingw-toolchain.cmake
cmake --build build
```

The built `TES4MP.dll` is automatically copied to your Oblivion folder if the installer has been run once.

---

## What works

- Token-based auth (no accounts/passwords)
- Character name synced from Oblivion's character creation screen
- Position and rotation sync within cells
- Cell transition events (GHOST_APPEAR / GHOST_LEAVE)
- Ghost NPC spawning at runtime via `PlaceAtMe` — no ESP required
- Per-frame ghost position interpolation via D3D9 Present hook
- Ghost animation sync (idle / walk / run / turn)
- Character stats saved server-side every 15 seconds
- Dungeon clear tracking with cooldowns
- Faction rank sync
- Bounty tracking per hold

## What doesn't work yet

- Appearance sync applied to ghosts (race/hair/face morphs not yet written to the ref)
- Combat sync
