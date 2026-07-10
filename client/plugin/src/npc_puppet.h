#pragma once
#include <string>
#include <cstdint>

// WP13: follower-side NPC puppet mirroring. When the cell authority streams
// NPC_POS for an actor, we put that actor in puppet mode (setrestrained 1 —
// local AI off, still hittable; NEVER setghost) and drive its position from
// the shared interpolation buffer every rendered frame, exactly like player
// ghosts. When the stream for an actor goes silent (left combat/range on the
// authority, authority handover), the puppet is released back to local AI.

// Queue a position sample from the network thread.
// key = static refID, or sid (authority-local refID) for dynamic replicas.
void NpcPuppet_OnPos(uint32_t key, float x, float y, float z, float rot);

// Game-thread tick (from NpcSync_Tick — scan-safe, cell-settled): resolves
// refs, applies/releases restrained state, drops stale puppets.
void NpcPuppet_Tick(const std::string& cellKey, bool isAuthority);

// Present-hook frame update: interp-sample and raw-write positions.
// No engine calls.
void NpcPuppet_OnFrame();
