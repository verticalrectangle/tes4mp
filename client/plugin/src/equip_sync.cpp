#include "equip_sync.h"
#include "oblivion_internal.h"
#include "network.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ── ExtraContainerChanges layout (OBSE GameExtraData.h, confirmed) ────────────
// EntryData        { EntryExtendData* extendData; SInt32 countDelta; TESForm* type; }
// EntryExtendData  { ExtraDataList* data; EntryExtendData* next; }
// ExtraDataList    (BaseExtraList) { void** vtbl; BSExtraData* m_data; ... }
// Entry            { EntryData* data; Entry* next; }   (same walk as npc_sync)

static constexpr uint8_t kExtraData_Worn     = 0x1B;
static constexpr uint8_t kExtraData_WornLeft = 0x1C;

static constexpr int MAX_ITEMS = 20;

static uint8_t ExtraType(void* ed) {
    // BSExtraData::GetType() = vtable[1]
    return ((uint8_t(__fastcall*)(void*))(*(void***)ed)[1])(ed);
}

// Does any ExtraDataList hanging off this entry contain ExtraWorn / ExtraWornLeft?
static bool EntryIsWorn(void* entryData) {
    void* extend = *(void**)entryData;  // EntryExtendData* at +0x000
    while (extend) {
        void* xlist = *(void**)extend;  // ExtraDataList*
        if (xlist) {
            void* ed = *(void**)((char*)xlist + 0x004);  // BaseExtraList::m_data
            while (ed) {
                uint8_t t = ExtraType(ed);
                if (t == kExtraData_Worn || t == kExtraData_WornLeft) return true;
                ed = *(void**)((char*)ed + 0x004);  // BSExtraData::next
            }
        }
        extend = *(void**)((char*)extend + 0x004);  // EntryExtendData::next
    }
    return false;
}

// Collect the player's worn base formIDs — vanilla (mod index 00) only, sorted.
static std::vector<uint32_t> ReadWornItems() {
    std::vector<uint32_t> items;

    void* player = *(void**)Oblivion::kPlayerPtr;
    if (!player) return items;

    void* ed = *(void**)((char*)player + Oblivion::kRef_extraList);
    while (ed) {
        if (ExtraType(ed) == Oblivion::kExtraData_ContainerChanges) {
            void* eccData = *(void**)((char*)ed + 0x008);  // ExtraContainerChanges::data
            if (!eccData) break;
            void* entry = *(void**)eccData;                // Data::objList
            while (entry && (int)items.size() < MAX_ITEMS) {
                void* edata = *(void**)entry;              // EntryData*
                if (edata && EntryIsWorn(edata)) {
                    void* form = *(void**)((char*)edata + 0x008);
                    if (form) {
                        uint32_t fid = *(uint32_t*)((char*)form + Oblivion::kForm_refID);
                        // Only Oblivion.esm forms resolve identically on every client
                        if (fid != 0 && (fid >> 24) == 0)
                            items.push_back(fid);
                    }
                }
                entry = *(void**)((char*)entry + 0x004);   // Entry::next
            }
            break;
        }
        ed = *(void**)((char*)ed + 0x004);
    }

    std::sort(items.begin(), items.end());
    return items;
}

// ── Public API ────────────────────────────────────────────────────────────────

static std::vector<uint32_t> g_lastSent;
static bool                  g_everSent = false;
static DWORD                 g_lastPoll = 0;

void EquipSync_Reset() {
    g_lastSent.clear();
    g_everSent = false;
}

void EquipSync_Tick() {
    DWORD now = GetTickCount();
    if (now - g_lastPoll < 2000) return;
    g_lastPoll = now;

    if (!g_network.isConnected()) return;

    std::vector<uint32_t> worn = ReadWornItems();
    if (g_everSent && worn == g_lastSent) return;

    std::string pkt = "{\"type\":\"EQUIP_UPDATE\",\"items\":[";
    for (size_t i = 0; i < worn.size(); ++i) {
        if (i) pkt += ',';
        char num[16];
        snprintf(num, sizeof(num), "%u", worn[i]);
        pkt += num;
    }
    pkt += "]}";
    g_network.send(pkt);

    g_lastSent = std::move(worn);
    g_everSent = true;
}
