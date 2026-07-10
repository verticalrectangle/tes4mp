#pragma once
// Shared snapshot-ring interpolation for remote-driven actors (player ghosts,
// puppet NPCs). Extracted from ghost_system.cpp — behavior identical:
// interpolate INTERP delay behind live, shortest-arc rotation, brief velocity
// extrapolation on buffer underrun, snap after long silence.
#include <cstdint>

namespace Interp {

static constexpr int      SNAP_CAP      = 16;   // ~1s of history at 15Hz
static constexpr uint32_t DELAY_MS      = 150;  // render this far behind live
static constexpr uint32_t EXTRAP_MAX_MS = 250;  // extrapolation cap on underrun
static constexpr uint32_t GAP_RESET_MS  = 1500; // silence → drop history, snap

struct Snap {
    uint32_t ms;
    float    x, y, z, rotZ;
};

// Shortest-arc angle lerp — plain lerp spins the long way across the ±π wrap.
float LerpAngle(float a, float b, float t);

class Buffer {
public:
    void push(uint32_t nowMs, float x, float y, float z, float rotZ);

    // Sample the interpolated pose at renderMs (caller applies DELAY_MS).
    // Extrapolates ≤ EXTRAP_MAX_MS past the newest snapshot on underrun.
    // Returns false only when the buffer is empty.
    bool sample(uint32_t renderMs, float& x, float& y, float& z, float& rotZ) const;

    void clear() { head_ = 0; count_ = 0; }
    bool empty() const { return count_ == 0; }

private:
    Snap buf_[SNAP_CAP] = {};
    int  head_  = 0;
    int  count_ = 0;
};

// Write a pose straight into a TESObjectREFR + its NiNode world transform.
// Game/render thread only; no engine calls.
void WriteRef(void* ref, float x, float y, float z, float rotZ);

} // namespace Interp
