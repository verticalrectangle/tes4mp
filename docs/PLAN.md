# TES4MP Sync Expansion Plan

Status: **in progress** (this document is the coordination point — update it as work lands).
Next round (WP11–WP15: full quest/NPC/mob/fight/loot sync) is planned in
**docs/FULL_SYNC_PLAN.md** — WP10 puppet mirroring executes as WP13 there.
Goal of this round: make co-op *feel* real — players see each other properly geared and
moving smoothly, share NPC fights, and can optionally hurt each other — while keeping the
core purpose intact: **playing through quests together** (quest sync is already
server-authoritative and working).

Design constraints (unchanged from GOAL.md):
- No .esp, no Construction Set. Client is a single OBSE plugin DLL.
- Server authoritative where it matters (stats, quests, kills); client-simulated where
  sync cost outweighs value (NPC pathing/AI).
- One binary target: Oblivion 1.2.416. Hardcoded offsets are acceptable and stable.

---

## Current architecture (as of this plan)

```
Client DLL (mingw-w64 C++, OBSE plugin)
├── main.cpp          OBSE entry, 200ms SetTimer game-thread tick
├── game_hooks.cpp    OBSE messages, cmd queue → RunScriptLine2, packet handling (PollLoop thread)
├── network.cpp/.h    Winsock TCP, recv thread, PacketType dispatch
├── pos_sync.cpp      10ms poll thread: player pos/rot/anim → POSITION_UPDATE
├── ghost_system.cpp  Remote players as restrained/ghosted Imperial Watch copies
│                     (PlaceAtMe + cell-scan claim; interp ring buffer; Present-hook tick)
├── npc_sync.cpp      NPC kill detection + container loot scan (cell ref-list walks)
├── appearance.cpp    TESNPC race/hair/eyes/facegeo read → APPEARANCE packet
└── d3d_hook.cpp      d3d9 Present vtable hook (per-frame ghost updates)

Server (Lua 5.x + LuaSocket + cjson + LuaSQL/SQLite)
├── main.lua          select loop, newline-JSON packets, dispatch table
├── modules/world.lua position/cell tracking, ghost appear/leave, NPC kills, loot, weather
├── modules/session.lua  online registry, cell broadcast helpers, CHAR_LOAD push
├── modules/auth.lua  token-only auth (HELLO → CHAR_LOAD)
└── db/store.lua      SQLite schema + queries

Tests: server/tests/integration_test.py — real Lua server + mock TCP clients (19 tests).
```

Cell identity: interiors use cell formID as string; exteriors bucket into 3×3-cell zones
(`E<worldspace>:<gx>:<gy>`, zone = 12288 units). Client (`game_hooks.cpp` cellKey logic)
and server (`world.lua cellKey()`) must stay in lock-step.

---

## Work packages

### WP1 — Join robustness (client) — SUPERSEDED BY WP7 (implemented)

Original scope (gold-dup fix, PostLoadGame path) landed, then WP7 replaced the whole
join-trigger design. Kept for history:
- Gold applied only when `is_new` (was re-added every join → duplication).
- One token = one server character; loading a save with a different name renames the
  server character via CHAR_SAVE (rename now skipped if the name is taken — UNIQUE).

### WP7 — Connection & new-game flow (implemented, needs playtest)

The old design waited for OBSE messages (`PostLoadGame`, or first `SaveGame` of a new
game — the "first autosave" dependency). Replaced with three layers in
`game_hooks.cpp`:

1. **Readiness poll** (GameHooks_Tick): once `InWorld()` (player ptr + parentCell),
   install the D3D Present hook (fixes a latent bug: pure new-game sessions never
   fired PostLoadGame, so ghosts never rendered) and auto-connect every 15s while
   idle. Only the first silent failure prints a HUD message.
2. **F10 = manual connect/reconnect** any time in-world (edge-triggered
   GetAsyncKeyState; no dialog needed).
3. **Server-driven chargen**: a new game connects immediately (name still empty →
   HELLO uses token-derived placeholder `Adventurer-XXXX`; CHAR_SAVE renames later).
   On `is_new` CHAR_LOAD with no player name, a state machine takes over:
   wait for the vanilla race menu to close → `coc` to server start cell + setstage →
   chain `ShowClassMenu` / `ShowBirthsignMenu` / `ShowNameMenu` (plus `ShowRaceMenu`
   first if the vanilla one never appeared), advancing on the engine's
   `IsMenuMode()` at **0x00578F60** (verified against xOBSE GameAPI.cpp for 1.2.416)
   → upload initial stats. 10-minute failsafe ends the chain unconditionally.
   Joining *after* chargen (name already set) keeps the legacy immediate-teleport path.

**Playtest needed:** new game → immediate connect → menu chain → correct stats upload;
old save join; F10 reconnect after server restart; two simultaneous pre-chargen joins
(placeholder name uniqueness).

### WP2 — Deterministic ghost spawning + movement smoothing (client)

**Problem.** Spawn = `PlaceAtMe` then scan the cell for the newest unclaimed guard ref
after 500ms. Two simultaneous GHOST_APPEARs can race; a guard placed by anything else
can be mis-claimed. Movement: linear interp with no extrapolation, and **rotZ lerps
across the ±π wrap** (ghost visibly spins the long way round).

**Changes** (`ghost_system.cpp`):
- Serialize spawning: a single global in-flight spawn. Queue `Phase::Spawning` ghosts;
  only one PlaceAtMe outstanding at a time; next spawn starts after the previous claim
  completes (or times out). Removes the claim race entirely without an ESP.
- Rotation lerp via shortest-arc (wrap delta into [-π, π]).
- Extrapolate up to 250ms using last-two-snapshot velocity when the interp buffer
  underruns (hides network jitter); snap (no interp) when target is >512 units away.
- On spawn and on HP change, `setav health` on the ghost ref so its actual actor HP
  mirrors the remote player (needed by WP5 PvP; also makes kill-cam/etc. coherent).

### WP3 — Equipment sync (client `equip_sync.cpp` — new; server)

Remote players should look like what they wear. Inventory itself stays local-only.

- **Read:** every 2s walk the local player's `ExtraContainerChanges` entries
  (`ref+0x048` extra list; entry = `{EntryExtendData* extendData, SInt32 countDelta,
  TESForm* type}` — layout confirmed against OBSE SDK `GameExtraData.h`). An item is
  worn iff some `extendData` node's `ExtraDataList` contains extra type
  `kExtraData_Worn = 0x1B` (or `0x1C` WornLeft). Collect worn base formIDs.
- **Filter:** only sync formIDs with mod index 00 (Oblivion.esm) — other clients may
  not have the same load order for DLC/mods.
- **Send:** on change → `{"type":"EQUIP_UPDATE","items":[<u32>,...]}` (≤ 20 items).
- **Server:** cache per-session + persist like appearance (`character_equipment` blob);
  broadcast `EQUIP_SYNC {char_id, items}` to cell; include `equipment` array in
  GHOST_APPEAR.
- **Apply:** for each ghost, on EQUIP_SYNC / appear:
  `prid <ref> ; removeallitems ; additem <hex> 1 ; equipitem <hex>` per item.
  (Guard base has its own armor — removeallitems first, then dress.)

### WP4 — NPC HP authority sync (client + server)

Kills already sync. This makes *fights in progress* shared. NPC **positions are
explicitly out of scope** — each client runs its own AI; HP is the state that must
agree, and kill-sync ends every fight identically.

**Authority model:** the server assigns the first player in a cell as that cell's
authority (`CELL_AUTHORITY {authority: true|false}` sent on every cell join/handover;
handover to the longest-present member when the authority leaves or disconnects).

- Authority client: every 1s, batch-sample HP of NPC/creature refs in the cell
  (existing `WalkCellRefs` + `GetActorHp`) → `NPC_HP {cell, npcs:[{ref,hp},...]}` —
  only entries that changed since last send.
- Server: relays `NPC_HP` to other cell members (validates sender is the authority).
- Non-authority client: applies via `prid <ref> ; setav health <hp>`. Before applying,
  compares its own local HP for that ref with the last authority value: if the local
  value is *lower* (this player hit the NPC), send
  `NPC_DAMAGE {cell, ref_id, amount}` instead of silently losing the damage.
- Server routes NPC_DAMAGE to the authority client, which applies
  `prid <ref> ; damageav health <amount>` — the merged result comes back on the next
  NPC_HP tick. Death then flows through the existing NPC_KILLED path on the authority.

This is eventually-consistent (≤ ~1s skew) and idempotent — good enough for co-op.

### WP5 — Basic PvP combat (client + server, config-gated)

- `config.json`: `"pvp": false` default. Sent to clients inside CHAR_LOAD (`pvp` bool).
- PvP off (co-op default): ghosts stay `setghost 1` (untargetable) — nothing changes.
- PvP on: ghosts spawn with `setghost 0` + `setrestrained 1` (hittable holograms).
  Client polls each active ghost's actor HP per second (direct `GetActorHp`, no console
  round-trip); a drop against the server-known value means *someone here hit them* →
  `PLAYER_HIT {target_char_id, amount}`.
- Server: if pvp enabled and both in same cell → `DAMAGE_TAKEN {amount, from}` to the
  target, clamped (≤ 100/packet, rate-limited later if needed).
- Target client: `player.damageav health <amount>` + HUD message. Death then flows
  through the existing PLAYER_DIED path.

Known coarseness (documented, acceptable for "basic"): NPCs can also hit a non-ghosted
ghost, which reports as PvP damage from whoever's client observed it. Server-side pvp
gate keeps this a non-issue for co-op; a later round can filter by "local player in
combat & in range".

### WP6 — Protocol & test coverage (server)

New packets (all newline-JSON like everything else):

| Packet | Direction | Payload | Notes |
|---|---|---|---|
| `EQUIP_UPDATE` | C→S | `items:[u32]` | cached + persisted per char |
| `EQUIP_SYNC` | S→C | `char_id, items` | cell broadcast; also embedded in GHOST_APPEAR as `equipment` |
| `CELL_AUTHORITY` | S→C | `cell, authority:bool` | sent on join/handover |
| `NPC_HP` | C→S→C | `cell, npcs:[{ref,hp}]` | authority-only inbound; relayed to cell |
| `NPC_DAMAGE` | C→S→C | `cell, ref_id, amount` | non-authority → routed to authority only |
| `PLAYER_HIT` | C→S | `target_char_id, amount` | dropped unless config.pvp |
| `DAMAGE_TAKEN` | S→C | `amount, from` | target applies damageav |

Client `PacketType` enum continues from 40.

Integration tests to add (`server/tests/integration_test.py`):
1. equip: EQUIP_UPDATE → peer in cell gets EQUIP_SYNC; late joiner sees `equipment` in GHOST_APPEAR.
2. authority: first-in-cell gets `authority:true`, second `false`; leaver → handover packet to survivor.
3. npc_hp: authority's NPC_HP relayed to peer; non-authority's NPC_HP **ignored**.
4. npc_damage: non-authority NPC_DAMAGE arrives only at authority.
5. pvp: PLAYER_HIT → DAMAGE_TAKEN when `pvp:true` config; dropped when false (needs a second test config or config override).

---

### WP8 — Quest auto-sync (implemented, needs playtest)

Quest sync was server-complete but one-directional: nothing on the client detected
local quest progress (the only sender was an ObScript function requiring an .esp).
Closed DLL-only in `quest_sync.cpp`:

- Server includes its `monitored` quest list (quests.json) in CHAR_LOAD.
- Client resolves each editor ID to a `TESQuest*` by walking the DataHandler
  (`g_dataHandler` at **0x00B33A98**, xOBSE GameAPI.cpp) quest list at +0x084 and
  matching `editorName` (+0x060) case-insensitively — the same walk as xOBSE's
  `DataHandler::GetQuestByEditorName`.
- Every 5s it reads `TESQuest::stageIndex` (+0x05C — the engine-maintained value
  GetStage returns) and sends `QUEST_STAGE` on change. First poll after connect
  baselines and uploads non-zero local progress once (server is forward-only, so
  equal/stale stages are ignored; the setstage echo from applying QUEST_SYNC is
  likewise harmless). Local regressions (console tinkering) are not sent.
- Scoping/persistence/broadcast unchanged: MQ* global to everyone, guild lines
  party-scoped, admins keep /setstage.

**Playtest needed:** advance MQ on client A → B gets the stage; guild quest stays
party-only; offline progress uploads on next join.

### WP9 — Dynamic spawn mirroring, v1 "shared fate" (in progress)

**Problem.** Leveled/dynamic spawns (most dungeon creatures) are rolled per
client with client-local FF refIDs: no shared identity, so players fight
different creatures or don't see each other's at all. Static refs (00 index)
already sync by refID.

**Model (option B — local AI, shared fate):**
- The cell authority enumerates its live dynamic actors (~2s) and broadcasts
  `NPC_SPAWNS {cell, spawns:[{sid, base, x, y, z, hp}]}` — a full snapshot,
  `sid` = the authority's local refID (opaque shared key), `base` = rolled
  creature base formID (vanilla index only).
- Followers **suppress** their own rolled dynamic actors (disable, once) and
  host **replicas**: one spawn per new sid, placed at the authority's coords,
  then driven by the follower's OWN AI (real combat feel; positions may
  drift between screens — accepted in v1).
- Fate flows authority→follower: snapshot hp applied to replicas (setav);
  sid disappearing or hp<=0 kills/removes the replica. v1 does NOT merge
  follower damage on replicas back (documented gap; needs sid-keyed
  NPC_DAMAGE and a replica hp poll).
- Authority handover: new authority has different refIDs → its first
  snapshot replaces everything (followers wipe and rebuild replicas).

**Spawn primitive** (the hard part — hex formID literals with A–F digits
don't parse in the RunScriptLine path):
1. Multi-line temp script: `ref r / let r := GetFormFromMod "Oblivion.esm"
   <decimal id> / player.PlaceAtMe r 1` (OBSE expression compiler).
2. Fallback: `player.PlaceAtMe %08X 1` — works only when the hex happens to
   be all decimal digits (how ghosts spawn today, base 00096765).
3. Give up + log. Each attempt is verified by the claim scan (newest
   unclaimed FF ref with the right base), so tes4mp_debug.txt records which
   strategy actually works in-game — check it after the first playtest.

**Server:** relay NPC_SPAWNS from the cell authority only (mirrors NPC_HP
validation); integration test for relay + non-authority rejection.

**Also:** stage-parity — world content is enabled/disabled by quest stage, so
stage divergence makes spawns invisible to one player (the "only I saw the
rat" bug had this as a co-cause). QUEST_SYNC is re-sent on every cell
transition, not just login.

### WP10 — Full puppet NPC mirroring (option A) — PLAN ONLY, for future agents

Upgrade path from WP9; do not start until WP9 is verified in-game.

**Goal.** One shared reality: every synced NPC is at the same position doing
the same thing on all screens, like player ghosts.

**Architecture (A = WP9 + position streaming + AI freeze):**
1. Authority streams `NPC_POS {cell, npcs:[{sid, x, y, z, rot, anim}]}` at
   2–5 Hz for dynamic replicas AND static actors in combat. Reuse the ghost
   snapshot ring + interpolation (extract ghost interp into a shared
   helper — do not fork the code).
2. Followers put mirrored actors in puppet mode: `setrestrained 1` (their
   AI off) and drive positions via the ghost WriteRef path per frame.
   Puppets must NOT be `setghost` — players need to hit them.
3. Follower hits on puppets: poll actor hp like GhostSystem_PollHits, send
   sid-keyed NPC_DAMAGE; authority applies (its actor is live AI), result
   returns via NPC_POS/NPC_HP.
4. Combat targeting: puppets never attack followers (restrained). Mitigate:
   authority also streams target info (`tsid`/char_id) so followers can play
   attack anims via PlayGroup toward the right victim; real threat for
   followers comes only when they hold authority. This is A's fundamental
   compromise — document it in the server MOTD/README.
5. Bandwidth: cap streamed actors (nearest N=8 to any player), send deltas
   only, drop to 1 Hz when no player within ~2000 units.
6. Handover: identical to WP9 (snapshot replace), plus followers must exit
   puppet mode (`setrestrained 0`) for actors the new authority doesn't
   stream.

**Prerequisites discovered the hard way (read before coding):**
- No hex-with-letters literals in RunScriptLine commands; refs go via
  RunScriptLine2 callingRefr (see WP7/WP9), forms via GetFormFromMod.
- All memory walks gated on GameHooks_IsSafeToScan(); teleports raise a
  transition guard. Never gate the command queue on more than menu-mode.
- Present-hook code must not call engine functions (cache on the tick).
- Verify every engine offset against the local OBSE SDK headers or xOBSE
  sources; 0x1C34F-style "known" constants have been wrong before.

**Definition of done:** two clients watch the same bandit pace the same
patrol; either player can kill it; the corpse lies in the same spot for both.

## Explicitly out of scope (this round)

- **NPC position/AI mirroring** — the deep rabbit hole; kill+HP sync gives shared
  outcomes without it.
- **FaceGen replication** — appearance.cpp already ships geo/tex floats; applying them
  to ghost base forms needs an in-memory regen call that must be found by
  reverse-engineering (`BSFaceGen*` — future work).
- **Animation graph sync** — PlayGroup buckets stay (idle/walk/run/turn/jump).
- **ESP of any kind** — spawn determinism is solved by serializing spawns instead.
- **Inventory/loot duplication rules between players** — trade is a future feature.

## Verification reality

Everything server-side is covered by the integration suite (real server, mock clients).
The client DLL cross-compiles here but **cannot be executed here** — memory-offset code
paths (equip walk, ghost HP poll, setghost toggling) need a live Oblivion playtest.
Anything untested in-game is marked `PLAYTEST` in commit messages and below:

- PLAYTEST: WP1 join from old save (gold, stats, position)
- PLAYTEST: WP2 two ghosts appearing simultaneously; rotation smoothness
- PLAYTEST: WP3 worn-item walk offsets (worst case: garbage formIDs → filtered by
  mod-index-00 check, fails safe to "default guard outfit")
- PLAYTEST: WP4 two clients fighting the same bandit camp
- PLAYTEST: WP5 pvp duel with `"pvp": true`
