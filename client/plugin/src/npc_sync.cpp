#include "npc_sync.h"
#include "oblivion_internal.h"
#include "ghost_system.h"
#include "game_hooks.h"
#include "network.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Cell object list walk ─────────────────────────────────────────────────────
// TESObjectCELL linked list of refs sits at cell+0x048.
// Same layout used by ghost_system FindUnclaimedRefInCell.
// Each node: void* at +0x000 = next, void* at +0x008 = TESObjectREFR*

static constexpr ptrdiff_t kCell_refList      = 0x048; // BSSimpleList<TESObjectREFR*> head
static constexpr ptrdiff_t kCellNode_next     = 0x000;
static constexpr ptrdiff_t kCellNode_ref      = 0x008;

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

// ── Authority state (server-assigned per cell) ────────────────────────────────

static std::mutex   g_authMtx;
static std::string  g_authCell;        // cell key we hold authority for ("" = none)
static std::unordered_map<uint32_t, int> g_sentHp;       // authority: last hp broadcast
static std::unordered_map<uint32_t, int> g_authorityHp;  // follower: last hp received

// NPC_HP batches from the network thread, applied on the game-thread tick
static std::mutex                 g_pendMtx;
static std::vector<NpcHpEntry>    g_pendingHp;

static void ResetCellState() {
    g_knownAlive.clear();
    g_knownDead.clear();
    g_containerSnapshot.clear();
    g_sentHp.clear();
    g_authorityHp.clear();
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
    void* node = *(void**)((char*)cell + kCell_refList);
    while (node) {
        void* ref = *(void**)((char*)node + kCellNode_ref);
        if (ref) {
            uint32_t refId = *(uint32_t*)((char*)ref + Oblivion::kForm_refID);
            if (!cb(ref, refId)) return;
        }
        node = *(void**)((char*)node + kCellNode_next);
    }
}

// ── NPC kill scan (~1s) ───────────────────────────────────────────────────────

static void ScanNpcKills(void* cell, const std::string& cellKey) {
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        // Skip player, ghosts, dynamic refs (formId > 0xFF000000 = runtime-placed)
        if (refId == 0) return true;
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

// ── Container loot scan (~2s) ─────────────────────────────────────────────────

static void ScanContainerLoots(void* cell, const std::string& cellKey) {
    WalkCellRefs(cell, [&](void* ref, uint32_t refId) {
        if (refId == 0) return true;
        if ((refId & 0xFF000000) != 0) return true;
        if (GhostSystem_IsGhostRef(refId)) return true;

        void* base = *(void**)((char*)ref + Oblivion::kRef_baseForm);
        if (!base) return true;

        uint8_t typeId = *(uint8_t*)((char*)base + Oblivion::kForm_typeID);
        if (typeId != Oblivion::kFormType_Container) return true;

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

// ── NPC HP authority scan (~1s, authority only) ───────────────────────────────

static bool IsActorRef(void* ref, uint32_t refId) {
    if (refId == 0) return false;
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
                NpcSync_OnKilled(e.ref);  // prid + kill (death anim)
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
            snprintf(buf, sizeof(buf), "prid %X", e.ref);
            GameHooks_EnqueueCmd(buf);
            snprintf(buf, sizeof(buf), "setav health %d", e.hp);
            GameHooks_EnqueueCmd(buf);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void NpcSync_Tick(const std::string& cellKey) {
    if (cellKey != g_cellKey) {
        g_cellKey = cellKey;
        ResetCellState();
    }

    if (cellKey.empty()) return;

    void* cell = GetPlayerCell();
    if (!cell) return;

    DWORD now = GetTickCount();

    bool isAuthority;
    {
        std::lock_guard<std::mutex> lk(g_authMtx);
        isAuthority = (g_authCell == cellKey);
    }

    if (now - g_lastNpcMs >= 1000) {
        g_lastNpcMs = now;
        ScanNpcKills(cell, cellKey);
        if (isAuthority) ScanNpcHpAsAuthority(cell, cellKey);
    }

    if (!isAuthority) ApplyPendingHp(cell, cellKey);

    if (now - g_lastContMs >= 200) {
        g_lastContMs = now;
        ScanContainerLoots(cell, cellKey);
    }
}

void NpcSync_OnKilled(uint32_t refId) {
    // Run `prid REFHEX; kill` — victim plays death animation
    char buf[64];
    snprintf(buf, sizeof(buf), "prid %X", refId);
    GameHooks_EnqueueCmd(buf);
    GameHooks_EnqueueCmd("kill");
    g_knownDead.insert(refId);
    g_knownAlive.erase(refId);
}

void NpcSync_OnKillSync(const uint32_t* refs, int count) {
    // Cell entry sync — disable silently (no death anim)
    for (int i = 0; i < count; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "prid %X", refs[i]);
        GameHooks_EnqueueCmd(buf);
        GameHooks_EnqueueCmd("disable");
        g_knownDead.insert(refs[i]);
    }
}

void NpcSync_OnItemSync(uint32_t containerRefId, uint32_t itemFormId, int count) {
    // Remove items from the container on this client
    char buf[128];
    snprintf(buf, sizeof(buf), "prid %X", containerRefId);
    GameHooks_EnqueueCmd(buf);
    snprintf(buf, sizeof(buf), "removeitem %X %d", itemFormId, count);
    GameHooks_EnqueueCmd(buf);
}

void NpcSync_OnContainerState(const ContainerEntry* entries, int count) {
    for (int i = 0; i < count; ++i) {
        char buf[128];
        snprintf(buf, sizeof(buf), "prid %X", entries[i].refId);
        GameHooks_EnqueueCmd(buf);
        snprintf(buf, sizeof(buf), "removeitem %X %d", entries[i].formId, entries[i].count);
        GameHooks_EnqueueCmd(buf);
    }
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
    char buf[64];
    snprintf(buf, sizeof(buf), "prid %X", refId);
    GameHooks_EnqueueCmd(buf);
    snprintf(buf, sizeof(buf), "damageav health %d", amount);
    GameHooks_EnqueueCmd(buf);
}
