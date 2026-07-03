#pragma once
#include <string>
#include <cstdint>

// WP9: dynamic spawn mirroring, "shared fate" model. The cell authority
// broadcasts its rolled dynamic actors; followers suppress their own rolls
// and host replicas driven by local AI. See docs/PLAN.md.

struct SpawnEntry { uint32_t sid; uint32_t base; float x, y, z; int hp; };

// Called from NpcSync_Tick (game thread, scan-safe, cell-settled).
void NpcSpawnSync_Tick(const std::string& cellKey, bool isAuthority);

// Queue an NPC_SPAWNS snapshot from the server (network thread).
void NpcSpawnSync_OnSnapshot(const SpawnEntry* entries, int count);

// True if refId is one of our replicas — other scanners must ignore it.
bool NpcSpawnSync_IsReplica(uint32_t refId);
