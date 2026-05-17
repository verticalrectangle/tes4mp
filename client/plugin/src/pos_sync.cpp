#include "pos_sync.h"
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
    f << msg << "\n";
}

// ── Oblivion 1.2.416 memory offsets ──────────────────────────────────────────
// Player TESObjectREFR* is at a static global pointer.
// All offsets are byte offsets from the TESObjectREFR* (or cell*).

static constexpr uintptr_t ADDR_PLAYER_PTR  = 0x00B333C4; // TESObjectREFR**

// TESObjectREFR fields (verified against existing code and OBSE headers)
static constexpr ptrdiff_t OFF_REF_NINODE   = 0x008; // NiNode* — 3D scene node
static constexpr ptrdiff_t OFF_REF_BASE     = 0x01C; // TESForm* baseForm
static constexpr ptrdiff_t OFF_REF_POS_X    = 0x02C; // float posX (verified)
static constexpr ptrdiff_t OFF_REF_POS_Y    = 0x030; // float posY (verified)
static constexpr ptrdiff_t OFF_REF_POS_Z    = 0x034; // float posZ (verified)
static constexpr ptrdiff_t OFF_REF_ROT_Z    = 0x040; // float rotZ heading
static constexpr ptrdiff_t OFF_REF_CELL     = 0x048; // TESObjectCELL*

// TESForm::refID (formID) — verified from OBSE GameForms.h TESForm layout.
// +0x004=typeID, +0x008=flags, +0x00C=refID. Prior code used 0x008 (flags) by mistake.
static constexpr ptrdiff_t OFF_FORM_ID      = 0x00C;

// ── Local player snapshot ─────────────────────────────────────────────────────

static std::mutex    g_localMtx;
static PlayerState   g_local = {};

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
    s.valid      = true;
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

static void PollThread() {
    PS_DBG("PosSync PollThread start");
    PlayerState last = {};

    while (g_running) {
        PS_DBG("PosSync SamplePlayer");
        PlayerState now = SamplePlayer();
        PS_DBG("PosSync SamplePlayer done");

        if (now.valid) {
            PS_DBG("PosSync update g_local");
            {
                std::lock_guard<std::mutex> lk(g_localMtx);
                g_local = now;
            }
            PS_DBG("PosSync check connected");
            if (g_network.isConnected()) {
                float dx   = now.x - last.x;
                float dy   = now.y - last.y;
                float dz   = now.z - last.z;
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                float dr   = std::abs(now.rotZ - last.rotZ);

                if (dist > POS_DEAD_BAND || dr > ROT_DEAD_BAND
                        || now.cellFormID != last.cellFormID) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "{\"type\":\"POSITION_UPDATE\","
                        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                        "\"rot\":%.4f,\"cell\":%u}",
                        now.x, now.y, now.z, now.rotZ, now.cellFormID);
                    PS_DBG("PosSync sending");
                    g_network.send(buf);
                    PS_DBG("PosSync sent");
                    last = now;
                }
            }
        }

        PS_DBG("PosSync sleep");
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
