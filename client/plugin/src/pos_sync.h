#pragma once
#include <cstdint>

void PosSync_Start();
void PosSync_Stop();

// Snapshot of the local player's world state, readable from any thread.
struct PlayerState {
    float    x, y, z, rotZ;
    uint32_t cellFormID;
    bool     valid;
};
PlayerState PosSync_GetLocal();
