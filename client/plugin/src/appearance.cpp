#include "appearance.h"
#include <windows.h>
#include <sstream>
#include <iomanip>
#include <cstring>

// ── Oblivion 1.2.416 TESNPC memory layout ────────────────────────────────────
// Offsets from the TESNPC* (player->baseForm).
// TESFullName at 0xA0 is verified (used in existing code for player name).
// Post-TESFullName fields are derived from OBSE headers and need in-game verification.

static constexpr uintptr_t ADDR_PLAYER_PTR  = 0x00B333C4;
static constexpr ptrdiff_t OFF_REF_BASE     = 0x01C;  // TESForm* baseForm (verified)

// Within TESNPC base form:
static constexpr ptrdiff_t OFF_NPC_RACE     = 0x0AC;  // TESRace*  — approx
static constexpr ptrdiff_t OFF_NPC_GENDER   = 0x0B0;  // UInt8     — approx
static constexpr ptrdiff_t OFF_NPC_HAIR     = 0x0B4;  // TESHair*  — approx
static constexpr ptrdiff_t OFF_NPC_EYES     = 0x0B8;  // TESEyes*  — approx
// Face geometry: 17 floats immediately after eyes pointer
static constexpr ptrdiff_t OFF_NPC_FACE_GEO = 0x0BC;  // float[17] — approx
// Face texture: 8 floats immediately after geometry
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
    out.gender     = *(uint8_t*)((char*)base + OFF_NPC_GENDER);

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

    // We only write scalar fields; pointer fields (race, hair, eyes) require
    // the target game to have those FormIDs loaded — skip them for now.
    *(uint8_t*)((char*)base + OFF_NPC_GENDER) = a.gender;
    memcpy((char*)base + OFF_NPC_FACE_GEO, a.faceGeo, sizeof(a.faceGeo));
    memcpy((char*)base + OFF_NPC_FACE_TEX, a.faceTex, sizeof(a.faceTex));
    return true;
}
