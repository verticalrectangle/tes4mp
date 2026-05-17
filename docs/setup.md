# TES4MP Setup Guide

## Prerequisites

| Tool | Install |
|---|---|
| mingw-w64 | `sudo pacman -S mingw-w64-gcc` |
| CMake | `sudo pacman -S cmake` |
| Lua 5.4 | `sudo pacman -S lua` |
| LuaRocks | `sudo pacman -S luarocks` |
| Lua deps | install separately — luarocks only accepts one package at a time:<br>`sudo luarocks install luasocket`<br>`sudo luarocks install luasql-sqlite3`<br>`sudo luarocks install lua-cjson` |
| Oblivion Construction Set | Run via Wine/Proton (comes with Oblivion GOTY) |

---

## 1. Install OBSE

1. Download OBSE from https://obse.silverlock.org/ (version 0021 or later)
2. Extract into your Oblivion directory:
   ```
   ~/.local/share/Steam/steamapps/common/Oblivion/
   ├── obse_loader.exe
   ├── obse_steam_loader.dll   ← rename/copy here for Steam
   └── Data/
       └── OBSE/
           └── Plugins/        ← TES4MP.dll goes here
   ```
3. For Steam: copy `obse_steam_loader.dll` to the Oblivion root. Steam will load it automatically.

---

## 2. Build the OBSE Plugin (C++ DLL)

```bash
cd client/plugin
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../../../cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The post-build step automatically copies `TES4MP.dll` to:
```
~/.local/share/Steam/steamapps/common/Oblivion/Data/OBSE/Plugins/TES4MP.dll
```

---

## 3. Create the .esp Mod

You need the **Oblivion Construction Set** to compile the ObScript files into a `.esp`.

1. Launch the CS via Wine/Proton
2. Load `Oblivion.esm` as the active file
3. Create two new **Quest** records:
   - `TES4MPMainQuest` — attach script `TES4MPMain`
   - `TES4MPQuestsQuest` — attach script `TES4MPQuests`
   - Both quests: set **Start Game Enabled = Yes**, **Priority = 100**
4. Create a **Misc Item** record named `TES4MP Terminal`:
   - Attach script `TES4MPMenu`
   - Give it to the player via a startup script or place one in the world
5. Copy the script text from `client/esp/scripts/*.txt` into the CS script editor for each script
6. Save as `TES4MP.esp` into `Data/`

---

## 4. Configure the Server

Edit `server/config.json`:
```json
{
  "host": "0.0.0.0",
  "port": 7777,
  "server_name": "My TES4MP Server",
  "max_players": 10,
  "motd": "Welcome!"
}
```

Edit `server/data/quests.json` to add/remove which quests sync globally.

---

## 5. Run the Server

```bash
cd /home/alexis/dev/TES4MP
lua server/main.lua
```

Server listens on port 7777 by default. The first player to connect is automatically promoted to **admin**.

---

## 6. Configure the Client

In `TES4MPMain.txt` (before compiling in CS), set:
```
let sServerHost := sv_Construct "YOUR_SERVER_IP"
set iServerPort to 7777
```

For local testing use `127.0.0.1`.

---

## 7. Load Order

In `Data/` your load order should be:
```
Oblivion.esm
[DLC .esps]
TES4MP.esp   ← last
```

---

## Packet Type Reference (for debugging)

| Int | Type | strField | intField |
|---|---|---|---|
| 1 | AUTH_OK | role | playerId |
| 2 | AUTH_FAIL | reason | — |
| 3 | QUEST_STAGE | questId | stage |
| 4 | QUEST_SYNC | — | — |
| 5 | PLAYER_LIST | — | — |
| 6 | PLAYER_JOIN | name | — |
| 7 | PLAYER_LEAVE | name | — |
| 8 | COMMAND_RESULT | text | — |
| 9 | MESSAGE | text | — |
| 10 | KICK | reason | — |

---

## Commands

| Command | Role | Description |
|---|---|---|
| `/help` | player | List commands |
| `/players` | player | Show online players |
| `/who` | player | Show your role |
| `/kick <name>` | moderator | Kick a player |
| `/setstage <questId> <stage>` | moderator | Force quest stage for all |
| `/ban <name> [reason]` | admin | Ban a player |
| `/unban <name>` | admin | Unban a player |
| `/setrank <name> <role>` | admin | Set player role |
| `/syncquests` | admin | Force full quest sync to all |

---

## Troubleshooting

**Plugin not loading**: Check that OBSE is installed correctly and `obse_steam_loader.dll` is in the Oblivion root. Look for OBSE logs in `Data\OBSE\Logs\`.

**Connection refused**: Make sure the server is running (`lua server/main.lua`) and the IP/port in TES4MPMain matches.

**Quest stages not syncing**: Verify the quest editor IDs in `TES4MPQuests.txt` match the actual IDs in your game. Open the CS and check the quest editor IDs under `Gameplay > Quests`.
