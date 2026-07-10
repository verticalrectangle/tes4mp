#include "npc_puppet.h"
#include "npc_spawn_sync.h"
#include "interp.h"
#include "game_hooks.h"
#include "oblivion_internal.h"
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include <vector>

// Stream silence (ms) after which a puppet reverts to local AI. Doubles as
// authority-handover cleanup: the new authority streams different keys, the
// stale ones age out here.
static constexpr DWORD RELEASE_AFTER_MS = 3000;

struct Puppet {
    void*          ref       = nullptr;
    Interp::Buffer interp;
    DWORD          lastPosMs = 0;
    bool           restrained = false;
};

// One mutex guards the map: pushed from the network thread, managed on the
// game tick, sampled in the Present hook. Entries are few (≤8 streamed).
static std::mutex                            g_mtx;
static std::unordered_map<uint32_t, Puppet>  g_puppets;
static std::string                           g_cell;

void NpcPuppet_OnPos(uint32_t key, float x, float y, float z, float rot) {
    DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_mtx);
    Puppet& p = g_puppets[key];       // ref resolved on the game tick
    p.interp.push(now, x, y, z, rot);
    p.lastPosMs = now;
}

static void ReleaseAllLocked() {
    for (auto& [key, p] : g_puppets)
        if (p.ref && p.restrained)
            GameHooks_EnqueueCmdOnRef(p.ref, "setrestrained 0");
    g_puppets.clear();
}

void NpcPuppet_Tick(const std::string& cellKey, bool isAuthority) {
    std::lock_guard<std::mutex> lk(g_mtx);

    if (cellKey != g_cell) {
        // Old cell's refs are gone — no un-restrain possible (or needed).
        g_cell = cellKey;
        g_puppets.clear();
        return;
    }

    if (isAuthority) {
        // We stream now; everything we mirror runs live AI on our screen.
        ReleaseAllLocked();
        return;
    }

    DWORD now = GetTickCount();
    for (auto it = g_puppets.begin(); it != g_puppets.end();) {
        Puppet& p = it->second;

        if (now - p.lastPosMs > RELEASE_AFTER_MS) {
            if (p.ref && p.restrained)
                GameHooks_EnqueueCmdOnRef(p.ref, "setrestrained 0");
            it = g_puppets.erase(it);
            continue;
        }

        if (!p.ref) {
            uint32_t key = it->first;
            // Dynamic keys are sids — translate to our local replica ref.
            // Static keys are shared refIDs — engine lookup (game thread only).
            p.ref = (key >= 0xFF000000u)
                        ? NpcSpawnSync_GetReplicaRef(key)
                        : Oblivion::LookupFormByID(key);
        }
        if (p.ref && !p.restrained) {
            GameHooks_EnqueueCmdOnRef(p.ref, "setrestrained 1");
            p.restrained = true;
        }
        ++it;
    }
}

void NpcPuppet_OnFrame() {
    if (!GameHooks_IsSafeToScan()) return;

    DWORD now      = GetTickCount();
    DWORD renderMs = (now > Interp::DELAY_MS) ? now - Interp::DELAY_MS : 0;

    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& [key, p] : g_puppets) {
        if (!p.ref || !p.restrained) continue;
        float x, y, z, rot;
        if (p.interp.sample(renderMs, x, y, z, rot))
            Interp::WriteRef(p.ref, x, y, z, rot);
    }
}
