#include "quest_sync.h"
#include "network.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

// ── Oblivion 1.2.416 quest internals ─────────────────────────────────────────
// DataHandler global: xOBSE GameAPI.cpp binds g_dataHandler at 0x00B33A98.
// DataHandler::quests is a tList<TESQuest> at +0x084 (GameData.h).
// tList node: { item*, next* }.
// TESQuest (GameForms.h): stageIndex UInt8 at +0x05C (engine-maintained current
// stage — what GetStage returns), editorName BSStringT at +0x060
// (m_data char* at +0x060, m_dataLen UInt16 at +0x064). Runtime keeps quest
// editor names — xOBSE's DataHandler::GetQuestByEditorName does this same walk.

static constexpr uintptr_t kDataHandlerPtr   = 0x00B33A98;
static constexpr ptrdiff_t kDH_questList     = 0x084;
static constexpr ptrdiff_t kQuest_stageIndex = 0x05C;
static constexpr ptrdiff_t kQuest_edNameData = 0x060;
static constexpr ptrdiff_t kQuest_edNameLen  = 0x064;

struct Monitored {
    std::string questId;
    void*       quest     = nullptr;  // TESQuest*, resolved lazily
    int         lastStage = -1;       // -1 = not yet baselined
};

static std::mutex             g_mtx;   // guards the list swap from the net thread
static std::vector<Monitored> g_quests;
static bool                   g_allResolved = false;
static DWORD                  g_lastPoll    = 0;

void QuestSync_SetMonitored(const std::vector<std::string>& questIds) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_quests.clear();
    g_quests.reserve(questIds.size());
    for (const auto& id : questIds) {
        Monitored m;
        m.questId = id;
        g_quests.push_back(std::move(m));
    }
    g_allResolved = false;
}

void QuestSync_Reset() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_quests.clear();
    g_allResolved = false;
}

// Walk the DataHandler quest list once, filling in TESQuest* for any
// still-unresolved monitored entries (case-insensitive editor-name match).
static void ResolveQuests() {
    void* dh = *(void**)kDataHandlerPtr;
    if (!dh) return;

    struct Node { void* item; Node* next; };
    bool any = false;

    for (Node* n = (Node*)((char*)dh + kDH_questList); n; n = n->next) {
        void* q = n->item;
        if (!q) continue;
        const char* name = *(const char**)((char*)q + kQuest_edNameData);
        uint16_t    len  = *(uint16_t*)((char*)q + kQuest_edNameLen);
        if (!name || len == 0) continue;

        for (auto& m : g_quests) {
            if (m.quest) continue;
            if (m.questId.size() == len && _strnicmp(name, m.questId.c_str(), len) == 0) {
                m.quest = q;
                any = true;
            }
        }
    }

    g_allResolved = true;
    for (auto& m : g_quests)
        if (!m.quest) { g_allResolved = false; break; }
    (void)any;
}

void QuestSync_Tick() {
    DWORD now = GetTickCount();
    if (now - g_lastPoll < 5000) return;
    g_lastPoll = now;

    if (!g_network.isConnected()) return;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_quests.empty()) return;

    if (!g_allResolved) ResolveQuests();

    for (auto& m : g_quests) {
        if (!m.quest) continue;
        int stage = *(uint8_t*)((char*)m.quest + kQuest_stageIndex);

        if (m.lastStage == -1) {
            // Baseline. Upload any non-zero local progress once — the server is
            // forward-only, so stale/equal stages are ignored there.
            m.lastStage = stage;
            if (stage == 0) continue;
        } else if (stage == m.lastStage) {
            continue;
        } else if (stage < m.lastStage) {
            m.lastStage = stage;  // local regression (console tinkering) — don't send
            continue;
        } else {
            m.lastStage = stage;
        }

        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"QUEST_STAGE\",\"questId\":\"%s\",\"stage\":%d}",
            m.questId.c_str(), m.lastStage);
        g_network.send(buf);
    }
}
