#include "appearance.h"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <cstring>

// ── Oblivion 1.2.416 TESNPC memory layout ────────────────────────────────────
// Offsets from the TESNPC* (player->baseForm).
// All offsets confirmed from GameForms.h TESNPC / TESActorBase / TESActorBaseData layout.

static constexpr uintptr_t ADDR_PLAYER_PTR  = 0x00B333C4;
static constexpr ptrdiff_t OFF_REF_BASE     = 0x01C;  // TESForm* baseForm (verified)

// Within TESNPC base form (inherits TESActorBase):
// TESActorBase::TESActorBaseData actorBaseData at 0x024; flags UInt32 at actorBaseData+0x004
static constexpr ptrdiff_t OFF_NPC_GENDER   = 0x028;  // UInt32 flags; bit 0 = kFlag_IsFemale
// TESNPC::TESRaceForm race at 0xE4; TESRaceForm::TESRace* race at TESRaceForm+0x004
static constexpr ptrdiff_t OFF_NPC_RACE     = 0x0E8;  // TESRace* (via TESRaceForm.race)
// TESNPC members (GameForms.h lines 2696, 2698)
static constexpr ptrdiff_t OFF_NPC_HAIR     = 0x1C8;  // TESHair*
static constexpr ptrdiff_t OFF_NPC_EYES     = 0x1D0;  // TESEyes*
// Face geometry and texture follow eyes — offsets relative to TESNPC base
static constexpr ptrdiff_t OFF_NPC_FACE_GEO = 0x0BC;  // float[17] — approx (BSFaceGenNiNode path deferred)
static constexpr ptrdiff_t OFF_NPC_FACE_TEX = OFF_NPC_FACE_GEO + 17 * 4; // 0x100

// TESForm::refID at +0x00C (verified: +0x004=typeID, +0x008=flags, +0x00C=refID)
static constexpr ptrdiff_t OFF_FORM_ID      = 0x00C;

// ── Read ──────────────────────────────────────────────────────────────────────

AppearanceData Appearance_ReadLocal() {
    AppearanceData out = {};

    void* player = *(void**)ADDR_PLAYER_PTR;
    if (!player) return out;

    void* base = *(void**)((char*)player + OFF_REF_BASE);
    if (!base) return out;

    void* racePtr = *(void**)((char*)base + OFF_NPC_RACE);
    void* hairPtr = *(void**)((char*)base + OFF_NPC_HAIR);
    void* eyesPtr = *(void**)((char*)base + OFF_NPC_EYES);

    out.raceFormID = racePtr ? *(uint32_t*)((char*)racePtr + OFF_FORM_ID) : 0;
    out.hairFormID = hairPtr ? *(uint32_t*)((char*)hairPtr + OFF_FORM_ID) : 0;
    out.eyesFormID = eyesPtr ? *(uint32_t*)((char*)eyesPtr + OFF_FORM_ID) : 0;
    uint32_t actorFlags = *(uint32_t*)((char*)base + OFF_NPC_GENDER);
    out.gender = (actorFlags & 0x00000001) ? 1 : 0;  // kFlag_IsFemale = bit 0

    memcpy(out.faceGeo, (char*)base + OFF_NPC_FACE_GEO, sizeof(out.faceGeo));
    memcpy(out.faceTex, (char*)base + OFF_NPC_FACE_TEX, sizeof(out.faceTex));

    out.valid = true;
    return out;
}

// ── JSON encode ───────────────────────────────────────────────────────────────

std::string Appearance_ToJson(const AppearanceData& a) {
    std::ostringstream o;
    o << std::fixed;
    o.precision(4);

    o << "{\"type\":\"APPEARANCE\""
      << ",\"race\":"   << a.raceFormID
      << ",\"gender\":" << (int)a.gender
      << ",\"hair\":"   << a.hairFormID
      << ",\"eyes\":"   << a.eyesFormID
      << ",\"geo\":[";
    for (int i = 0; i < 17; ++i) { if (i) o << ','; o << a.faceGeo[i]; }
    o << "],\"tex\":[";
    for (int i = 0; i < 8;  ++i) { if (i) o << ','; o << a.faceTex[i]; }
    o << "]}";
    return o.str();
}

// ── Apply to ghost base form ───────────────────────────────────────────────────

bool Appearance_ApplyToRef(void* base, const AppearanceData& a) {
    if (!base || !a.valid) return false;

    // Gender is applied via SexChange console command, not direct memory write.
    // Race/hair/eyes are applied via setrace command and future form-by-ID lookup.
    memcpy((char*)base + OFF_NPC_FACE_GEO, a.faceGeo, sizeof(a.faceGeo));
    memcpy((char*)base + OFF_NPC_FACE_TEX, a.faceTex, sizeof(a.faceTex));
    return true;
}
