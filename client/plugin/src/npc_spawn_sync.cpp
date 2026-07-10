#include "npc_spawn_sync.h"
#include "oblivion_internal.h"
#include "game_hooks.h"
#include "ghost_system.h"
#include "network.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static void SSDBG(const std::string& s) {
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << "[spawn] " << s << "\n";
}

// ── Cell walk (embedded ObjectListEntry at cell+0x048, per GameForms.h) ───────

template<typename Fn>
static void WalkRefs(void* cell, Fn cb) {
    struct OLE { void* refr; OLE* next; };
    for (OLE* e = reinterpret_cast<OLE*>((char*)cell + 0x048); e; e = e->next) {
        if (e->refr) {
            if (!cb(e->refr, *(uint32_t*)((char*)e->refr + Oblivion::kForm_refID)))
                return;
        }
    }
}

static bool IsDynamicActor(void* ref, uint32_t refId) {
    if ((refId & 0xFF000000) != 0xFF000000) return false;  // dynamic refs only
    // Player ghosts are ALSO PlaceAtMe dynamic NPC refs — never treat them as
    // mobs (suppressing one would disable a fellow player's ghost; an authority
    // would broadcast them as spawns and followers would replicate them).
    if (GhostSystem_IsGhostRef(refId)) return false;
    void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
    if (!base) return false;
    uint8_t t = *(uint8_t*)((char*)base + Oblivion::kForm_typeID);
    return t == Oblivion::kFormType_NPC || t == Oblivion::kFormType_Creature;
}

static float ActorHp(void* actor) {
    typedef float(__fastcall* GetAVFn)(void*, void*, unsigned int);
    return (((GetAVFn*)(*(void***)actor))[0xA1])(actor, nullptr, 8);
}

static uint32_t BaseId(void* ref) {
    void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
    return base ? *(uint32_t*)((char*)base + Oblivion::kForm_refID) : 0;
}

// ── State ─────────────────────────────────────────────────────────────────────

struct Replica {
    void*    ref     = nullptr;  // claimed local replica (null while spawning)
    uint32_t refId   = 0;
    uint32_t base    = 0;
    float    x = 0, y = 0, z = 0;
    int      lastHp  = -1;
};

static std::string g_cell;
static DWORD       g_lastSendMs   = 0;
static int         g_lastSentN    = 0;

static std::unordered_map<uint32_t, Replica> g_replicas;   // sid → replica
static std::unordered_set<uint32_t>          g_replicaIds; // local refIds
static std::unordered_set<uint32_t>          g_suppressed; // disabled local rolls
static std::unordered_set<uint32_t>          g_failedSids; // spawn gave up

// spawn serialization
static uint32_t g_spawningSid   = 0;   // 0 = none in flight
static int      g_spawnStrategy = 0;
static DWORD    g_spawnBeganMs  = 0;

static std::mutex               g_snapMtx;
static std::vector<SpawnEntry>  g_pendingSnap;
static bool                     g_snapDirty = false;

void NpcSpawnSync_OnSnapshot(const SpawnEntry* entries, int count) {
    std::lock_guard<std::mutex> lk(g_snapMtx);
    g_pendingSnap.assign(entries, entries + count);
    g_snapDirty = true;
}

bool NpcSpawnSync_IsReplica(uint32_t refId) {
    return g_replicaIds.count(refId) != 0;
}

static void ResetState() {
    g_replicas.clear();
    g_replicaIds.clear();
    g_suppressed.clear();
    g_failedSids.clear();
    g_spawningSid = 0;
    g_lastSentN   = 0;
    std::lock_guard<std::mutex> lk(g_snapMtx);
    g_pendingSnap.clear();
    g_snapDirty = false;
}

// ── Authority: broadcast dynamic spawn snapshot ───────────────────────────────

static void AuthorityBroadcast(void* cell, const std::string& cellKey) {
    std::string arr;
    int n = 0;
    WalkRefs(cell, [&](void* ref, uint32_t refId) {
        if (n >= 24) return false;
        if (!IsDynamicActor(ref, refId)) return true;
        // Handover: an ex-follower promoted to authority still has its own
        // suppressed (disabled) local rolls in the cell list — never broadcast
        // those as live spawns.
        if (g_suppressed.count(refId)) return true;
        int hp = (int)ActorHp(ref);
        if (hp <= 0) return true;                 // dead ones age out of snapshots
        uint32_t base = BaseId(ref);
        if (!base || (base >> 24) != 0) return true;  // vanilla bases only
        float x = *(float*)((char*)ref + Oblivion::kRef_posX);
        float y = *(float*)((char*)ref + Oblivion::kRef_posY);
        float z = *(float*)((char*)ref + Oblivion::kRef_posZ);
        char e[128];
        snprintf(e, sizeof(e),
            "%s{\"sid\":%u,\"base\":%u,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"hp\":%d}",
            n ? "," : "", refId, base, x, y, z, hp);
        arr += e;
        ++n;
        return true;
    });

    if (n == 0 && g_lastSentN == 0) return;  // nothing, and peers know it
    g_lastSentN = n;
    g_network.send("{\"type\":\"NPC_SPAWNS\",\"cell\":\"" + cellKey + "\",\"spawns\":["
                   + arr + "]}");
}

// ── Follower: replica spawn primitive ─────────────────────────────────────────
// Hex formIDs with A–F digits don't parse in the RunScriptLine path (verified
// live). Strategy 0 uses an OBSE temp script (decimal literal via
// GetFormFromMod); strategy 1 works iff the hex string is all decimal digits.

static bool HexAllDecimal(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", v);
    for (const char* c = buf; *c; ++c)
        if (*c < '0' || *c > '9') return false;
    return true;
}

static bool IssueSpawnCmd(uint32_t base, int strategy) {
    char buf[192];
    switch (strategy) {
    case 0:
        snprintf(buf, sizeof(buf),
            "ref TES4MPSpawnRef\r\n"
            "let TES4MPSpawnRef := GetFormFromMod \"Oblivion.esm\" %u\r\n"
            "player.PlaceAtMe TES4MPSpawnRef 1",
            base & 0x00FFFFFF);
        GameHooks_EnqueueCmd(buf);
        return true;
    case 1:
        if (!HexAllDecimal(base)) return false;
        snprintf(buf, sizeof(buf), "player.PlaceAtMe %08X 1", base);
        GameHooks_EnqueueCmd(buf);
        return true;
    default:
        return false;
    }
}

// Newest unclaimed dynamic ref with the wanted base (same claim trick as ghosts).
static void* FindFreshSpawn(void* cell, uint32_t base) {
    void*    best   = nullptr;
    uint32_t bestId = 0;
    WalkRefs(cell, [&](void* ref, uint32_t refId) {
        if (!IsDynamicActor(ref, refId)) return true;
        if (g_replicaIds.count(refId) || g_suppressed.count(refId)) return true;
        if (BaseId(ref) != base) return true;
        if (refId > bestId) { bestId = refId; best = ref; }
        return true;
    });
    return best;
}

// ── Follower tick ─────────────────────────────────────────────────────────────

static void FollowerTick(void* cell, const std::string& cellKey) {
    // Apply the latest snapshot
    std::vector<SpawnEntry> snap;
    bool haveSnap = false;
    {
        std::lock_guard<std::mutex> lk(g_snapMtx);
        if (g_snapDirty) { snap = g_pendingSnap; g_snapDirty = false; haveSnap = true; }
    }

    if (haveSnap) {
        std::unordered_set<uint32_t> live;
        for (const SpawnEntry& e : snap) {
            live.insert(e.sid);
            auto it = g_replicas.find(e.sid);
            if (it != g_replicas.end()) {
                Replica& r = it->second;
                // The authority value is truth — it already merged any
                // NPC_DAMAGE_SID we reported (WP11). Apply on any difference,
                // and ALWAYS track it so the damage poll below diffs against
                // the authority's view, not our own last write.
                if (r.ref && e.hp > 0 && r.lastHp > 0 && e.hp != r.lastHp) {
                    char buf[48];
                    snprintf(buf, sizeof(buf), "setav health %d", e.hp);
                    GameHooks_EnqueueCmdOnRef(r.ref, buf);
                }
                r.lastHp = e.hp;
            } else if (!g_failedSids.count(e.sid) && g_spawningSid != e.sid) {
                Replica r;
                r.base = e.base; r.x = e.x; r.y = e.y; r.z = e.z; r.lastHp = e.hp;
                g_replicas[e.sid] = r;  // ref filled by the spawn machinery below
            }
        }
        // sids gone from the snapshot → their replicas die/despawn
        for (auto it = g_replicas.begin(); it != g_replicas.end();) {
            if (!live.count(it->first)) {
                if (it->second.ref) {
                    GameHooks_EnqueueCmdOnRef(it->second.ref, "kill");
                    g_replicaIds.erase(it->second.refId);
                }
                if (g_spawningSid == it->first) g_spawningSid = 0;
                it = g_replicas.erase(it);
            } else ++it;
        }
    }

    // WP11: our damage on replicas must merge back. A live replica sitting
    // below the last authority-known hp means WE hit it — report the delta.
    // The authority applies it and the merged value returns in the next
    // snapshot (which we then apply + track above).
    for (auto& [sid, r] : g_replicas) {
        if (!r.ref || r.lastHp <= 0) continue;
        int localHp = (int)ActorHp(r.ref);
        if (localHp < r.lastHp - 1) {   // tolerance 1: float/regen noise
            char buf[112];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"NPC_DAMAGE_SID\",\"cell\":\"%s\",\"sid\":%u,\"amount\":%d}",
                cellKey.c_str(), sid, r.lastHp - localHp);
            g_network.send(buf);
            r.lastHp = localHp;  // don't resend; next snapshot reconciles
        }
    }

    DWORD now = GetTickCount();

    // Spawn machinery — one replica in flight at a time
    if (g_spawningSid) {
        auto it = g_replicas.find(g_spawningSid);
        if (it == g_replicas.end()) {
            g_spawningSid = 0;
        } else if (!it->second.ref) {
            void* found = FindFreshSpawn(cell, it->second.base);
            if (found) {
                Replica& r = it->second;
                r.ref   = found;
                r.refId = *(uint32_t*)((char*)found + Oblivion::kForm_refID);
                g_replicaIds.insert(r.refId);
                char buf[64];
                snprintf(buf, sizeof(buf), "setpos x %.1f", r.x);
                GameHooks_EnqueueCmdOnRef(found, buf);
                snprintf(buf, sizeof(buf), "setpos y %.1f", r.y);
                GameHooks_EnqueueCmdOnRef(found, buf);
                snprintf(buf, sizeof(buf), "setpos z %.1f", r.z);
                GameHooks_EnqueueCmdOnRef(found, buf);
                SSDBG("replica up sid=" + std::to_string(g_spawningSid)
                      + " strategy=" + std::to_string(g_spawnStrategy));
                g_spawningSid = 0;
            } else if (now - g_spawnBeganMs > 3000) {
                // this strategy didn't produce a ref — try the next
                g_spawnStrategy++;
                if (IssueSpawnCmd(it->second.base, g_spawnStrategy)) {
                    g_spawnBeganMs = now;
                    SSDBG("spawn retry sid=" + std::to_string(g_spawningSid)
                          + " strategy=" + std::to_string(g_spawnStrategy));
                } else {
                    SSDBG("spawn FAILED sid=" + std::to_string(g_spawningSid)
                          + " base=" + std::to_string(it->second.base));
                    g_failedSids.insert(g_spawningSid);
                    g_replicas.erase(it);
                    g_spawningSid = 0;
                }
            }
        } else {
            g_spawningSid = 0;
        }
    }

    if (!g_spawningSid) {
        for (auto& [sid, r] : g_replicas) {
            if (r.ref || g_failedSids.count(sid)) continue;
            g_spawningSid   = sid;
            g_spawnStrategy = 0;
            g_spawnBeganMs  = now;
            IssueSpawnCmd(r.base, 0);
            break;
        }
    }

    // Suppress our own local rolls — but never while a spawn is in flight
    // (a fresh replica would look like a local roll before it's claimed).
    if (!g_spawningSid) {
        WalkRefs(cell, [&](void* ref, uint32_t refId) {
            if (!IsDynamicActor(ref, refId)) return true;
            if (g_replicaIds.count(refId) || g_suppressed.count(refId)) return true;
            // Leave corpses alone (dead replicas whose sid left the snapshot,
            // and anything killed before we joined) — vanishing bodies look
            // worse than duplicate ones lying around.
            if (ActorHp(ref) <= 0.f) return true;
            GameHooks_EnqueueCmdOnRef(ref, "disable");
            g_suppressed.insert(refId);
            return true;
        });
    }
}

// ── Public tick ───────────────────────────────────────────────────────────────

void NpcSpawnSync_Tick(const std::string& cellKey, bool isAuthority) {
    if (cellKey != g_cell) {
        g_cell = cellKey;
        ResetState();
    }
    if (cellKey.empty() || !g_network.isConnected()) return;

    void* player = *(void**)Oblivion::kPlayerPtr;
    if (!player) return;
    void* cell = *(void**)((char*)player + Oblivion::kRef_parentCell);
    if (!cell) return;

    DWORD now = GetTickCount();

    if (isAuthority) {
        if (now - g_lastSendMs >= 2000) {
            g_lastSendMs = now;
            AuthorityBroadcast(cell, cellKey);
        }
    } else {
        FollowerTick(cell, cellKey);
    }
}
