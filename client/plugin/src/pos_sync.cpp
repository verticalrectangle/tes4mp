#include "pos_sync.h"
#include "oblivion_internal.h"
#include "network.h"
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

                if (dist > POS_DEAD_BAND || dr > ROT_DEAD_BAND
                        || now.cellFormID != last.cellFormID
                        || now.worldspaceFormID != last.worldspaceFormID
                        || anim != lastAnim) {
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                        "{\"type\":\"POSITION_UPDATE\","
                        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                        "\"rot\":%.4f,\"cell\":%u,\"ws\":%u,\"anim\":%d}",
                        now.x, now.y, now.z, now.rotZ,
                        now.cellFormID, now.worldspaceFormID, anim);
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
