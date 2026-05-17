#pragma once
#include <string>
#include <cstdint>

// Callback type for enqueuing a console command (thread-safe).
typedef void (*GhostCmdFn)(const char* cmd);

// numSlots : maximum concurrent ghost players
// cmdFn    : thread-safe function to queue a console command for the next game tick
void GhostSystem_Init(int numSlots, GhostCmdFn cmdFn);
void GhostSystem_Shutdown();

// Called from the network thread — thread-safe.
// raceFormId: TESRace formID from the remote player's appearance (0 = unknown).
// gender: 0=male, 1=female (0 = unknown/default).
void GhostSystem_OnAppear(const std::string& charId, const std::string& charName,
                          uint32_t raceFormId, int gender,
                          float x, float y, float z, float rotZ, int animGroup);
void GhostSystem_OnLeave(const std::string& charId);
void GhostSystem_OnPosUpdate(const std::string& charId,
                             float x, float y, float z, float rotZ, int animGroup, int hp);

// Called from D3D9 Present hook every rendered frame — game thread.
// Interpolates positions and writes them to TESObjectREFR memory.
void GhostSystem_OnFrame();

// Returns true if formId belongs to a ghost ref spawned by this system.
// Used by npc_sync to skip scanning ghost actors.
bool GhostSystem_IsGhostRef(uint32_t formId);
