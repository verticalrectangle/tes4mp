#include "ghost_system.h"
#include "oblivion_internal.h"
#include <windows.h>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <queue>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstdint>

static void GS_DBG(const std::string& s) {
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << "[ghost] " << s << "\n";
}

// ── Config ─────────────────────────────────────────────────────────────────────

static constexpr int     SNAP_CAP       = 8;          // ring buffer size per ghost
static constexpr DWORD   INTERP_DELAY   = 120;        // ms behind live for interpolation
static constexpr DWORD   SPAWN_WAIT_MS  = 500;        // ms after PlaceAtMe before scanning
static constexpr DWORD   SPAWN_TIMEOUT  = 3000;       // ms — retry PlaceAtMe if scan fails
static constexpr uint32_t GHOST_BASE_NPC = 0x0001C34F; // vanilla Imperial Watch guard (Oblivion.esm)

// ── Slot pool ─────────────────────────────────────────────────────────────────

static int        g_numSlots    = 0;
static GhostCmdFn g_cmdFn       = nullptr;
static bool       g_slotFree[32] = {};
static void*      g_slotRefs[32] = {};  // TESObjectREFR* per slot, filled by cell scan

static void EnqCmd(const std::string& s) {
    if (g_cmdFn) g_cmdFn(s.c_str());
}

// ── Claimed ref tracking ──────────────────────────────────────────────────────
// Prevents two ghosts appearing simultaneously from claiming the same newly placed ref.

static std::set<uint32_t> g_claimedFids;

// ── Snapshot ring buffer ──────────────────────────────────────────────────────

struct Snap {
    DWORD ms;
    float x, y, z, rotZ;
};

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

    Snap        buf[SNAP_CAP] = {};
    int         head          = 0;
    int         count         = 0;

    int         animGroup   = 0;
    int         appliedAnim = -1;

    DWORD       phaseReadyMs = 0;  // earliest time to start scanning
    DWORD       spawnedMs    = 0;  // when last PlaceAtMe was enqueued (for timeout)
};

static std::map<std::string, Ghost> g_ghosts;
static std::mutex                   g_mtx;    // guards g_ghosts only in Shutdown

// ── Events from network thread ────────────────────────────────────────────────

enum class EvtType { Appear, Leave, PosUpdate };
struct Evt {
    EvtType     type;
    std::string charId;
    std::string name;
    uint32_t    raceFormId;
    int         gender;
    float       x, y, z, rotZ;
    int         animGroup;
};

static std::queue<Evt> g_evtQ;
static std::mutex      g_evtMtx;

static void PushEvt(Evt e) {
    std::lock_guard<std::mutex> lk(g_evtMtx);
    g_evtQ.push(std::move(e));
}

// ── Write ghost ref position (game thread only) ───────────────────────────────

static void WriteRef(void* ref, float x, float y, float z, float rotZ) {
    using namespace Oblivion;
    auto* r = static_cast<char*>(ref);
    *reinterpret_cast<float*>(r + kRef_posX) = x;
    *reinterpret_cast<float*>(r + kRef_posY) = y;
    *reinterpret_cast<float*>(r + kRef_posZ) = z;
    *reinterpret_cast<float*>(r + kRef_rotZ) = rotZ;

    void* ni = *reinterpret_cast<void**>(r + kRef_niNode);
    if (ni) {
        auto* n = static_cast<char*>(ni);
        *reinterpret_cast<float*>(n + kNi_worldTransX) = x;
        *reinterpret_cast<float*>(n + kNi_worldTransY) = y;
        *reinterpret_cast<float*>(n + kNi_worldTransZ) = z;
        *reinterpret_cast<float*>(n + kNi_boundCtrX)   = x;
        *reinterpret_cast<float*>(n + kNi_boundCtrY)   = y;
        *reinterpret_cast<float*>(n + kNi_boundCtrZ)   = z;
    }
}

// ── Push snapshot ─────────────────────────────────────────────────────────────

static void PushSnap(Ghost& g, float x, float y, float z, float rotZ) {
    Snap& s = g.buf[g.head];
    s.ms   = GetTickCount();
    s.x = x; s.y = y; s.z = z; s.rotZ = rotZ;
    g.head = (g.head + 1) % SNAP_CAP;
    if (g.count < SNAP_CAP) g.count++;
}

// ── Interpolate position at renderTime ───────────────────────────────────────

static bool InterpPos(const Ghost& g, DWORD renderMs,
                      float& ox, float& oy, float& oz, float& orot)
{
    if (g.count == 0) return false;

    int newest = (g.head + SNAP_CAP - 1) % SNAP_CAP;
    int oldest = (g.head + SNAP_CAP - g.count) % SNAP_CAP;

    const Snap& sNew = g.buf[newest];
    if (renderMs >= sNew.ms || g.count == 1) {
        ox = sNew.x; oy = sNew.y; oz = sNew.z; orot = sNew.rotZ;
        return true;
    }

    int prevIdx = oldest;
    for (int i = 1; i < g.count; i++) {
        int curIdx = (oldest + i) % SNAP_CAP;
        const Snap& sa = g.buf[prevIdx];
        const Snap& sb = g.buf[curIdx];
        if (sa.ms <= renderMs && renderMs <= sb.ms) {
            float t = (sb.ms == sa.ms) ? 0.f
                      : (float)(renderMs - sa.ms) / (float)(sb.ms - sa.ms);
            ox   = sa.x    + t * (sb.x    - sa.x);
            oy   = sa.y    + t * (sb.y    - sa.y);
            oz   = sa.z    + t * (sb.z    - sa.z);
            orot = sa.rotZ + t * (sb.rotZ - sa.rotZ);
            return true;
        }
        prevIdx = curIdx;
    }

    const Snap& sOld = g.buf[oldest];
    ox = sOld.x; oy = sOld.y; oz = sOld.z; orot = sOld.rotZ;
    return true;
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

// ── Process events (game thread) ──────────────────────────────────────────────

static void DrainEvents() {
    std::queue<Evt> local;
    {
        std::lock_guard<std::mutex> lk(g_evtMtx);
        std::swap(local, g_evtQ);
    }

    DWORD now = GetTickCount();

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

            PushSnap(gh, ev.x, ev.y, ev.z, ev.rotZ);
            gh.animGroup    = ev.animGroup;
            gh.phase        = Phase::Spawning;
            gh.spawnedMs    = now;
            gh.phaseReadyMs = now + SPAWN_WAIT_MS;

            // PlaceAtMe creates an enabled ref near the player in the current cell.
            EnqCmd("player.PlaceAtMe 1C34F 1");
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
                    char buf[64];
                    snprintf(buf, sizeof(buf), "prid %08X", fid);
                    EnqCmd(buf);
                    EnqCmd("disable");
                    EnqCmd("markfordelete");
                    g_claimedFids.erase(fid);
                    g_slotRefs[gh.slot] = nullptr;
                }
                g_slotFree[gh.slot] = true;
                GS_DBG("Leave charId=" + ev.charId + " slot=" + std::to_string(gh.slot));
            }
            g_ghosts.erase(it);
            break;
        }

        case EvtType::PosUpdate: {
            auto it = g_ghosts.find(ev.charId);
            if (it == g_ghosts.end()) break;
            Ghost& gh = it->second;
            PushSnap(gh, ev.x, ev.y, ev.z, ev.rotZ);
            gh.animGroup = ev.animGroup;
            break;
        }

        }
    }
}

// ── Per-frame update (Present hook — game thread) ─────────────────────────────

static void TickGhosts() {
    DWORD now      = GetTickCount();
    DWORD renderMs = (now > INTERP_DELAY) ? now - INTERP_DELAY : 0;

    for (auto& [charId, gh] : g_ghosts) {
        if (gh.phase == Phase::Spawning) {
            // Wait for PlaceAtMe command to execute and the ref to appear in cell.
            if ((int)(now - gh.phaseReadyMs) < 0) continue;

            // Retry if we've been waiting too long (command may have been missed).
            if (now - gh.spawnedMs > SPAWN_TIMEOUT) {
                GS_DBG("spawn timeout for " + charId + ", retrying PlaceAtMe");
                gh.spawnedMs    = now;
                gh.phaseReadyMs = now + SPAWN_WAIT_MS;
                EnqCmd("player.PlaceAtMe 1C34F 1");
                continue;
            }

            void* ref = FindUnclaimedRefInCell(GHOST_BASE_NPC);
            if (!ref) continue;  // not in cell yet, try next frame

            uint32_t fid = *(uint32_t*)((char*)ref + Oblivion::kForm_refID);
            g_slotRefs[gh.slot] = ref;
            g_claimedFids.insert(fid);
            gh.appliedAnim = -1;
            gh.phase       = Phase::Active;

            char buf[128];
            snprintf(buf, sizeof(buf), "prid %08X", fid);
            EnqCmd(buf);
            EnqCmd("setrestrained 1");  // freeze AI so position writes stick

            // Apply player name (OBSE SetName — per-ref, doesn't affect base form)
            if (!gh.name.empty()) {
                snprintf(buf, sizeof(buf), "SetName \"%s\"", gh.name.c_str());
                EnqCmd(buf);
            }

            // Apply race if we know it (creates a per-ref change record)
            if (gh.raceFormId) {
                const char* raceEd = RaceEditorId(gh.raceFormId);
                if (raceEd) {
                    snprintf(buf, sizeof(buf), "setrace %s", raceEd);
                    EnqCmd(buf);
                }
            }

            GS_DBG("spawned ghost charId=" + charId
                   + " fid=" + std::to_string(fid)
                   + " slot=" + std::to_string(gh.slot));
            continue;
        }

        if (gh.phase != Phase::Active) continue;
        if (gh.slot < 0 || !g_slotRefs[gh.slot]) continue;

        float x, y, z, rotZ;
        if (!InterpPos(gh, renderMs, x, y, z, rotZ)) continue;

        WriteRef(g_slotRefs[gh.slot], x, y, z, rotZ);

        if (gh.animGroup != gh.appliedAnim) {
            uint32_t fid = *(uint32_t*)((char*)g_slotRefs[gh.slot] + Oblivion::kForm_refID);
            char buf[64];
            snprintf(buf, sizeof(buf), "prid %08X", fid);
            EnqCmd(buf);
            EnqCmd(std::string("PlayGroup ") + Oblivion::AnimGroupName(gh.animGroup) + " 1");
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
            char buf[64];
            snprintf(buf, sizeof(buf), "prid %08X", fid);
            EnqCmd(buf);
            EnqCmd("disable");
            EnqCmd("markfordelete");
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
    PushEvt({ EvtType::Appear, charId, charName, raceFormId, gender, x, y, z, rotZ, animGroup });
}

void GhostSystem_OnLeave(const std::string& charId) {
    PushEvt({ EvtType::Leave, charId, {}, 0, 0, 0, 0, 0, 0, 0 });
}

void GhostSystem_OnPosUpdate(const std::string& charId,
                             float x, float y, float z, float rotZ, int animGroup)
{
    PushEvt({ EvtType::PosUpdate, charId, {}, 0, 0, x, y, z, rotZ, animGroup });
}

void GhostSystem_OnFrame() {
    DrainEvents();
    TickGhosts();
}
