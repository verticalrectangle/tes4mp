#pragma once
#include <string>

// Callback type for enqueuing a console command (thread-safe).
typedef void (*GhostCmdFn)(const char* cmd);

// numSlots  : number of ACHR ghost refs (must match GHOST_SLOTS)
// refPtrs   : array of TESObjectREFR* pointers registered by TES4MP_SetGhostRef
// cmdFn     : thread-safe function to queue a console command for the next game tick
void GhostSystem_Init(int numSlots, void** refPtrs, GhostCmdFn cmdFn);
void GhostSystem_Shutdown();

// Called from the network thread — thread-safe.
void GhostSystem_OnAppear(const std::string& charId, const std::string& charName,
                          float x, float y, float z, float rotZ, int animGroup);
void GhostSystem_OnLeave(const std::string& charId);
void GhostSystem_OnPosUpdate(const std::string& charId,
                             float x, float y, float z, float rotZ, int animGroup);

// Called from D3D9 Present hook every rendered frame — game thread.
// Interpolates positions and writes them to TESObjectREFR memory.
void GhostSystem_OnFrame();
