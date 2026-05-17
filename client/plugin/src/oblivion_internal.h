#pragma once
#include <cstdint>
#include <cstddef>

// Oblivion 1.2.416 confirmed memory layout.
// Offsets from GameObjects.h (authoritative) unless noted.
namespace Oblivion {

// ── Player global ──────────────────────────────────────────────────────────────
static constexpr uintptr_t kPlayerPtr = 0x00B333C4; // TESObjectREFR**

// ── TESObjectREFR byte offsets ────────────────────────────────────────────────
static constexpr ptrdiff_t kRef_flags      = 0x008; // UInt32 (TESForm::flags)
static constexpr ptrdiff_t kRef_baseForm   = 0x01C; // TESForm*
static constexpr ptrdiff_t kRef_rotX       = 0x020; // float — confirmed GameObjects.h
static constexpr ptrdiff_t kRef_rotY       = 0x024; // float
static constexpr ptrdiff_t kRef_rotZ       = 0x028; // float — confirmed GameObjects.h
static constexpr ptrdiff_t kRef_posX       = 0x02C; // float — confirmed GameObjects.h
static constexpr ptrdiff_t kRef_posY       = 0x030; // float
static constexpr ptrdiff_t kRef_posZ       = 0x034; // float
static constexpr ptrdiff_t kRef_scale      = 0x038; // float
static constexpr ptrdiff_t kRef_niNode     = 0x03C; // NiNode* — confirmed GameObjects.h
static constexpr ptrdiff_t kRef_parentCell = 0x040; // TESObjectCELL* — confirmed

// ── TESForm offsets ───────────────────────────────────────────────────────────
static constexpr ptrdiff_t kForm_refID  = 0x00C; // UInt32 formID
static constexpr uint32_t  kFlag_Disabled = 0x00000800;

// ── TESActorBase name (baseForm + 0xA4 = char*) ───────────────────────────────
static constexpr ptrdiff_t kBase_nameStr = 0xA4;

// ── NiAVObject transform offsets (from NiAVObject start = niNode pointer value)
// Layout: NiRefObject(0x08) + NiObjectNET(0x10) + NiAVObject fields:
//   0x018 parent*, 0x01C flags
//   0x020 m_kLocal  (NiTransform = NiMatrix3[36B] + NiPoint3[12B] + float[4B] = 52B)
//     rotate:    0x020..0x043
//     translate: 0x044  (NiPoint3: x=0x044, y=0x048, z=0x04C)
//     scale:     0x050
//   0x054 m_kWorld  (same layout)
//     rotate:    0x054..0x077
//     translate: 0x078  (x=0x078, y=0x07C, z=0x080)
//     scale:     0x084
//   0x088 m_kWorldBound (NiBound: center NiPoint3 + float radius)
static constexpr ptrdiff_t kNi_localTransX = 0x044;
static constexpr ptrdiff_t kNi_localTransY = 0x048;
static constexpr ptrdiff_t kNi_localTransZ = 0x04C;
static constexpr ptrdiff_t kNi_worldTransX = 0x078;
static constexpr ptrdiff_t kNi_worldTransY = 0x07C;
static constexpr ptrdiff_t kNi_worldTransZ = 0x080;
static constexpr ptrdiff_t kNi_boundCtrX   = 0x088; // NiBound center x
static constexpr ptrdiff_t kNi_boundCtrY   = 0x08C;
static constexpr ptrdiff_t kNi_boundCtrZ   = 0x090;

// ── Animation group codes (for PlayGroup console command) ─────────────────────
enum AnimGroup : int {
    kAnim_Idle        = 0x00,
    kAnim_Forward     = 0x03, // walk forward
    kAnim_Backward    = 0x04,
    kAnim_FastForward = 0x07, // run forward
    kAnim_FastBack    = 0x08,
    kAnim_TurnLeft    = 0x0F,
    kAnim_TurnRight   = 0x10,
    kAnim_JumpStart   = 0x28,
    kAnim_JumpLoop    = 0x29,
    kAnim_JumpLand    = 0x2A,
};

inline const char* AnimGroupName(int g) {
    switch (g) {
    case kAnim_Idle:        return "Idle";
    case kAnim_Forward:     return "Forward";
    case kAnim_Backward:    return "Backward";
    case kAnim_FastForward: return "FastForward";
    case kAnim_FastBack:    return "FastBackward";
    case kAnim_TurnLeft:    return "TurnLeft";
    case kAnim_TurnRight:   return "TurnRight";
    case kAnim_JumpStart:   return "JumpStart";
    case kAnim_JumpLoop:    return "JumpLoop";
    case kAnim_JumpLand:    return "JumpLand";
    default:                return "Idle";
    }
}

// Speed thresholds (Oblivion units/second)
static constexpr float kSpeed_Walk = 60.f;
static constexpr float kSpeed_Run  = 200.f;

} // namespace Oblivion
