#pragma once
#include <string>
#include <cstdint>

// Appearance data read from the local player's TESNPC base form.
struct AppearanceData {
    uint32_t raceFormID;    // TESRace FormID
    uint8_t  gender;        // 0 = male, 1 = female
    uint32_t hairFormID;    // TESHair FormID
    uint32_t eyesFormID;    // TESEyes FormID
    float    faceGeo[17];   // face geometry sliders
    float    faceTex[8];    // face texture sliders
    bool     valid;
};

// Read appearance from the live player base form.
AppearanceData Appearance_ReadLocal();

// Encode to a JSON string suitable for the APPEARANCE packet.
std::string Appearance_ToJson(const AppearanceData& a);

// Apply appearance data to a ghost NPC's base form pointer.
// safe = false if the offsets can't be verified; caller should check valid.
bool Appearance_ApplyToRef(void* baseFormPtr, const AppearanceData& a);
