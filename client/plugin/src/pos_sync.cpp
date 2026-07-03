#include "pos_sync.h"
#include "oblivion_internal.h"
#include "network.h"
#include "game_hooks.h"
#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <fstream>

static void PS_DBG(const char* msg) {
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << "[pos_sync] " << msg << "\n";
}

// ── Oblivion 1.2.416 memory offsets ──────────────────────────────────────────
// All offsets confirmed from GameObjects.h (see oblivion_internal.h).

static constexpr uintptr_t ADDR_PLAYER_PTR = Oblivion::kPlayerPtr;
static constexpr ptrdiff_t OFF_REF_POS_X   = Oblivion::kRef_posX;
static constexpr ptrdiff_t OFF_REF_POS_Y   = Oblivion::kRef_posY;
static constexpr ptrdiff_t OFF_REF_POS_Z   = Oblivion::kRef_posZ;
static constexpr ptrdiff_t OFF_REF_ROT_Z   = Oblivion::kRef_rotZ;   // 0x028 (was wrong 0x040)
static constexpr ptrdiff_t OFF_REF_CELL    = Oblivion::kRef_parentCell; // 0x040

// TESForm::refID
static constexpr ptrdiff_t OFF_FORM_ID     = Oblivion::kForm_refID; // 0x00C

// ── Local player snapshot ─────────────────────────────────────────────────────

static std::mutex    g_localMtx;
static PlayerState   g_local = {};

// TESObjectCELL offsets (from GameForms.h)
static constexpr ptrdiff_t OFF_CELL_FLAGS0      = 0x024; // UInt8 — bit 0 = interior
static constexpr ptrdiff_t OFF_CELL_WORLDSPACE  = 0x050; // TESWorldSpace*

static PlayerState SamplePlayer() {
    PlayerState s = {};
    void* player = *(void**)ADDR_PLAYER_PTR;
    if (!player) return s;
    void* cell = *(void**)((char*)player + OFF_REF_CELL);
    s.x          = *(float*)((char*)player + OFF_REF_POS_X);
    s.y          = *(float*)((char*)player + OFF_REF_POS_Y);
    s.z          = *(float*)((char*)player + OFF_REF_POS_Z);
    s.rotZ       = *(float*)((char*)player + OFF_REF_ROT_Z);
    s.cellFormID = cell ? *(uint32_t*)((char*)cell + OFF_FORM_ID) : 0;

    // Worldspace: 0 for interior cells, non-zero for exterior (e.g. Tamriel = 0x3C)
    if (cell) {
        uint8_t flags0 = *(uint8_t*)((char*)cell + OFF_CELL_FLAGS0);
        bool interior  = (flags0 & 0x01) != 0;
        if (!interior) {
            void* ws = *(void**)((char*)cell + OFF_CELL_WORLDSPACE);
            s.worldspaceFormID = ws ? *(uint32_t*)((char*)ws + OFF_FORM_ID) : 0;
        }
    }

    s.valid = true;
    return s;
}

PlayerState PosSync_GetLocal() {
    std::lock_guard<std::mutex> lk(g_localMtx);
    return g_local;
}

// ── Poll thread ───────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{false};
static std::thread       g_thread;

static constexpr float POS_DEAD_BAND = 0.5f;   // units — skip send if smaller
static constexpr float ROT_DEAD_BAND = 0.02f;  // radians

// While moving continuously the dead-band passes every 10ms poll — that's
// ~100 packets/s, which overflows the ghost interp buffer on the other end
// (8 snaps = 80ms < the 120ms render delay) and reads as jitter. 15 Hz is
// plenty for smooth interpolation.
static constexpr DWORD SEND_MIN_INTERVAL_MS = 66;

// Classify movement as an Oblivion AnimGroup code based on speed/rotation.
static int ClassifyAnim(const PlayerState& now, const PlayerState& last, float dtSec) {
    float dx    = now.x - last.x;
    float dy    = now.y - last.y;
    float dist  = std::sqrt(dx*dx + dy*dy);
    float speed = (dtSec > 0.f) ? dist / dtSec : 0.f;
    float drot  = (dtSec > 0.f) ? (now.rotZ - last.rotZ) / dtSec : 0.f;

    if (speed >= Oblivion::kSpeed_Run)  return Oblivion::kAnim_FastForward;
    if (speed >= Oblivion::kSpeed_Walk) return Oblivion::kAnim_Forward;
    if (std::abs(drot) > 0.5f)
        return (drot > 0.f) ? Oblivion::kAnim_TurnLeft : Oblivion::kAnim_TurnRight;
    return Oblivion::kAnim_Idle;
}

static void PollThread() {
    PS_DBG("PosSync PollThread start");
    PlayerState last    = {};
    DWORD       lastMs  = GetTickCount();
    int         lastAnim = Oblivion::kAnim_Idle;

    while (g_running) {
        PlayerState now = SamplePlayer();
        DWORD nowMs = GetTickCount();

        // Loading screens can yield NaN/Inf position reads — treat as no sample.
        if (now.valid && !(std::isfinite(now.x) && std::isfinite(now.y)
                           && std::isfinite(now.z) && std::isfinite(now.rotZ)))
            now.valid = false;

        if (now.valid) {
            {
                std::lock_guard<std::mutex> lk(g_localMtx);
                g_local = now;
            }
            if (g_network.isConnected()) {
                float dx   = now.x - last.x;
                float dy   = now.y - last.y;
                float dz   = now.z - last.z;
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                float dr   = std::abs(now.rotZ - last.rotZ);

                float dtSec = (float)(nowMs - lastMs) / 1000.f;
                int   anim  = ClassifyAnim(now, last, dtSec);

                // Detect fast travel: cell changed AND 2D jump > 3000 units in one 10ms tick
                // (max normal run speed ~600 u/s = 6 u/tick — 3000 is impossible without FT)
                bool cellChanged = (now.cellFormID != last.cellFormID
                                    || now.worldspaceFormID != last.worldspaceFormID);
                bool isFastTravel = false;
                char cellName[64] = {};
                if (cellChanged && last.valid) {
                    float hd = std::sqrt(dx*dx + dy*dy);
                    if (hd > 3000.f) {
                        isFastTravel = true;
                        // Read cell name: TESObjectCELL TESFullName mixin at cell+0x018,
                        // BSString.m_data char* at +0x004 → cell+0x01C
                        void* player = *(void**)Oblivion::kPlayerPtr;
                        if (player) {
                            void* cell = *(void**)((char*)player + Oblivion::kRef_parentCell);
                            if (cell && now.worldspaceFormID == 0) {
                                // Interior: read name from TESFullName mixin
                                const char* nm = *(const char**)((char*)cell + 0x01C);
                                if (nm) {
                                    snprintf(cellName, sizeof(cellName), "%s", nm);
                                }
                            } else {
                                snprintf(cellName, sizeof(cellName), "Tamriel");
                            }
                        }
                    }
                }

                bool rateOk = (nowMs - lastMs) >= SEND_MIN_INTERVAL_MS;
                if ((rateOk && (dist > POS_DEAD_BAND || dr > ROT_DEAD_BAND))
                        || cellChanged || anim != lastAnim) {
                    char buf[280];
                    if (isFastTravel && cellName[0]) {
                        snprintf(buf, sizeof(buf),
                            "{\"type\":\"POSITION_UPDATE\","
                            "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                            "\"rot\":%.4f,\"cell\":%u,\"ws\":%u,\"anim\":%d,\"hp\":%d,"
                            "\"ft\":1,\"cn\":\"%s\"}",
                            now.x, now.y, now.z, now.rotZ,
                            now.cellFormID, now.worldspaceFormID, anim,
                            GameHooks_GetPlayerHp(), cellName);
                    } else {
                        snprintf(buf, sizeof(buf),
                            "{\"type\":\"POSITION_UPDATE\","
                            "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                            "\"rot\":%.4f,\"cell\":%u,\"ws\":%u,\"anim\":%d,\"hp\":%d}",
                            now.x, now.y, now.z, now.rotZ,
                            now.cellFormID, now.worldspaceFormID, anim,
                            GameHooks_GetPlayerHp());
                    }
                    g_network.send(buf);
                    last     = now;
                    lastMs   = nowMs;
                    lastAnim = anim;
                }
            }
        }

        Sleep(10);
    }
}

void PosSync_Start() {
    g_running = true;
    g_thread  = std::thread(PollThread);
}

void PosSync_Stop() {
    g_running = false;
    if (g_thread.joinable()) g_thread.join();
}
