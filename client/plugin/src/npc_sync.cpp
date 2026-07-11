#include "npc_sync.h"
#include "npc_spawn_sync.h"
#include "npc_puppet.h"
#include "oblivion_internal.h"
#include "ghost_system.h"
#include "game_hooks.h"
#include "network.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Cell object list walk ─────────────────────────────────────────────────────
// TESObjectCELL::ObjectListEntry objectList is EMBEDDED at cell+0x048
// (GameForms.h): { TESObjectREFR* refr; ObjectListEntry* next; }.
// Matches ghost_system FindUnclaimedRefInCell.

static constexpr ptrdiff_t kCell_refList = 0x048; // embedded ObjectListEntry head

// ── getAV vtable dispatch — slot 0xA1, AV code 8 = Health ────────────────────
static float GetActorHp(void* actor) {
    typedef float(__fastcall* GetAVFn)(void*, void*, unsigned int);
    GetAVFn fn = ((GetAVFn*)(*(void***)actor))[0xA1];
    return fn(actor, nullptr, 8);
}

// ── State ─────────────────────────────────────────────────────────────────────

static std::string  g_cellKey;
static DWORD        g_lastNpcMs  = 0;
static DWORD        g_lastContMs = 0;

// NPC kill tracking
static std::unordered_set<uint32_t> g_knownAlive;   // refIds seen alive in current cell
static std::unordered_set<uint32_t> g_knownDead;    // refIds we've already reported dead

// Container snapshot: containerRefId → { itemFormId → countTaken }
static std::unordered_map<uint32_t, std::unordered_map<uint32_t, int>> g_containerSnapshot;

// Follower damage detection: last locally observed hp per NPC ref
static std::unordered_map<uint32_t, int> g_prevLocalHp;

// World-item pickup tracking (WP12b). Keyed on the ENGINE cell, not the zone
// key: an exterior zone spans 3×3 cells but the ref walk only covers the
// player's parentCell, so crossing a cell inside one zone must re-baseline or
// every item of the old cell would read as "taken".
static std::unordered_set<uint32_t> g_knownItems;
static std::unordered_set<uint32_t> g_takenItems;   // reported or peer-applied
static void*                        g_lastItemCell = nullptr;

// NPC position streaming (WP13, authority only)
struct StreamPos { float x, y, z, rot; };
static std::unordered_map<uint32_t, StreamPos> g_lastStreamPos;
static DWORD g_lastPosStreamMs = 0;

// ── Authority state (server-assigned per cell) ────────────────────────────────

static std::mutex   g_authMtx;
static std::string  g_authCell;        // cell key we hold authority for ("" = none)
static std::unordered_map<uint32_t, int> g_sentHp;       // authority: last hp broadcast
static std::unordered_map<uint32_t, int> g_authorityHp;  // follower: last hp received

// NPC_HP batches from the network thread, applied on the game-thread tick
static std::mutex                 g_pendMtx;
static std::vector<NpcHpEntry>    g_pendingHp;

// Server-pushed ref operations. They arrive on the network thread with only a
// refID; hex formID literals don't parse in the RunScriptLine path, so each op
// is resolved to a live TESObjectREFR* via LookupFormByID on the game thread
// and executed with the ref as calling context.
struct RefOp {
    enum Kind { Kill, KillSilent, RemoveItem, Damage, DisableItem } kind;
    uint32_t refId;
    uint32_t formId;  // RemoveItem
    int      amount;  // RemoveItem count / Damage amount
};
static std::vector<RefOp> g_pendingOps;

static void QueueRefOp(RefOp op) {
    std::lock_guard<std::mutex> lk(g_pendMtx);
    g_pendingOps.push_back(op);
}

static void ApplyPendingOps() {
    std::vector<RefOp> ops;
    {
        std::lock_guard<std::mutex> lk(g_pendMtx);
        std::swap(ops, g_pendingOps);
    }
    for (const RefOp& op : ops) {
        // Never run a mirrored op on the player's own ref — a bad or stale
        // server record keyed on 0x14 would kill/damage/strip THIS player.
        if (op.refId == Oblivion::kPlayerRefID) continue;
        void* ref = Oblivion::LookupFormByID(op.refId);
        if (!ref) continue;  // not loaded on this client — nothing to mirror

        char buf[64];
        switch (op.kind) {
        case RefOp::Kill:
            GameHooks_EnqueueCmdOnRef(ref, "kill");
            g_knownDead.insert(op.refId);
            g_knownAlive.erase(op.refId);
            break;
        case RefOp::KillSilent:
            GameHooks_EnqueueCmdOnRef(ref, "disable");
            g_knownDead.insert(op.refId);
            break;
        case RefOp::RemoveItem:
            snprintf(buf, sizeof(buf), "removeitem %08X %d", op.formId, op.amount);
            GameHooks_EnqueueCmdOnRef(ref, buf);
            break;
        case RefOp::Damage:
            snprintf(buf, sizeof(buf), "damageav health %d", op.amount);
            GameHooks_EnqueueCmdOnRef(ref, buf);
            break;
        case RefOp::DisableItem:
            // A peer took this world item — it no longer exists for us either.
            GameHooks_EnqueueCmdOnRef(ref, "disable");
            g_takenItems.insert(op.refId);
            break;
        }
    }
}

static void ResetCellState() {
    g_knownAlive.clear();
    g_knownDead.clear();
    g_containerSnapshot.clear();
    g_sentHp.clear();
    g_authorityHp.clear();
    g_prevLocalHp.clear();
    g_knownItems.clear();
    g_takenItems.clear();   // server re-pushes WORLD_ITEM_SYNC on cell entry
    g_lastItemCell = nullptr;
    g_lastStreamPos.clear();
    std::lock_guard<std::mutex> lk(g_pendMtx);
    g_pendingHp.clear();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static void* GetPlayer() {
    return *(void**)Oblivion::kPlayerPtr;
}

static void* GetPlayerCell() {
    void* player = GetPlayer();
    if (!player) return nullptr;
    return *(void**)((char*)player + Oblivion::kRef_parentCell);
}

// Iterate all refs in a cell, calling cb(ref, refId). Stop if cb returns false.
template<typename Fn>
static void WalkCellRefs(void* cell, Fn cb) {
    struct OLE { void* refr; OLE* next; };
    for (OLE* e = reinterpret_cast<OLE*>((char*)cell + kCell_refList); e; e = e->next) {
        void* ref = e->refr;
        if (ref) {
            uint32_t refId = *(uint32_t*)((char*)ref + Oblivion::kForm_refID);
            if (!cb(ref, refId)) return;
        }
    }
}

// ── NPC kill scan (~1s) ───────────────────────────────────────────────────────

static void ScanNpcKills(void* cell, const std::string& cellKey, bool isAuthority) {
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        // Skip player, ghosts, dynamic refs (formId > 0xFF000000 = runtime-placed)
        if (refId == 0 || refId == Oblivion::kPlayerRefID) return true;
        if ((refId & 0xFF000000) != 0) return true;  // skip dynamic refs
        if (GhostSystem_IsGhostRef(refId)) return true;

        void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
        if (!base) return true;

        uint8_t typeId = *(uint8_t*)((char*)base + Oblivion::kForm_typeID);
        if (typeId != Oblivion::kFormType_NPC && typeId != Oblivion::kFormType_Creature)
            return true;

        // Already reported dead — skip
        if (g_knownDead.count(refId)) return true;

        float hp = GetActorHp(ref);

        // Follower: report our local damage proactively. The authority only
        // broadcasts NPC_HP when ITS copy changes, so damage dealt by a
        // non-authority player would otherwise never merge.
        if (!isAuthority && hp > 0.f) {
            auto it = g_prevLocalHp.find(refId);
            if (it != g_prevLocalHp.end() && (int)hp < it->second - 1) {
                char buf[112];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"NPC_DAMAGE\",\"cell\":\"%s\",\"ref_id\":%u,\"amount\":%d}",
                    cellKey.c_str(), refId, it->second - (int)hp);
                g_network.send(buf);
            }
            g_prevLocalHp[refId] = (int)hp;
        }

        if (hp > 0.f) {
            g_knownAlive.insert(refId);
        } else if (g_knownAlive.count(refId)) {
            // Was alive, now dead — report it
            g_knownDead.insert(refId);
            g_knownAlive.erase(refId);

            char buf[96];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"NPC_KILLED\",\"ref_id\":%u,\"cell\":\"%s\"}",
                refId, cellKey.c_str());
            g_network.send(buf);
        }
        return true;
    });
}

// ── Container + corpse loot scan (~2s) ────────────────────────────────────────
// Dead actors are containers: looting a corpse writes the same
// ExtraContainerChanges data. Living actors are excluded — their inventory
// churns constantly from AI equip/unequip. Static refs only (replica corpse
// loot needs sid translation — documented gap in docs/FULL_SYNC_PLAN.md).

static void ScanContainerLoots(void* cell, const std::string& cellKey) {
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        if (refId == 0 || refId == Oblivion::kPlayerRefID) return true;
        if ((refId & 0xFF000000) != 0) return true;
        if (GhostSystem_IsGhostRef(refId)) return true;

        void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
        if (!base) return true;

        uint8_t typeId = *(uint8_t*)((char*)base + Oblivion::kForm_typeID);
        bool isContainer = (typeId == Oblivion::kFormType_Container);
        bool isCorpse    = (typeId == Oblivion::kFormType_NPC ||
                            typeId == Oblivion::kFormType_Creature)
                           && GetActorHp(ref) <= 0.f;
        if (!isContainer && !isCorpse) return true;

        // Check presence bit for kExtraData_ContainerChanges (0x1A = 26)
        // byte index = 26/8 = 3, bit index = 26%8 = 2
        const uint8_t* presenceBits = (const uint8_t*)((char*)ref + Oblivion::kRef_extraPresence);
        if (((presenceBits[3] >> 2) & 1) == 0) return true;

        // Walk BSExtraData linked list to find ContainerChanges entry
        void* ed = *(void**)((char*)ref + Oblivion::kRef_extraList);
        while (ed) {
            // GetType() = vtable[1](this)
            uint8_t edType = ((uint8_t(__fastcall*)(void*))(*(void***)ed)[1])(ed);
            if (edType == Oblivion::kExtraData_ContainerChanges) {
                void* eccData = *(void**)((char*)ed + 0x008);  // ExtraContainerChanges::data
                if (!eccData) break;
                void* entry = *(void**)eccData;  // Data::objList = Entry*
                while (entry) {
                    void* edata = *(void**)entry;  // EntryData*
                    if (edata) {
                        int32_t delta = *(int32_t*)((char*)edata + 0x004);  // countDelta
                        void*   iForm = *(void**)((char*)edata + 0x008);    // TESForm*
                        if (delta < 0 && iForm) {
                            uint32_t itemId = *(uint32_t*)((char*)iForm + Oblivion::kForm_refID);
                            int taken = (int)(-delta);

                            auto& snap = g_containerSnapshot[refId];
                            int prev = 0;
                            auto it = snap.find(itemId);
                            if (it != snap.end()) prev = it->second;

                            if (taken > prev) {
                                int newTaken = taken - prev;
                                snap[itemId] = taken;

                                char buf[128];
                                snprintf(buf, sizeof(buf),
                                    "{\"type\":\"ITEM_TAKEN\","
                                    "\"container_ref_id\":%u,"
                                    "\"item_form_id\":%u,"
                                    "\"count\":%d,"
                                    "\"cell\":\"%s\"}",
                                    refId, itemId, newTaken, cellKey.c_str());
                                g_network.send(buf);
                            }
                        }
                    }
                    entry = *(void**)((char*)entry + 0x004);  // Entry::next
                }
                break;
            }
            ed = *(void**)((char*)ed + 0x004);  // BSExtraData::next
        }
        return true;
    });
}

// ── World (loose) item pickup scan (~1s) ──────────────────────────────────────
// Picking up a placed item removes its ref from the cell walk (or leaves it
// disabled). Presence-diff against the last scan: known → gone = taken.
// First scan after an engine-cell change only baselines, so pre-existing
// absences are never reported — the server's persisted state covers those.

static bool IsLootableItemType(uint8_t t) {
    using namespace Oblivion;
    return t == kFormType_Weapon || t == kFormType_Armor  || t == kFormType_Clothing
        || t == kFormType_Misc   || t == kFormType_Ingredient
        || t == kFormType_Book   || t == kFormType_AlchemyItem
        || t == kFormType_SoulGem|| t == kFormType_Key    || t == kFormType_Ammo
        || t == kFormType_Apparatus;
}

static void ScanWorldItems(void* cell, const std::string& cellKey) {
    if (cell != g_lastItemCell) {
        // Engine cell changed under the same zone key (exterior sub-cell
        // crossing) — re-baseline or every old-cell item reads as taken.
        g_lastItemCell = cell;
        g_knownItems.clear();
    }

    std::unordered_set<uint32_t> present;
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        if (refId == 0 || (refId & 0xFF000000) != 0) return true;  // static only
        void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
        if (!base) return true;
        if (!IsLootableItemType(*(uint8_t*)((char*)base + Oblivion::kForm_typeID)))
            return true;
        uint32_t flags = *(uint32_t*)((char*)ref + Oblivion::kRef_flags);
        if (flags & Oblivion::kFlag_Disabled) return true;  // peer-sync applied / engine-off
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

// ── NPC HP authority scan (~1s, authority only) ───────────────────────────────

static bool IsActorRef(void* ref, uint32_t refId) {
    if (refId == 0 || refId == Oblivion::kPlayerRefID) return false;
    if ((refId & 0xFF000000) != 0) return false;       // dynamic (runtime-placed)
    if (GhostSystem_IsGhostRef(refId)) return false;
    void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
    if (!base) return false;
    uint8_t typeId = *(uint8_t*)((char*)base + Oblivion::kForm_typeID);
    return typeId == Oblivion::kFormType_NPC || typeId == Oblivion::kFormType_Creature;
}

static void ScanNpcHpAsAuthority(void* cell, const std::string& cellKey) {
    std::string batch;
    int n = 0;

    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        if (n >= 64) return false;
        if (!IsActorRef(ref, refId)) return true;
        if (g_knownDead.count(refId)) return true;     // kill flow owns dead NPCs

        int hp = (int)GetActorHp(ref);
        if (hp < 0) hp = 0;

        auto it = g_sentHp.find(refId);
        if (it != g_sentHp.end() && it->second == hp) return true;
        g_sentHp[refId] = hp;

        char e[48];
        snprintf(e, sizeof(e), "%s{\"ref\":%u,\"hp\":%d}", n ? "," : "", refId, hp);
        batch += e;
        ++n;
        return true;
    });

    if (n == 0) return;
    std::string pkt = "{\"type\":\"NPC_HP\",\"cell\":\"" + cellKey + "\",\"npcs\":["
                    + batch + "]}";
    g_network.send(pkt);
}

// ── NPC position streaming (authority, ~3Hz — WP13) ──────────────────────────
// Stream moving actors (static NPCs/creatures + our dynamic spawns) so
// followers can puppet them. Moved-only + nearest-8 keeps this ~2KB/s.

static void StreamNpcPosAsAuthority(void* cell, const std::string& cellKey) {
    using namespace Oblivion;
    void* player = GetPlayer();
    if (!player) return;
    float px = *(float*)((char*)player + kRef_posX);
    float py = *(float*)((char*)player + kRef_posY);

    struct Cand { uint32_t key; float x, y, z, rot, d2; };
    std::vector<Cand> cands;

    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        bool isStatic  = IsActorRef(ref, refId) && !g_knownDead.count(refId);
        bool isDynamic = (refId & 0xFF000000) == 0xFF000000
                         && !GhostSystem_IsGhostRef(refId);
        if (isDynamic) {
            void* base = *(void**)((char*)ref + kRef_baseForm);
            uint8_t t  = base ? *(uint8_t*)((char*)base + kForm_typeID) : 0;
            isDynamic  = (t == kFormType_NPC || t == kFormType_Creature);
        }
        if (!isStatic && !isDynamic) return true;
        if (GetActorHp(ref) <= 0.f) return true;

        float x = *(float*)((char*)ref + kRef_posX);
        float y = *(float*)((char*)ref + kRef_posY);
        float z = *(float*)((char*)ref + kRef_posZ);
        float r = *(float*)((char*)ref + kRef_rotZ);

        auto it = g_lastStreamPos.find(refId);
        bool moved = (it == g_lastStreamPos.end())
            ? false  // first sight only baselines — idle actors cost nothing
            : ((x - it->second.x) * (x - it->second.x)
             + (y - it->second.y) * (y - it->second.y) > 4.f
             || std::abs(r - it->second.rot) > 0.05f);
        g_lastStreamPos[refId] = { x, y, z, r };
        if (!moved) return true;

        float dx = x - px, dy = y - py;
        cands.push_back({ refId, x, y, z, r, dx * dx + dy * dy });
        return true;
    });

    if (cands.empty()) return;
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });

    std::string arr;
    int n = 0;
    for (const Cand& c : cands) {
        if (n >= 8) break;
        char e[128];
        snprintf(e, sizeof(e),
            "%s{\"ref\":%u,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"rot\":%.3f}",
            n ? "," : "", c.key, c.x, c.y, c.z, c.rot);
        arr += e;
        ++n;
    }
    g_network.send("{\"type\":\"NPC_POS\",\"cell\":\"" + cellKey + "\",\"npcs\":["
                   + arr + "]}");
}

// ── NPC HP apply (~game tick, followers) ─────────────────────────────────────

static void ApplyPendingHp(void* cell, const std::string& cellKey) {
    std::vector<NpcHpEntry> pending;
    {
        std::lock_guard<std::mutex> lk(g_pendMtx);
        std::swap(pending, g_pendingHp);
    }
    if (pending.empty()) return;

    // One walk to index the cell's actors by refId
    std::unordered_map<uint32_t, void*> refs;
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        if (IsActorRef(ref, refId)) refs[refId] = ref;
        return true;
    });

    for (const NpcHpEntry& e : pending) {
        auto it = refs.find(e.ref);
        if (it == refs.end()) continue;

        int local = (int)GetActorHp(it->second);
        g_authorityHp[e.ref] = e.hp;

        if (e.hp <= 0) {
            if (local > 0 && !g_knownDead.count(e.ref)) {
                GameHooks_EnqueueCmdOnRef(it->second, "kill");  // death anim
                g_knownDead.insert(e.ref);
                g_knownAlive.erase(e.ref);
            }
            continue;
        }

        if (local < e.hp - 2) {
            // We damaged this NPC locally and the authority doesn't know yet —
            // report the delta instead of losing it to the overwrite below.
            char buf[112];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"NPC_DAMAGE\",\"cell\":\"%s\",\"ref_id\":%u,\"amount\":%d}",
                cellKey.c_str(), e.ref, e.hp - local);
            g_network.send(buf);
        } else if (local != e.hp) {
            char buf[64];
            snprintf(buf, sizeof(buf), "setav health %d", e.hp);
            GameHooks_EnqueueCmdOnRef(it->second, buf);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void NpcSync_Tick(const std::string& cellKey) {
    static DWORD cellSettleMs = 0;

    DWORD now = GetTickCount();

    if (cellKey != g_cellKey) {
        g_cellKey = cellKey;
        ResetCellState();
        cellSettleMs = now;
    }

    if (cellKey.empty()) return;
    // Let the cell finish streaming in before touching its object list.
    if (now - cellSettleMs < 2000) return;

    void* cell = GetPlayerCell();
    if (!cell) return;

    // Server-pushed ref ops (kills, loot removal, damage) — all clients
    ApplyPendingOps();

    bool isAuthority;
    {
        std::lock_guard<std::mutex> lk(g_authMtx);
        isAuthority = (g_authCell == cellKey);
    }

    if (now - g_lastNpcMs >= 1000) {
        g_lastNpcMs = now;
        ScanNpcKills(cell, cellKey, isAuthority);
        ScanWorldItems(cell, cellKey);
        if (isAuthority) ScanNpcHpAsAuthority(cell, cellKey);
    }

    // WP13: authority streams moving-actor positions; followers manage puppets
    if (isAuthority && now - g_lastPosStreamMs >= 333) {
        g_lastPosStreamMs = now;
        StreamNpcPosAsAuthority(cell, cellKey);
    }
    NpcPuppet_Tick(cellKey, isAuthority);

    if (!isAuthority) ApplyPendingHp(cell, cellKey);

    if (now - g_lastContMs >= 200) {
        g_lastContMs = now;
        ScanContainerLoots(cell, cellKey);
    }

    // WP9: dynamic spawn mirroring (authority broadcast / follower replicas)
    NpcSpawnSync_Tick(cellKey, isAuthority);
}

void NpcSync_OnKilled(uint32_t refId) {
    QueueRefOp({ RefOp::Kill, refId, 0, 0 });
}

void NpcSync_OnKillSync(const uint32_t* refs, int count) {
    // Cell entry sync — disable silently (no death anim)
    for (int i = 0; i < count; ++i)
        QueueRefOp({ RefOp::KillSilent, refs[i], 0, 0 });
}

void NpcSync_OnItemSync(uint32_t containerRefId, uint32_t itemFormId, int count) {
    QueueRefOp({ RefOp::RemoveItem, containerRefId, itemFormId, count });
}

void NpcSync_OnWorldItemSync(const uint32_t* refs, int count) {
    for (int i = 0; i < count; ++i)
        QueueRefOp({ RefOp::DisableItem, refs[i], 0, 0 });
}

void NpcSync_OnContainerState(const ContainerEntry* entries, int count) {
    for (int i = 0; i < count; ++i)
        QueueRefOp({ RefOp::RemoveItem, entries[i].refId, entries[i].formId, entries[i].count });
}

void NpcSync_SetAuthority(const char* cellKey, bool authority) {
    std::lock_guard<std::mutex> lk(g_authMtx);
    if (authority) {
        g_authCell = cellKey ? cellKey : "";
    } else if (g_authCell == (cellKey ? cellKey : "")) {
        g_authCell.clear();
    }
}

void NpcSync_OnHpSync(const NpcHpEntry* entries, int count) {
    std::lock_guard<std::mutex> lk(g_pendMtx);
    for (int i = 0; i < count; ++i)
        g_pendingHp.push_back(entries[i]);
}

void NpcSync_OnDamageRequest(uint32_t refId, int amount) {
    if (amount <= 0) return;
    QueueRefOp({ RefOp::Damage, refId, 0, amount });
}
