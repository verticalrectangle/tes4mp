#include "ghost_system.h"
#include "oblivion_internal.h"
#include <windows.h>
#include <string>
#include <map>
#include <mutex>
#include <queue>
#include <cmath>
#include <fstream>

static void GS_DBG(const std::string& s) {
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << "[ghost] " << s << "\n";
}

// ── Config ─────────────────────────────────────────────────────────────────────

static constexpr int   SNAP_CAP      = 8;    // ring buffer size per ghost
static constexpr DWORD INTERP_DELAY  = 120;  // ms behind live — ensures two snaps to lerp

// ── Slot pool ─────────────────────────────────────────────────────────────────

static int      g_numSlots = 0;
static void**   g_refPtrs  = nullptr;
static GhostCmdFn g_cmdFn  = nullptr;
static bool     g_slotFree[32] = {};

static void EnqCmd(const std::string& s) {
    if (g_cmdFn) g_cmdFn(s.c_str());
}

static std::string GhostEdId(int slot) {
    return "TES4MPGhostACHR0" + std::to_string(slot + 1);
}

// ── Snapshot ring buffer ──────────────────────────────────────────────────────

struct Snap {
    DWORD ms;
    float x, y, z, rotZ;
};

// ── Ghost state ───────────────────────────────────────────────────────────────

enum class Phase { Free, Queued, Active };

struct Ghost {
    Phase       phase     = Phase::Free;
    int         slot      = -1;
    std::string name;

    // Interpolation buffer
    Snap        buf[SNAP_CAP] = {};
    int         head  = 0;   // next write index
    int         count = 0;

    // Animation
    int         animGroup     = 0;
    int         appliedAnim   = -1;  // last anim sent to engine
    DWORD       phaseReadyMs  = 0;   // when Queued→Active transition allowed
};

static std::map<std::string, Ghost> g_ghosts;
static std::mutex                   g_mtx;

// ── Events from network thread ────────────────────────────────────────────────

enum class EvtType { Appear, Leave, PosUpdate };
struct Evt {
    EvtType     type;
    std::string charId;
    std::string name;
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

    // Also update the NiNode world transform and bound centre so the renderer
    // doesn't have to wait for the scene-graph update pass next frame.
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
    s.x    = x; s.y = y; s.z = z; s.rotZ = rotZ;
    g.head = (g.head + 1) % SNAP_CAP;
    if (g.count < SNAP_CAP) g.count++;
}

// ── Interpolate position at renderTime ───────────────────────────────────────

static bool InterpPos(const Ghost& g, DWORD renderMs,
                      float& ox, float& oy, float& oz, float& orot)
{
    if (g.count == 0) return false;

    // Find the two snapshots that bracket renderMs.
    // Snapshots are stored newest-first (head-1 is newest).
    // We want: snap_a.ms <= renderMs <= snap_b.ms
    // i.e. a is older, b is newer. Lerp from a toward b.

    int newest = (g.head + SNAP_CAP - 1) % SNAP_CAP;
    int oldest = (g.head + SNAP_CAP - g.count) % SNAP_CAP;

    // If renderMs is beyond our newest snap, clamp to newest.
    const Snap& sNew = g.buf[newest];
    if (renderMs >= sNew.ms || g.count == 1) {
        ox = sNew.x; oy = sNew.y; oz = sNew.z; orot = sNew.rotZ;
        return true;
    }

    // Walk buffer to find bracketing pair.
    // Indices in arrival order: oldest, ..., newest
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

    // renderMs is older than all our snaps — clamp to oldest.
    const Snap& sOld = g.buf[oldest];
    ox = sOld.x; oy = sOld.y; oz = sOld.z; orot = sOld.rotZ;
    return true;
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
                // Assign a free slot
                int slot = -1;
                for (int i = 0; i < g_numSlots; i++) {
                    if (g_slotFree[i]) { slot = i; g_slotFree[i] = false; break; }
                }
                if (slot < 0) { GS_DBG("no free slot for " + ev.charId); break; }
                gh.slot = slot;
                gh.name = ev.name;
                GS_DBG("Appear charId=" + ev.charId + " slot=" + std::to_string(slot));
            }

            PushSnap(gh, ev.x, ev.y, ev.z, ev.rotZ);
            gh.animGroup = ev.animGroup;
            gh.phase = Phase::Queued;
            gh.phaseReadyMs = now + 500; // wait 500ms for enable+moveto to settle

            std::string edId = GhostEdId(gh.slot);
            EnqCmd("prid " + edId);
            EnqCmd("enable");
            EnqCmd("moveto player");
            EnqCmd("setrestrained 1");  // freeze AI so position writes stick
            break;
        }

        case EvtType::Leave: {
            auto it = g_ghosts.find(ev.charId);
            if (it == g_ghosts.end()) break;
            Ghost& gh = it->second;
            if (gh.slot >= 0) {
                EnqCmd("prid " + GhostEdId(gh.slot));
                EnqCmd("disable");
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
            if (gh.phase == Phase::Queued && (int)(now - gh.phaseReadyMs) >= 0)
                gh.phase = Phase::Active;
            break;
        }

        }
    }
}

// ── Per-frame update (Present hook — game thread) ─────────────────────────────

static void TickGhosts() {
    DWORD now       = GetTickCount();
    DWORD renderMs  = (now > INTERP_DELAY) ? now - INTERP_DELAY : 0;

    for (auto& [charId, gh] : g_ghosts) {
        if (gh.phase != Phase::Active) continue;
        if (gh.slot < 0 || !g_refPtrs[gh.slot]) continue;

        float x, y, z, rotZ;
        if (!InterpPos(gh, renderMs, x, y, z, rotZ)) continue;

        WriteRef(g_refPtrs[gh.slot], x, y, z, rotZ);

        // Anim group transitions
        if (gh.animGroup != gh.appliedAnim) {
            std::string edId = GhostEdId(gh.slot);
            const char* gname = Oblivion::AnimGroupName(gh.animGroup);
            EnqCmd("prid " + edId);
            EnqCmd(std::string("PlayGroup ") + gname + " 1");
            gh.appliedAnim = gh.animGroup;
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

void GhostSystem_Init(int numSlots, void** refPtrs, GhostCmdFn cmdFn) {
    g_numSlots = numSlots;
    g_refPtrs  = refPtrs;
    g_cmdFn    = cmdFn;
    for (int i = 0; i < numSlots && i < 32; i++)
        g_slotFree[i] = true;
    GS_DBG("GhostSystem_Init slots=" + std::to_string(numSlots));
}

void GhostSystem_Shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& [charId, gh] : g_ghosts) {
        if (gh.slot >= 0) {
            EnqCmd("prid " + GhostEdId(gh.slot));
            EnqCmd("disable");
            if (gh.slot < 32) g_slotFree[gh.slot] = true;
        }
    }
    g_ghosts.clear();
}

void GhostSystem_OnAppear(const std::string& charId, const std::string& charName,
                          float x, float y, float z, float rotZ, int animGroup)
{
    PushEvt({ EvtType::Appear, charId, charName, x, y, z, rotZ, animGroup });
}

void GhostSystem_OnLeave(const std::string& charId) {
    PushEvt({ EvtType::Leave, charId, {}, 0, 0, 0, 0, 0 });
}

void GhostSystem_OnPosUpdate(const std::string& charId,
                             float x, float y, float z, float rotZ, int animGroup)
{
    PushEvt({ EvtType::PosUpdate, charId, {}, x, y, z, rotZ, animGroup });
}

void GhostSystem_OnFrame() {
    DrainEvents();
    TickGhosts();
}
