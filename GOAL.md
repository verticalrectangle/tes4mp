# TES4MP — Project Goals & Status

Multiplayer mod for The Elder Scrolls IV: Oblivion (Steam 1.2.416, original — not Remastered).
Linux-native dedicated server in Lua; Windows DLL client loaded by OBSE 0021, cross-compiled with mingw-w64.

---

## Core Design Principles

- **Plug-and-play client** — drop `TES4MP.dll` + `TES4MP.ini` into `Data/OBSE/Plugins/`, edit the IP, done. No .esp, no Construction Set, no ObScript.
- **Server authoritative, no client trust** — the server owns all character state. Clients apply what the server pushes; they never push stats back.
- **TES3MP-style auth** — username/password prompt on first join, session token for reconnects.

---

## Architecture

```
Oblivion (Wine/Proton)
└── OBSE 0021
    └── TES4MP.dll  (mingw-w64 C++)
        ├── Background recv thread → packet queue
        ├── Background PollLoop   → auth dialogs, applies CHAR_LOAD via RunScriptLine2
        └── F10 hotkey window     → command input dialog

TCP (port 7777, newline-delimited JSON)

Lua dedicated server
├── main.lua          — select loop, packet routing, per-socket state machine
├── modules/
│   ├── auth.lua      — REGISTER / LOGIN / TOKEN_AUTH / CHAR_SELECT / CHAR_CREATE
│   ├── session.lua   — in-game player registry, CHAR_LOAD push, broadcasts
│   ├── quests.lua    — global vs personal quest sync
│   ├── world.lua     — dungeon clears, faction ranks, position, crime/bounty
│   ├── commands.lua  — all slash commands
│   └── rbac.lua      — player < moderator < admin permission checks
├── db/
│   └── store.lua     — LuaSQL/SQLite3, full schema
└── util/
    └── crypto.lua    — PBKDF2-SHA256 via Python3, token generation
```

---

## Feature Status

### Foundation
- [x] OBSE plugin skeleton (OBSEPlugin_Query / OBSEPlugin_Load)
- [x] Winsock TCP client, background recv thread, newline-delimited JSON
- [x] CMakeLists mingw-w64 cross-compile; post-build installs DLL+INI to Data/OBSE/Plugins
- [x] `TES4MP.ini` config (Host, Port, CharName fallback)
- [x] Lua TCP server with non-blocking select loop
- [x] LuaSQL/SQLite3 persistence layer

### Auth & Character Management
- [x] Username/password registration (Win32 modal dialogs, no .esp needed)
- [x] Login with username/password
- [x] Session token auth (reconnect without re-entering password)
- [x] Token persistence to `%APPDATA%\TES4MP\token.txt`
- [x] PBKDF2-SHA256 password hashing (100k iterations, via Python3)
- [x] Rate limiting: 5 failed attempts → 15-minute lockout
- [x] First registered account auto-promoted to admin
- [x] 3 character slots per account
- [x] Character creation (name, race, class, birthsign, gender)
- [x] Full character sheet persistence (21 skills, 8 attributes, vitals, level, gold, position)
- [x] CHAR_LOAD push on login — client applies via player.setlevel / player.setav / coc
- [x] CHARACTER_LIST dialog — shows slot, name, level; player picks slot

### Quest Sync
- [x] Global quests — stage changes broadcast to all connected players
- [x] Personal quests — scoped to party only
- [x] Server-side `/setstage questId stage` admin command
- [x] QUEST_SYNC on login (full quest state pushed to joining player)
- [x] Forward-progress only (server ignores lower-stage updates)
- [x] `quests.json` config — defines which quests are global vs personal

### Admin Commands (all server-side, no client trust)
- [x] `/give <name> <item_id> <count>` — sends GIVE_ITEM; client does player.additem
- [x] `/take <name> <item_id> <count>` — sends TAKE_ITEM; client does player.removeitem
- [x] `/tp <name> <cell>` — sends TELEPORT; client does coc
- [x] `/tphere <name>` — teleport target to your last known position
- [x] `/setlevel <name> <level>` — DB + SET_LEVEL packet
- [x] `/setskill <name> <skill> <value>` — DB + SET_SKILL packet
- [x] `/setattr <name> <attr> <value>` — DB + SET_ATTR packet
- [x] `/addgold <name> <amount>` — DB + ADD_GOLD packet
- [x] `/setbounty <name> <hold> <amount>` — DB + SET_BOUNTY packet
- [x] `/kick <name> [reason]`
- [x] `/ban <name>` / `/unban <name>`
- [x] `/setrank <name> <player|moderator|admin>`
- [x] `/setstage <questId> <stage>` — force quest stage on all clients
- [x] `/syncquests` — re-push full quest state to all clients
- [x] `/audit <name>` — dump recent audit log entries

### Social & Communication
- [x] `/tell <name> <message>` — private message between online players
- [x] `/announce <message>` — server-wide announcement (moderator+)
- [x] `/setpw <oldpw> <newpw>` — password change
- [x] `/players` — list online players with role and cell
- [x] `/who` — show your own character info
- [x] `/help` — list available commands for your role

### Party System
- [x] `/party invite <name>`
- [x] `/party accept` / `/party leave`
- [x] `/party members` — list current party
- [x] Party membership persisted in DB
- [x] Personal quest stages scoped to party broadcast only

### World State
- [x] Dungeon clear tracking (`DUNGEON_CLEAR` from client → recorded, cooldown, broadcast `DUNGEON_CLEARED`)
- [x] Dungeon query on cell enter (`DUNGEON_QUERY` → `DUNGEON_STATUS` response)
- [x] Faction rank sync — forward-progress only, broadcast on change
- [x] Bounty/crime per hold — `CRIME_REPORT` accumulates, `BOUNTY_PAID` zeroes hold
- [x] Position storage (last known cell + x/y/z for /tphere)
- [x] Audit log — writes on suspicious events for anti-cheat review

### RBAC
- [x] Three tiers: player < moderator < admin
- [x] Per-command permission checks
- [x] `/announce` = moderator+; all stat/give/tp commands = admin only

---

## Not Done / Remaining Work

### High Priority
- [x] **End-to-end runtime test** — server + test_client verified: register, login, token re-auth, all quest/admin commands pass
- [x] **In-game message display** — all passive notifications (`/tell`, `/announce`, `DUNGEON_CLEARED`, `PARTY_INVITE`, join/leave) use Oblivion's non-blocking `Message "text"` HUD overlay via `RunScriptLine2`. Party invites tell player to type `/party accept <name>`. Auth flow dialogs remain intentionally blocking (pre-game).

### Medium Priority
- [x] **`/motd set <message>`** — let admins update MOTD at runtime without restarting
- [x] **`/clearflag <name>`** — clear audit flags on a character
- [ ] **Inventory table** (`character_inventory`) — track items in player bags; currently designed in schema notes but not in store.lua. Needed if you ever want CHAR_LOAD to restore inventory too.
- [ ] **`CHAR_LOGOUT` final position save** — on clean disconnect, save player's current position. Currently position is only updated by `POSITION_UPDATE` packets, which the client doesn't send yet.
- [ ] **KDE `.desktop` launchers** — `TES4MP Server.desktop` and `TES4MP CS.desktop` (Construction Set via Proton) shortcuts on the desktop

### Low Priority / Deferred
- [ ] **`validator.lua`** — anti-cheat module. Planned to validate CHAR_SAVE deltas (skill gains, gold delta, level-ups per session). Deferred because the client currently sends no stats back; all stat changes are admin-driven. Only relevant if client-side reporting is added.
- [ ] **Client-side event reporting** — `POSITION_UPDATE`, `CRIME_REPORT`, `BOUNTY_PAID`, `FACTION_RANK`, `DUNGEON_CLEAR` all require the client to *read* game state (current cell, bounty values, faction ranks). The DLL currently can't read game memory without either ObScript hooks (needs .esp, breaks plug-and-play) or reverse-engineering memory offsets (complex, version-specific). This is the fundamental trade-off of the no-.esp design.
- [ ] **Periodic CHAR_SAVE** — client pushes full stat snapshot every 5 minutes so the server can persist mid-session changes. Blocked by the same issue as above — client can't read its own stats without memory scanning or ObScript.

---

## Test Client Usage

```bash
# First time — register a new account + character
lua test_client.lua register 127.0.0.1 7777 myuser mypassword "My Hero"

# Subsequent logins — pick slot 1
lua test_client.lua login 127.0.0.1 7777 myuser mypassword 1

# Fast reconnect with saved session token
lua test_client.lua token 127.0.0.1 7777 <token>
```

## Build

```bash
cd client/plugin/build && cmake --build .
# → installs TES4MP.dll + TES4MP.ini to Data/OBSE/Plugins/

lua server/main.lua
# → starts server on port 7777
```
