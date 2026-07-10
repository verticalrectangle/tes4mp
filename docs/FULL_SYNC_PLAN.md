# Full Sync Plan — quests, NPCs, mobs, fights, loot

Status: **draft — awaiting answers to open questions at the bottom, then execution.**
Companion to docs/PLAN.md (which stays the coordination doc; WP numbering continues).

Goal of this round: close the remaining gaps between "we see each other move"
and "we play the same world":

| Domain | Today | Gap this plan closes |
|---|---|---|
| Quests | Stage sync for a hand-maintained `quests.json` list, forward-only, stage parity on cell entry (WP8) | Monitor **every** quest automatically; no more curated list |
| NPCs (static) | Kill sync + HP authority sync (WP4) | Position/AI mirroring in combat (puppets, WP13) |
| Mobs (dynamic) | WP9 shared-fate replicas (authority snapshot → follower replicas, own AI) | Follower damage merges back (WP11); positions mirrored (WP13) |
| Fights | NPC_DAMAGE merge for statics; PvP config-gated (WP5) | Damage merge for replicas; same fight on both screens |
| Loot | Container removal sync + persisted CONTAINER_STATE | Corpse loot, world (loose) items, gold — full "if you took it, it's gone for me too" |

Design constraints unchanged: no .esp, one DLL, Oblivion 1.2.416 hardcoded
offsets, server-authoritative outcomes, client-simulated feel. All hard-won
rules from PLAN.md WP10 "prerequisites" apply to every WP here:

- No hex-with-letters literals through RunScriptLine; refs execute via
  `GameHooks_EnqueueCmdOnRef` (calling-ref context), forms via `GetFormFromMod`.
- Memory walks only when `GameHooks_IsSafeToScan()`.
- Present-hook code never calls engine functions — cache on the game tick.
- Every new offset gets verified against the local OBSE SDK headers first.

---

## Execution order & build gates

Per playtest policy (one minimal change per build), the WPs land as separate
buildable stages, each independently revertible:

1. **WP11** — replica damage merge (small, closes WP9's known gap)
2. **WP12** — loot completeness: corpse loot, then world items (two builds)
3. **WP13** — puppet mirroring: interp extraction (no behavior change) →
   NPC_POS streaming → puppet mode (three builds)
4. **WP14** — monitor-all-quests (server + client, one build)
5. **WP15** — protocol/tests land with each WP, not at the end

WP9 (spawn mirroring v1) is **untested in-game** as of d4bcd4c. WP11 and WP13
build directly on it. If the spawn primitive (GetFormFromMod temp script)
turns out broken in the first playtest, WP13 still works for **static** NPCs
(refs already shared by refID — no spawning needed), so the order inside WP13
is: statics first, replicas second.

---

## WP11 — Replica damage merge (fight sync for dynamic mobs)

**Problem.** WP9 v1 fate flows one way: authority → follower. A follower
hitting a replica does damage on their screen that silently un-happens on the
next snapshot (`setav health` overwrites it). Documented gap in PLAN.md.

**Model.** Mirror the static-NPC pattern (`NPC_DAMAGE`) but keyed by `sid`
(the authority's local refID — the shared identity), because the follower's
replica refID means nothing to anyone else.

### Client — follower side (`npc_spawn_sync.cpp`)

`Replica` already tracks `lastHp` (last authority-known hp). Poll the live
replica actor each FollowerTick; a local drop below `lastHp` is our damage:

```cpp
// In FollowerTick(), after the snapshot-apply block, before spawn machinery:
for (auto& [sid, r] : g_replicas) {
    if (!r.ref || r.lastHp <= 0) continue;
    int localHp = (int)ActorHp(r.ref);
    if (localHp < r.lastHp - 1) {           // we hit it (tolerance 1 for float noise)
        char buf[112];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"NPC_DAMAGE_SID\",\"cell\":\"%s\",\"sid\":%u,\"amount\":%d}",
            cellKey.c_str(), sid, r.lastHp - localHp);
        g_network.send(buf);
        r.lastHp = localHp;   // don't resend the same delta; snapshot will reconcile
    }
}
```

Snapshot-apply change: replace the current `abs(e.hp - r.lastHp) > 5` rule with
a directional one, so authority-side healing still applies but our own reported
damage isn't bounced back up before the authority has merged it:

```cpp
if (r.ref && e.hp > 0 && r.lastHp > 0 && e.hp != r.lastHp) {
    // Trust the authority value — it now includes merged NPC_DAMAGE_SID.
    char buf[48];
    snprintf(buf, sizeof(buf), "setav health %d", e.hp);
    GameHooks_EnqueueCmdOnRef(r.ref, buf);
}
r.lastHp = e.hp;   // ALWAYS track the authority value, even when not applying
```

### Client — authority side (`npc_sync.cpp` or `npc_spawn_sync.cpp`)

On `NPC_DAMAGE_SID` (routed by server), sid == authority's own local refID:

```cpp
void NpcSpawnSync_OnSidDamage(uint32_t sid, int amount) {
    if (amount <= 0) return;
    void* ref = Oblivion::LookupFormByID(sid);   // sid is OUR refID
    if (!ref) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "damageav health %d", amount);
    GameHooks_EnqueueCmdOnRef(ref, buf);
    // Death propagates via the next NPC_SPAWNS snapshot (hp<=0 → sid dropped
    // → followers kill their replica). No extra packet needed.
}
```

Wire into the network dispatch next to the existing `NPC_DAMAGE` handler.

### Server (`world.lua`)

Clone of `handleNpcDamage` with sid validation (dynamic range):

```lua
function M.handleNpcDamageSid(char_id, pkt)
    local json   = require("cjson")
    local cell   = tostring(pkt.cell or "")
    local sid    = math.floor(tonumber(pkt.sid) or 0)
    local amount = math.floor(tonumber(pkt.amount) or 0)
    if cell == "" or sid <= 0xFF000000 - 1 or amount <= 0 or amount > 1000 then return end
    -- sid must be a dynamic refID (0xFFxxxxxx) — it's the authority's local ref
    if sid < 0xFF000000 then return end

    local sess = session.getByCharId(char_id)
    if not sess or sess.cell ~= cell then return end

    local auth = authorities[cell]
    if not auth or auth == char_id then return end   -- authority damages locally
    session.sendTo(auth, json.encode({
        type = "NPC_DAMAGE_SID", cell = cell, sid = sid, amount = amount }))
end
```

**Tests** (`integration_test.py`): follower NPC_DAMAGE_SID reaches only the
authority; authority-sent packet is dropped; out-of-range sid rejected.

**Definition of done:** follower kills a replica solo → the real mob dies on
the authority's screen within one snapshot period (≤2s).

---

## WP12 — Loot completeness

Loot model everywhere: **shared-world subtractive**. Anything one player takes
is removed for everyone and persisted, exactly like containers today. No
duplication rules, no trading (future work).

### 12a — Corpse loot

Dead actors are containers: looting a corpse writes the same
`ExtraContainerChanges` extra data the container scan already parses.

**Client** (`npc_sync.cpp` `ScanContainerLoots`): accept actor types too, but
only when dead — living-NPC inventory churn (AI equipping) must not spam:

```cpp
uint8_t typeId = *(uint8_t*)((char*)base + Oblivion::kForm_typeID);
bool isContainer = (typeId == Oblivion::kFormType_Container);
bool isCorpse    = (typeId == Oblivion::kFormType_NPC ||
                    typeId == Oblivion::kFormType_Creature)
                   && GetActorHp(ref) <= 0.f;
if (!isContainer && !isCorpse) return true;
// …existing ExtraContainerChanges walk unchanged — it already works on any ref
```

Corpse refs can be dynamic (`0xFFxxxxxx` replicas / local rolls). v1 rule:
**static corpses only** (refId mod-index 00, same filter as today). Replica
corpse loot needs sid translation — deferred to a follow-up (documented gap;
the fun 90% case is bandit camps and dungeon bosses, which are static refs).

**Server**: no change — `handleItemTaken`/`CONTAINER_STATE` already keys by
arbitrary ref. The `ref_id > 0xFF000000` reject in `handleNpcKilled` does NOT
exist in `handleItemTaken`, but add the same static-only guard for symmetry.

**Apply path**: existing `NpcSync_OnItemSync` (`removeitem` on the ref) works
on corpses as-is.

### 12b — World (loose) items

Picking up a loose item (weapon on a table, ingredient, book) doesn't delete
the ref — the engine keeps it flagged. TESObjectREFR flags at **+0x008**
(TESForm::flags, confirmed GameForms.h): bit 0x800 = "harvested/taken" is
unreliable across types; the robust signal for placed items is
**kFlag_Disabled (1 << 11) or the ref vanishing from the cell walk after
activation**. Simplest reliable v1: track item-type refs present in the cell;
when a previously-seen ref stops appearing in the walk (or reads disabled),
it was taken.

```cpp
// npc_sync.cpp — new scan, same 1s cadence, static refs only
static std::unordered_set<uint32_t> g_knownItems;    // reset on cell change
static std::unordered_set<uint32_t> g_takenItems;    // reported/applied

static bool IsLootableItemType(uint8_t t) {
    using namespace Oblivion;
    return t == kFormType_Weapon || t == kFormType_Armor || t == kFormType_Clothing
        || t == kFormType_Misc   || t == kFormType_Ingredient
        || t == kFormType_Book   || t == kFormType_AlchemyItem
        || t == kFormType_SoulGem|| t == kFormType_Key || t == kFormType_Gold;
}

static void ScanWorldItems(void* cell, const std::string& cellKey) {
    std::unordered_set<uint32_t> present;
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        if (refId == 0 || (refId & 0xFF000000) != 0) return true;
        void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
        if (!base) return true;
        if (!IsLootableItemType(*(uint8_t*)((char*)base + Oblivion::kForm_typeID)))
            return true;
        // Disabled = taken-by-peer already applied, or engine-disabled: skip both
        uint32_t flags = *(uint32_t*)((char*)ref + 0x008);
        if (flags & (1u << 11)) return true;
        present.insert(refId);
        return true;
    });

    for (uint32_t refId : g_knownItems) {
        if (!present.count(refId) && !g_takenItems.count(refId)) {
            g_takenItems.insert(refId);
            char buf[96];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"WORLD_ITEM_TAKEN\",\"ref_id\":%u,\"cell\":\"%s\"}",
                refId, cellKey.c_str());
            g_network.send(buf);
        }
    }
    g_knownItems = std::move(present);
}
```

**Warning encoded above:** the first scan after cell entry only *baselines*
(g_knownItems empty → nothing reported), so items taken while nobody else was
in the cell **before the taker connected** aren't caught — acceptable, the
server-persisted state covers everything after first report.

**Apply on peers** (and on cell entry from persisted state): `disable` on the
ref — same `RefOp::KillSilent` path used for kill-sync corpses:

```cpp
void NpcSync_OnWorldItemSync(const uint32_t* refs, int count) {
    for (int i = 0; i < count; ++i) {
        g_takenItems.insert(refs[i]);          // don't re-report what we apply
        QueueRefOp({ RefOp::KillSilent, refs[i], 0, 0 });  // "disable"
    }
}
```

**Server** (`world.lua` + `store.lua`): mirror the killed-refs pattern exactly:

```lua
function M.handleWorldItemTaken(char_id, pkt)
    local json   = require("cjson")
    local ref_id = tonumber(pkt.ref_id) or 0
    local cell   = tostring(pkt.cell or "")
    if ref_id == 0 or cell == "" or ref_id > 0xFF000000 then return end
    local sess = session.getByCharId(char_id)
    if not sess or sess.cell ~= cell then return end

    store.setWorldItemTaken(ref_id, cell, char_id)   -- new table, killed_refs clone
    session.broadcastToCell(cell,
        json.encode({ type = "WORLD_ITEM_SYNC", refs = { ref_id } }), char_id)
end
```

Cell-entry push (in the transition block, next to NPC_KILL_SYNC):

```lua
local takenItems = store.getWorldItemsTaken(cell)
if #takenItems > 0 then
    session.sendTo(char_id, json.encode({ type = "WORLD_ITEM_SYNC", refs = takenItems }))
end
```

New `store.lua` table:

```sql
CREATE TABLE IF NOT EXISTS world_items_taken (
    ref_id     INTEGER NOT NULL,
    cell       TEXT    NOT NULL,
    taken_by   INTEGER,
    taken_at   INTEGER DEFAULT (strftime('%s','now')),
    PRIMARY KEY (ref_id, cell)
);
```

**Not in scope for 12b:** respawn timers for world items (config later, mirror
`npc_respawn_hours`), putting items INTO containers (positive countDelta —
easy to add to the container scan later, but "shared stash" invites dupe
exploits; needs its own design pass).

**Definition of done:** A takes the sword off the table → it vanishes for B in
≤1s; C entering the cell an hour later never sees it.

---

## WP13 — Puppet NPC mirroring (executes PLAN.md WP10)

The big one: same NPC at the same position doing the same thing on all
screens. Three separately buildable stages.

### 13a — Extract shared interpolation (no behavior change)

`ghost_system.cpp` owns the snapshot ring (`Snap`, SNAP_CAP=16), shortest-arc
rotation lerp, 250ms extrapolation, >512-unit snap, and `WriteRef`. Extract to
`interp.h/interp.cpp`:

```cpp
// interp.h
struct InterpSnap { DWORD ms; float x, y, z, rotZ; };

class InterpBuffer {
public:
    void push(DWORD ms, float x, float y, float z, float rotZ);
    // Sample the position INTERP_DELAY ms behind 'now'; extrapolates ≤250ms
    // on underrun; returns false when the buffer is empty.
    bool sample(DWORD now, float& x, float& y, float& z, float& rotZ) const;
    void clear();
    bool wantsSnap(float curX, float curY) const;  // >512u → teleport, no lerp
private:
    InterpSnap ring_[16];
    int head_ = 0, count_ = 0;
};

void Interp_WriteRef(void* ref, float x, float y, float z, float rotZ);
```

GhostSystem becomes a consumer; behavior byte-identical (playtest = "movement
still smooth" — reuses the existing two-client setup, cheap to verify).

### 13b — Authority streams NPC_POS

Authority already walks actors 1×/s for NPC_HP. Add a 3Hz position stream for
**actors in combat or moving**, capped at the 8 nearest to any player:

```cpp
// npc_sync.cpp — authority only, 333ms cadence
struct NpcPosEntry { uint32_t ref; float x, y, z, rot; int anim; };

static void StreamNpcPosAsAuthority(void* cell, const std::string& cellKey) {
    struct Cand { uint32_t ref; float x,y,z,rot; float d2; bool moved; };
    std::vector<Cand> cands;
    float px = PlayerX(), py = PlayerY();   // + ghost positions (peers' players)

    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        bool isStatic  = IsActorRef(ref, refId);               // 00-index NPC/creature
        bool isDynamic = IsDynamicActor(ref, refId);           // FF replicas source
        if (!isStatic && !isDynamic) return true;
        if (g_knownDead.count(refId)) return true;
        float x = *(float*)((char*)ref + Oblivion::kRef_posX);
        float y = *(float*)((char*)ref + Oblivion::kRef_posY);
        float z = *(float*)((char*)ref + Oblivion::kRef_posZ);
        float r = *(float*)((char*)ref + Oblivion::kRef_rotZ);
        auto& last = g_lastStreamPos[refId];                    // map<u32, {x,y,z}>
        bool moved = dist2(x, y, last.x, last.y) > 4.f;         // >2 units
        cands.push_back({refId, x, y, z, r, dist2(x, y, px, py), moved});
        last = {x, y, z};
        return true;
    });

    // moved-only, nearest 8, delta encoding
    std::sort(cands.begin(), cands.end(), [](auto& a, auto& b){ return a.d2 < b.d2; });
    std::string arr; int n = 0;
    for (auto& c : cands) {
        if (!c.moved || n >= 8) continue;   // idle actors cost nothing
        char e[128];
        snprintf(e, sizeof(e),
            "%s{\"ref\":%u,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"rot\":%.3f}",
            n ? "," : "", c.ref, c.x, c.y, c.z, c.rot);
        arr += e; ++n;
    }
    if (n == 0) return;
    g_network.send("{\"type\":\"NPC_POS\",\"cell\":\"" + cellKey + "\",\"npcs\":[" + arr + "]}");
}
```

`ref` in the packet: static refID as-is; for dynamics it IS the sid — followers
translate sid → replica ref via `g_replicas`. Bandwidth: ≤8 actors × 3Hz ×
~90B ≈ 2 KB/s per cell — noise next to POSITION_UPDATE.

Anim buckets (idle/walk/run/attack) ride along later; v1 positions only —
walking anims come free because followers' puppets still run `PlayGroup`
Forward when displaced (same trick the ghosts use).

**Server**: relay handler = clone of `handleNpcHp` (authority-only inbound,
validate finite floats, cap 8, broadcast to cell minus sender).

### 13c — Follower puppet mode

When NPC_POS entries arrive for a ref/sid, the follower flips that actor to
puppet mode and drives it from the interp buffer on the Present-hook tick
(positions cached game-side, same discipline as ghosts):

```cpp
// npc_puppet.cpp (new) — follower-side state
struct Puppet {
    void*        ref = nullptr;
    InterpBuffer interp;
    DWORD        lastPacketMs = 0;
    bool         restrained   = false;
};
static std::unordered_map<uint32_t, Puppet> g_puppets;   // key: static refId or sid

// On NPC_POS (network thread → pending queue → game tick):
void NpcPuppet_OnPos(uint32_t key, float x, float y, float z, float rot) {
    Puppet& p = g_puppets[key];               // ref resolved on game tick
    p.interp.push(GetTickCount(), x, y, z, rot);
    p.lastPacketMs = GetTickCount();
}

// Game tick: resolve refs, manage restrained state
void NpcPuppet_Tick(const std::string& cellKey) {
    for (auto& [key, p] : g_puppets) {
        if (!p.ref) p.ref = ResolvePuppetRef(key);   // LookupFormByID / g_replicas[sid]
        if (!p.ref) continue;
        if (!p.restrained) {
            GameHooks_EnqueueCmdOnRef(p.ref, "setrestrained 1");  // AI off — NOT setghost
            p.restrained = true;
        }
        // Stream stopped >3s → actor left combat/range on authority: release
        if (GetTickCount() - p.lastPacketMs > 3000) {
            GameHooks_EnqueueCmdOnRef(p.ref, "setrestrained 0");
            // erase after loop
        }
    }
}

// Present hook (via cached ref list — no engine calls):
void NpcPuppet_FrameUpdate() {
    DWORD now = GetTickCount();
    for (auto& [key, p] : g_puppets) {
        float x, y, z, r;
        if (p.ref && p.restrained && p.interp.sample(now, x, y, z, r))
            Interp_WriteRef(p.ref, x, y, z, r);
    }
}
```

Key decisions baked in (from PLAN.md WP10, unchanged):
- Puppets are `setrestrained 1` but **never** `setghost` — players must be able
  to hit them. Follower hits flow through the existing NPC_DAMAGE (statics) and
  WP11 NPC_DAMAGE_SID (replicas) paths — **no new damage code needed**.
- The release-on-silence rule (3s) doubles as authority handover cleanup: new
  authority streams different refs; stale puppets un-restrain and resume local AI.
- Fundamental compromise: puppets never attack followers. The authority's
  screen is where NPC aggression is real. Document in README/MOTD.
- Cell change → clear all puppets (no un-restrain needed; refs are gone).

**Definition of done (= WP10's):** two clients watch the same bandit pace the
same patrol; either can kill it; the corpse lies in the same spot for both.

---

## WP14 — Monitor all quests (full quest sync)

**Problem.** Only quests listed in `quests.json` sync. "Full" = any quest any
player advances syncs by its scope rules, including mod-added ones (same
editor-ID namespace on both clients since load orders must match for quests
anyway — vanilla-only filter NOT needed here, editor IDs are strings).

**Client** (`quest_sync.cpp`): instead of resolving a fixed list, walk the
whole DataHandler quest list every poll and track ALL stages:

```cpp
// Replaces the Monitored vector with a map keyed by TESQuest*
static std::unordered_map<void*, int> g_stages;   // quest → lastStage
static bool g_baselined = false;

void QuestSync_Tick() {
    // …5s cadence + connected guard unchanged…
    void* dh = *(void**)kDataHandlerPtr;
    if (!dh) return;
    struct Node { void* item; Node* next; };
    for (Node* n = (Node*)((char*)dh + kDH_questList); n; n = n->next) {
        void* q = n->item;
        if (!q) continue;
        const char* name = *(const char**)((char*)q + kQuest_edNameData);
        uint16_t    len  = *(uint16_t*)((char*)q + kQuest_edNameLen);
        if (!name || len == 0 || len > 64) continue;

        int stage = *(uint8_t*)((char*)q + kQuest_stageIndex);
        auto it = g_stages.find(q);
        if (it == g_stages.end()) {                  // first sight
            g_stages[q] = stage;
            if (!g_baselined && stage == 0) continue; // baseline: only non-zero uploads
        } else if (stage <= it->second) {
            it->second = stage;                       // regression: track, don't send
            continue;
        } else {
            it->second = stage;
        }
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"QUEST_STAGE\",\"questId\":\"%.*s\",\"stage\":%d}",
            (int)len, name, stage);
        g_network.send(buf);
    }
    g_baselined = true;
}
```

`QUEST_SYNC` apply (setstage by editor ID) already works for arbitrary IDs —
no client apply change.

**Server** (`quests.lua`): `getMonitored` list becomes optional (client no
longer needs it in CHAR_LOAD, but keep sending for old-client compat).
Scoping flips from allowlist to rules-first:

```lua
-- quests.json keeps global_quests/global_prefixes (MQ*, etc.).
-- NEW: everything not matching a global rule is personal/party-scoped by
-- default (today unlisted quests are simply never reported).
-- Add a denylist for engine-noise quests that must never sync:
--   "ignored_prefixes": ["Dark", ...]   ← answered by open question Q4
function M.isIgnored(questId)
    for _, prefix in ipairs(questConfig.ignored_prefixes or {}) do
        if questId:sub(1, #prefix) == prefix then return true end
    end
    return false
end
-- handleQuestStage: early-return if isIgnored(questId). Rest unchanged —
-- forward-only + global/party scoping already handles arbitrary IDs.
```

Also add a rate guard: a client bursting >40 QUEST_STAGE packets in one poll
window (corrupted memory walk) gets the batch dropped + audit-logged.

**Risk note:** Oblivion has ~200 quests, many of which are scripted controllers
(e.g. `TrapSetup`, conversation drivers) whose stages churn. The regression
guard + forward-only server absorbs most noise; the `ignored_prefixes`
denylist handles the rest as discovered in playtest. First playtest for this
WP should watch the server log for spam.

**Definition of done:** any side quest picked up by A advances for party
member B without touching quests.json.

---

## WP15 — Protocol & test additions

| Packet | Direction | Payload | WP |
|---|---|---|---|
| `NPC_DAMAGE_SID` | C→S→authority | `cell, sid, amount` | 11 |
| `WORLD_ITEM_TAKEN` | C→S | `ref_id, cell` | 12b |
| `WORLD_ITEM_SYNC` | S→C | `refs:[u32]` (broadcast + cell-entry push) | 12b |
| `NPC_POS` | authority→S→cell | `cell, npcs:[{ref,x,y,z,rot}]` ≤8, 3Hz | 13b |

Client `PacketType` enum continues from its current max.

Integration tests (each lands with its WP):
1. WP11: NPC_DAMAGE_SID routed only to authority; non-dynamic sid rejected; authority-origin dropped.
2. WP12a: ITEM_TAKEN from a corpse ref persists and pushes CONTAINER_STATE on re-entry.
3. WP12b: WORLD_ITEM_TAKEN → peer gets WORLD_ITEM_SYNC; late joiner gets it on cell entry; dynamic ref_id rejected.
4. WP13b: NPC_POS relayed from authority, rejected from follower; >8 entries truncated; non-finite floats dropped.
5. WP14: unlisted quest ID accepted + party-scoped; ignored prefix dropped; >40-packet burst dropped + audited.

---

## Explicitly out of scope (this round)

- Replica (dynamic) **corpse loot** — needs sid-keyed container ops (noted in 12a).
- Items **into** containers / player trading — dupe-exploit design pass needed.
- NPC animation graph sync beyond PlayGroup buckets.
- Quest **objective/journal-entry** replication beyond stages (stages drive
  journal + world state already; setstage on the receiving client regenerates
  both).
- FaceGen replication (unchanged from PLAN.md).

## Open questions (answer before execution)

- **Q1 — WP9 playtest first?** WP11/13 build on untested WP9 spawn code.
  Options: (a) playtest WP9 before starting, (b) build WP11 now and playtest
  both together, (c) start with WP12 loot (independent of WP9) while waiting.
- **Q2 — Loot exclusivity confirmed?** Shared-world subtractive ("you took it,
  I lose it") for corpses and world items — or should world items stay
  per-player (only containers shared)?
- **Q3 — Puppet scope:** stream only actors in combat/moving (plan above), or
  everything that moves including town schedule-walkers (more alive, ~2× the
  packets, more puppet churn)?
- **Q4 — Quest scope default:** unlisted quests default to party-scoped
  (plan above) or global? And keep MQ*/guild rules as-is?
