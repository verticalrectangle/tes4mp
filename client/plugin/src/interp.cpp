#include "interp.h"
#include "oblivion_internal.h"

namespace Interp {

static constexpr float kPi = 3.14159265358979f;

float LerpAngle(float a, float b, float t) {
    float d = b - a;
    while (d >  kPi) d -= 2.f * kPi;
    while (d < -kPi) d += 2.f * kPi;
    return a + t * d;
}

void Buffer::push(uint32_t now, float x, float y, float z, float rotZ) {
    // Long silence (out of range, lag spike) — snap to the new position
    // rather than gliding across the whole gap.
    if (count_ > 0) {
        int newest = (head_ + SNAP_CAP - 1) % SNAP_CAP;
        if (now - buf_[newest].ms > GAP_RESET_MS) { count_ = 0; head_ = 0; }
    }
    Snap& s = buf_[head_];
    s.ms = now;
    s.x = x; s.y = y; s.z = z; s.rotZ = rotZ;
    head_ = (head_ + 1) % SNAP_CAP;
    if (count_ < SNAP_CAP) count_++;
}

bool Buffer::sample(uint32_t renderMs, float& ox, float& oy, float& oz, float& orot) const {
    if (count_ == 0) return false;

    int newest = (head_ + SNAP_CAP - 1) % SNAP_CAP;
    int oldest = (head_ + SNAP_CAP - count_) % SNAP_CAP;

    const Snap& sNew = buf_[newest];
    if (renderMs >= sNew.ms || count_ == 1) {
        ox = sNew.x; oy = sNew.y; oz = sNew.z; orot = sNew.rotZ;
        // Buffer underrun — extrapolate briefly along the last known velocity
        // to hide network jitter, then hold position.
        if (count_ >= 2 && renderMs > sNew.ms) {
            const Snap& sPrev = buf_[(newest + SNAP_CAP - 1) % SNAP_CAP];
            uint32_t dt = sNew.ms - sPrev.ms;
            if (dt > 0 && dt < 1000) {
                uint32_t ahead = renderMs - sNew.ms;
                if (ahead > EXTRAP_MAX_MS) ahead = EXTRAP_MAX_MS;
                float k = (float)ahead / (float)dt;
                ox += (sNew.x - sPrev.x) * k;
                oy += (sNew.y - sPrev.y) * k;
                oz += (sNew.z - sPrev.z) * k;
            }
        }
        return true;
    }

    int prevIdx = oldest;
    for (int i = 1; i < count_; i++) {
        int curIdx = (oldest + i) % SNAP_CAP;
        const Snap& sa = buf_[prevIdx];
        const Snap& sb = buf_[curIdx];
        if (sa.ms <= renderMs && renderMs <= sb.ms) {
            float t = (sb.ms == sa.ms) ? 0.f
                      : (float)(renderMs - sa.ms) / (float)(sb.ms - sa.ms);
            ox   = sa.x + t * (sb.x - sa.x);
            oy   = sa.y + t * (sb.y - sa.y);
            oz   = sa.z + t * (sb.z - sa.z);
            orot = LerpAngle(sa.rotZ, sb.rotZ, t);
            return true;
        }
        prevIdx = curIdx;
    }

    const Snap& sOld = buf_[oldest];
    ox = sOld.x; oy = sOld.y; oz = sOld.z; orot = sOld.rotZ;
    return true;
}

void WriteRef(void* ref, float x, float y, float z, float rotZ) {
    using namespace Oblivion;
    auto* r = static_cast<char*>(ref);
    *reinterpret_cast<float*>(r + kRef_posX) = x;
    *reinterpret_cast<float*>(r + kRef_posY) = y;
    *reinterpret_cast<float*>(r + kRef_posZ) = z;
    *reinterpret_cast<float*>(r + kRef_rotZ) = rotZ;

    void* ni = *reinterpret_cast<void**>(r + kRef_niNode);
    if (ni) {
        auto* n = static_cast<char*>(ni);
        *reinterpret_cast<float*>(n + kNi_worldTransX) = x;
        *reinterpret_cast<float*>(n + kNi_worldTransY) = y;
        *reinterpret_cast<float*>(n + kNi_worldTransZ) = z;
        *reinterpret_cast<float*>(n + kNi_boundCtrX)   = x;
        *reinterpret_cast<float*>(n + kNi_boundCtrY)   = y;
        *reinterpret_cast<float*>(n + kNi_boundCtrZ)   = z;
    }
}

} // namespace Interp
