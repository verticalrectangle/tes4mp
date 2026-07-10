#include "ghost_system.h"
#include "game_hooks.h"
#include "interp.h"
#include "oblivion_internal.h"
#include <windows.h>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <queue>
#include <vector>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstdint>

static void GS_DBG(const std::string& s) {
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << "[ghost] " << s << "\n";
}

// ── Config ─────────────────────────────────────────────────────────────────────

// Snapshot ring + interpolation constants live in interp.h (shared with
// puppet NPCs). INTERP_DELAY = Interp::DELAY_MS.
static constexpr DWORD   SPAWN_WAIT_MS  = 500;        // ms after PlaceAtMe before scanning
static constexpr DWORD   SPAWN_TIMEOUT  = 3000;       // ms — retry PlaceAtMe if scan fails
static constexpr uint32_t GHOST_BASE_NPC = 0x0001C34F; // preferred: Imperial Watch guard

// ── Slot pool ─────────────────────────────────────────────────────────────────

static int        g_numSlots    = 0;
static GhostCmdFn g_cmdFn       = nullptr;
static bool       g_slotFree[32] = {};
static void*      g_slotRefs[32] = {};  // TESObjectREFR* per slot, filled by cell scan

static void EnqCmd(const std::string& s) {
    if (g_cmdFn) g_cmdFn(nullptr, s.c_str());
}

static void EnqRef(void* ref, const std::string& s) {
    if (g_cmdFn) g_cmdFn(ref, s.c_str());
}

// ── Spawn base resolution ─────────────────────────────────────────────────────
// The historical 0x1C34F constant was never verified. Validate it via the
// engine's form table; if it isn't an NPC_ base, take the first vanilla NPC_
// from the DataHandler bound-object list (race/name/gender get overridden
// per-ghost anyway). Game thread only. 0 = nothing usable (retry later).

static uint32_t g_ghostBase = 0;

static uint32_t ResolveGhostBase() {
    using namespace Oblivion;
    if (g_ghostBase) return g_ghostBase;

    void* f = LookupFormByID(GHOST_BASE_NPC);
    uint8_t ft = f ? *(uint8_t*)((char*)f + kForm_typeID) : 0;
    if (f && ft == kFormType_NPC) {
        g_ghostBase = GHOST_BASE_NPC;
        GS_DBG("spawn base 1C34F verified (NPC_)");
        return g_ghostBase;
    }
    GS_DBG("spawn base 1C34F invalid (form=" + std::to_string((uintptr_t)f)
           + " type=" + std::to_string(ft) + ") — scanning DataHandler");

    void* dh = *(void**)kDataHandlerPtr;
    if (!dh) return 0;
    void* head = *(void**)dh;                       // BoundObjectListHead*
    if (!head) return 0;
    void* obj = *(void**)((char*)head + 0x004);     // first TESBoundObject*
    while (obj) {
        uint8_t t = *(uint8_t*)((char*)obj + kForm_typeID);
        uint32_t fid = *(uint32_t*)((char*)obj + kForm_refID);
        // vanilla (index 00), skip the player base (0x7)
        if (t == kFormType_NPC && (fid >> 24) == 0 && fid != 0x7) {
            char buf[64];
            snprintf(buf, sizeof(buf), "spawn base fallback: %08X", fid);
            GS_DBG(buf);
            g_ghostBase = fid;
            return g_ghostBase;
        }
        obj = *(void**)((char*)obj + 0x020);        // TESBoundObject::next
    }
    GS_DBG("no NPC_ base found in DataHandler");
    return 0;
}

// ── Claimed ref tracking ──────────────────────────────────────────────────────
// Prevents two ghosts appearing simultaneously from claiming the same newly placed ref.

static std::set<uint32_t> g_claimedFids;

// ── Race EditorID lookup (Oblivion.esm — formID low 3 bytes) ─────────────────

static const char* RaceEditorId(uint32_t fid) {
    switch (fid & 0x00FFFFFF) {
    case 0x023FE9: return "ImperialRace";
    case 0x0224FD: return "NordRace";
    case 0x023FEA: return "DarkElfRace";
    case 0x0224FC: return "HighElfRace";
    case 0x0224FE: return "WoodElfRace";
    case 0x0224FF: return "BretonRace";
    case 0x0224F8: return "OrcRace";
    case 0x0224F9: return "RedguardRace";
    case 0x0224FB: return "KhajiitRace";
    case 0x0224FA: return "ArgonianRace";
    default:       return nullptr;
    }
}

// ── Ghost state ───────────────────────────────────────────────────────────────

enum class Phase { Free, Spawning, Active };

struct Ghost {
    Phase       phase        = Phase::Free;
    int         slot         = -1;
    std::string name;
    uint32_t    raceFormId   = 0;
    int         gender       = 0;

    Interp::Buffer interp;

    int         animGroup   = 0;
    int         appliedAnim = -1;
    int         hp          = 999;

    bool        placed       = false;  // PlaceAtMe issued for this spawn attempt
    DWORD       phaseReadyMs = 0;      // earliest time to start scanning
    DWORD       spawnedMs    = 0;      // when last PlaceAtMe was enqueued (for timeout)

    std::vector<uint32_t> equip;               // worn base formIDs from peer
    bool        equipDirty   = false;
    float       lastActorHp  = -1.f;           // actual actor HP at last combat poll
    DWORD       hpSuppressUntil = 0;           // ignore drops right after our own setav
};

static std::map<std::string, Ghost> g_ghosts;
static std::mutex                   g_mtx;    // guards g_ghosts only in Shutdown

// PvP mode (server config, from CHAR_LOAD)
static bool g_pvp = false;

// Spawn serialization: at most one PlaceAtMe outstanding at any time, so the
// newest-unclaimed-ref cell scan can never race between two appearing ghosts.
static std::string g_spawnInFlight;  // charId currently spawning ("" = none)

// ── Events from network thread ────────────────────────────────────────────────

enum class EvtType { Appear, Leave, PosUpdate, Equip };
struct Evt {
    EvtType     type;
    std::string charId;
    std::string name;
    uint32_t    raceFormId;
    int         gender;
    float       x, y, z, rotZ;
    int         animGroup;
    int         hp;
    std::vector<uint32_t> items;  // Equip only
};

static std::queue<Evt> g_evtQ;
static std::mutex      g_evtMtx;

static void PushEvt(Evt e) {
    std::lock_guard<std::mutex> lk(g_evtMtx);
    g_evtQ.push(std::move(e));
}

// ── Cell scan: find newest unclaimed ref with given base form ID ──────────────
// TESObjectCELL::ObjectListEntry is a linked list embedded at offset 0x048.
// Each entry: { TESObjectREFR* refr @ +0x000, ObjectListEntry* next @ +0x004 }.

static void* FindUnclaimedRefInCell(uint32_t baseFormId) {
    using namespace Oblivion;

    void* player = *(void**)kPlayerPtr;
    if (!player) return nullptr;

    void* cell = *(void**)((char*)player + kRef_parentCell);
    if (!cell) return nullptr;

    struct OLE { void* refr; OLE* next; };
    OLE* entry = reinterpret_cast<OLE*>((char*)cell + 0x048);

    void*    newest   = nullptr;
    uint32_t newestId = 0;

    while (entry) {
        void* refr = entry->refr;
        if (refr) {
            void* base = *(void**)((char*)refr + kRef_baseForm);
            if (base) {
                uint32_t bid = *(uint32_t*)((char*)base + kForm_refID);
                if (bid == baseFormId) {
                    uint32_t rid = *(uint32_t*)((char*)refr + kForm_refID);
                    if (rid > newestId && g_claimedFids.find(rid) == g_claimedFids.end()) {
                        newestId = rid;
                        newest   = refr;
                    }
                }
            }
        }
        entry = entry->next;
    }
    return newest;
}

// ── Health-in-name ────────────────────────────────────────────────────────────

static std::string NameWithHp(const std::string& name, int hp) {
    if (hp <= 10) return name + " (DYING)";
    if (hp <= 25) return name + " (!!)";
    if (hp <= 75) return name + " (!)";
    return name;
}

// ── Process events (game thread) ──────────────────────────────────────────────

static void DrainEvents() {
    std::queue<Evt> local;
    {
        std::lock_guard<std::mutex> lk(g_evtMtx);
        std::swap(local, g_evtQ);
    }

    while (!local.empty()) {
        Evt ev = std::move(local.front()); local.pop();

        switch (ev.type) {

        case EvtType::Appear: {
            Ghost& gh = g_ghosts[ev.charId];
            if (gh.phase == Phase::Free) {
                int slot = -1;
                for (int i = 0; i < g_numSlots; i++) {
                    if (g_slotFree[i]) { slot = i; g_slotFree[i] = false; break; }
                }
                if (slot < 0) { GS_DBG("no free slot for " + ev.charId); break; }
                gh.slot = slot;
                GS_DBG("Appear charId=" + ev.charId + " slot=" + std::to_string(slot));
            }
            gh.name       = ev.name;
            gh.raceFormId = ev.raceFormId;
            gh.gender     = ev.gender;

            gh.interp.push(GetTickCount(), ev.x, ev.y, ev.z, ev.rotZ);
            gh.animGroup = ev.animGroup;
            gh.phase     = Phase::Spawning;
            gh.placed    = false;
            // PlaceAtMe is issued by TickGhosts once no other spawn is in flight —
            // serializing spawns keeps the newest-unclaimed-ref scan unambiguous.
            break;
        }

        case EvtType::Leave: {
            auto it = g_ghosts.find(ev.charId);
            if (it == g_ghosts.end()) break;
            Ghost& gh = it->second;
            if (gh.slot >= 0) {
                void* ref = g_slotRefs[gh.slot];
                if (ref) {
                    uint32_t fid = *(uint32_t*)((char*)ref + Oblivion::kForm_refID);
                    EnqRef(ref, "disable");
                    EnqRef(ref, "markfordelete");
                    g_claimedFids.erase(fid);
                    g_slotRefs[gh.slot] = nullptr;
                }
                g_slotFree[gh.slot] = true;
                GS_DBG("Leave charId=" + ev.charId + " slot=" + std::to_string(gh.slot));
            }
            if (g_spawnInFlight == ev.charId) g_spawnInFlight.clear();
            g_ghosts.erase(it);
            break;
        }

        case EvtType::PosUpdate: {
            auto it = g_ghosts.find(ev.charId);
            if (it == g_ghosts.end()) break;
            Ghost& gh = it->second;
            gh.interp.push(GetTickCount(), ev.x, ev.y, ev.z, ev.rotZ);
            gh.animGroup = ev.animGroup;
            if (std::abs(ev.hp - gh.hp) > 5) {
                gh.hp = ev.hp;
                if (gh.phase == Phase::Active && gh.slot >= 0 && g_slotRefs[gh.slot]) {
                    void* ref = g_slotRefs[gh.slot];
                    char buf[128];
                    std::string newName = NameWithHp(gh.name, gh.hp);
                    snprintf(buf, sizeof(buf), "SetName \"%s\"", newName.c_str());
                    EnqRef(ref, buf);
                    // Mirror real actor HP so PvP hits land against a true value.
                    // Suppress the hit poll briefly — the setav lands a tick later.
                    if (ev.hp > 0) {
                        snprintf(buf, sizeof(buf), "setav health %d", ev.hp);
                        EnqRef(ref, buf);
                        gh.hpSuppressUntil = GetTickCount() + 1500;
                    }
                }
            }
            break;
        }

        case EvtType::Equip: {
            auto it = g_ghosts.find(ev.charId);
            if (it == g_ghosts.end()) break;
            it->second.equip      = ev.items;
            it->second.equipDirty = true;
            break;
        }

        }
    }
}

// ── Per-frame update (Present hook — game thread) ─────────────────────────────

// Dress a claimed ghost ref in the peer's synced equipment.
// NOTE: additem/equipitem take hex formID args — pending verification that hex
// form literals parse in this compile path (RunCmd FAILED lines will tell).
static void ApplyEquip(Ghost& gh) {
    if (gh.slot < 0 || !g_slotRefs[gh.slot]) return;
    void* ref = g_slotRefs[gh.slot];
    char buf[64];
    EnqRef(ref, "removeallitems");  // strip the guard base outfit first
    for (uint32_t item : gh.equip) {
        snprintf(buf, sizeof(buf), "additem %08X 1", item);
        EnqRef(ref, buf);
        snprintf(buf, sizeof(buf), "equipitem %08X", item);
        EnqRef(ref, buf);
    }
    gh.equipDirty = false;
}

static void TickGhosts() {
    DWORD now      = GetTickCount();
    DWORD renderMs = (now > Interp::DELAY_MS) ? now - Interp::DELAY_MS : 0;

    // Cell object lists mutate during loading screens — never scan or spawn
    // there. Position writes on already-claimed refs are skipped too: the refs
    // may be mid-teardown during a cell transition.
    if (!GameHooks_IsSafeToScan()) return;

    for (auto& [charId, gh] : g_ghosts) {
        if (gh.phase == Phase::Spawning) {
            // Serialize spawns: only the in-flight ghost may place and scan.
            if (g_spawnInFlight.empty() && !gh.placed) {
                uint32_t base = ResolveGhostBase();
                if (!base) continue;  // forms not ready yet — try next frame

                // One-shot parser diagnostics: do hex ref/form literals work
                // in this compile path at all? (prid 14 = player; additem F = 1 gold)
                static bool diagDone = false;
                if (!diagDone) {
                    diagDone = true;
                    EnqCmd("prid 14");
                    EnqCmd("player.additem F 1");
                }

                g_spawnInFlight = charId;
                gh.placed       = true;
                gh.spawnedMs    = now;
                gh.phaseReadyMs = now + SPAWN_WAIT_MS;
                // PlaceAtMe creates an enabled ref near the player in the current cell.
                char buf[64];
                snprintf(buf, sizeof(buf), "player.PlaceAtMe %08X 1", base);
                EnqCmd(buf);
                continue;
            }
            if (g_spawnInFlight != charId) continue;  // queued behind another spawn

            // Wait for PlaceAtMe command to execute and the ref to appear in cell.
            if ((int)(now - gh.phaseReadyMs) < 0) continue;

            // Retry if we've been waiting too long (command may have been missed).
            if (now - gh.spawnedMs > SPAWN_TIMEOUT) {
                char cmdBuf[64];
                snprintf(cmdBuf, sizeof(cmdBuf), "player.PlaceAtMe %08X 1", g_ghostBase);
                // Diagnostic scan: how many refs are in the cell, how many match
                // the guard base, how many are already claimed?
                {
                    using namespace Oblivion;
                    int total = 0, match = 0, claimed = 0;
                    void* player = *(void**)kPlayerPtr;
                    void* cell = player ? *(void**)((char*)player + kRef_parentCell) : nullptr;
                    if (cell) {
                        struct OLE { void* refr; OLE* next; };
                        for (OLE* e = reinterpret_cast<OLE*>((char*)cell + 0x048); e; e = e->next) {
                            if (!e->refr) continue;
                            ++total;
                            void* base = *(void**)((char*)e->refr + kRef_baseForm);
                            if (base && *(uint32_t*)((char*)base + kForm_refID) == g_ghostBase) {
                                ++match;
                                if (g_claimedFids.count(*(uint32_t*)((char*)e->refr + kForm_refID)))
                                    ++claimed;
                            }
                        }
                    }
                    GS_DBG("spawn timeout for " + charId
                           + " — cell=" + std::to_string((uintptr_t)cell)
                           + " refs=" + std::to_string(total)
                           + " guardBase=" + std::to_string(match)
                           + " claimed=" + std::to_string(claimed)
                           + ", retrying PlaceAtMe");
                }
                gh.spawnedMs    = now;
                gh.phaseReadyMs = now + SPAWN_WAIT_MS;
                EnqCmd(cmdBuf);
                continue;
            }

            void* ref = FindUnclaimedRefInCell(g_ghostBase);
            if (!ref) continue;  // not in cell yet, try next frame

            uint32_t fid = *(uint32_t*)((char*)ref + Oblivion::kForm_refID);
            g_slotRefs[gh.slot] = ref;
            g_claimedFids.insert(fid);
            gh.appliedAnim  = -1;
            gh.phase        = Phase::Active;
            g_spawnInFlight.clear();  // next queued ghost may spawn

            char buf[128];
            EnqRef(ref, "setrestrained 1");  // freeze AI so position writes stick
            // PvP servers get hittable ghosts; co-op keeps them intangible.
            EnqRef(ref, g_pvp ? "setghost 0" : "setghost 1");

            // Apply player name with HP indicator (OBSE SetName — per-ref, doesn't affect base form)
            if (!gh.name.empty()) {
                std::string nameStr = NameWithHp(gh.name, gh.hp);
                snprintf(buf, sizeof(buf), "SetName \"%s\"", nameStr.c_str());
                EnqRef(ref, buf);
            }

            // Apply race if we know it (creates a per-ref change record)
            if (gh.raceFormId) {
                const char* raceEd = RaceEditorId(gh.raceFormId);
                if (raceEd) {
                    snprintf(buf, sizeof(buf), "setrace %s", raceEd);
                    EnqRef(ref, buf);
                }
            }

            // Base is typically male — toggle sex for female remote players.
            if (gh.gender == 1) {
                EnqRef(ref, "SexChange");
            }

            // Mirror the peer's real HP onto the actor (needed for PvP hit detection)
            if (gh.hp > 0 && gh.hp != 999) {
                snprintf(buf, sizeof(buf), "setav health %d", gh.hp);
                EnqRef(ref, buf);
            }
            gh.lastActorHp     = -1.f;  // combat poll re-seeds from the live actor
            gh.hpSuppressUntil = now + 1500;

            if (!gh.equip.empty()) ApplyEquip(gh);

            GS_DBG("spawned ghost charId=" + charId
                   + " fid=" + std::to_string(fid)
                   + " slot=" + std::to_string(gh.slot));
            continue;
        }

        if (gh.phase != Phase::Active) continue;
        if (gh.slot < 0 || !g_slotRefs[gh.slot]) continue;

        if (gh.equipDirty) ApplyEquip(gh);

        float x, y, z, rotZ;
        if (!gh.interp.sample(renderMs, x, y, z, rotZ)) continue;

        Interp::WriteRef(g_slotRefs[gh.slot], x, y, z, rotZ);

        if (gh.animGroup != gh.appliedAnim) {
            EnqRef(g_slotRefs[gh.slot],
                   std::string("PlayGroup ") + Oblivion::AnimGroupName(gh.animGroup) + " 1");
            gh.appliedAnim = gh.animGroup;
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

void GhostSystem_Init(int numSlots, GhostCmdFn cmdFn) {
    g_numSlots = numSlots;
    g_cmdFn    = cmdFn;
    for (int i = 0; i < numSlots && i < 32; i++) {
        g_slotFree[i] = true;
        g_slotRefs[i] = nullptr;
    }
    GS_DBG("GhostSystem_Init slots=" + std::to_string(numSlots));
}

void GhostSystem_Shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& [charId, gh] : g_ghosts) {
        if (gh.slot < 0) continue;
        void* ref = g_slotRefs[gh.slot];
        if (ref) {
            uint32_t fid = *(uint32_t*)((char*)ref + Oblivion::kForm_refID);
            EnqRef(ref, "disable");
            EnqRef(ref, "markfordelete");
            g_claimedFids.erase(fid);
            g_slotRefs[gh.slot] = nullptr;
        }
        g_slotFree[gh.slot] = true;
    }
    g_ghosts.clear();
}

void GhostSystem_OnAppear(const std::string& charId, const std::string& charName,
                          uint32_t raceFormId, int gender,
                          float x, float y, float z, float rotZ, int animGroup)
{
    PushEvt({ EvtType::Appear, charId, charName, raceFormId, gender, x, y, z, rotZ, animGroup, 999 });
}

void GhostSystem_OnLeave(const std::string& charId) {
    PushEvt({ EvtType::Leave, charId, {}, 0, 0, 0, 0, 0, 0, 0, 0 });
}

void GhostSystem_OnPosUpdate(const std::string& charId,
                             float x, float y, float z, float rotZ, int animGroup, int hp)
{
    PushEvt({ EvtType::PosUpdate, charId, {}, 0, 0, x, y, z, rotZ, animGroup, hp });
}

void GhostSystem_OnFrame() {
    DrainEvents();
    TickGhosts();
}

bool GhostSystem_IsGhostRef(uint32_t formId) {
    return g_claimedFids.count(formId) != 0;
}

void GhostSystem_SetPvp(bool enabled) {
    g_pvp = enabled;
}

void GhostSystem_OnEquip(const std::string& charId, const uint32_t* items, int count) {
    Evt e{};
    e.type   = EvtType::Equip;
    e.charId = charId;
    e.items.assign(items, items + count);
    PushEvt(std::move(e));
}

// getAV vtable dispatch — slot 0xA1, AV code 8 = Health (same as npc_sync).
static float GhostActorHp(void* actor) {
    typedef float(__fastcall* GetAVFn)(void*, void*, unsigned int);
    GetAVFn fn = ((GetAVFn*)(*(void***)actor))[0xA1];
    return fn(actor, nullptr, 8);
}

int GhostSystem_PollHits(GhostHit* out, int max) {
    if (!g_pvp || max <= 0) return 0;
    DWORD now = GetTickCount();
    int n = 0;

    for (auto& [charId, gh] : g_ghosts) {
        if (n >= max) break;
        if (gh.phase != Phase::Active || gh.slot < 0 || !g_slotRefs[gh.slot]) continue;

        float cur = GhostActorHp(g_slotRefs[gh.slot]);

        // Right after our own setav (spawn or server HP update) the actor value
        // lags the queued command — resync without reporting.
        if (gh.lastActorHp < 0.f || (int)(gh.hpSuppressUntil - now) > 0) {
            gh.lastActorHp = cur;
            continue;
        }

        if (cur < gh.lastActorHp - 0.5f) {
            int amount = (int)(gh.lastActorHp - cur + 0.5f);
            snprintf(out[n].charId, sizeof(out[n].charId), "%s", charId.c_str());
            out[n].amount = amount;
            ++n;
        }
        gh.lastActorHp = cur;
    }
    return n;
}
