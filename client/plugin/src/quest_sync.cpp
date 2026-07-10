#include "quest_sync.h"
#include "network.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

// WP14: EVERY quest syncs — the server decides scope (global/party) and holds
// the denylist, so the client just reports any stage change on any quest.
// Keyed by TESQuest* (stable for the process lifetime; the DataHandler list
// only grows). Baseline pass uploads existing non-zero progress once — the
// server is forward-only, so equal/stale stages are ignored there. Sends are
// capped per poll so a mid-game character's first upload spreads over a few
// polls instead of tripping the server's burst guard.

static constexpr int kMaxSendsPerPoll = 8;

static std::mutex g_mtx;      // guards enable/reset from the network thread
static bool       g_enabled  = false;
static bool       g_baselined = false;
static std::unordered_map<void*, int> g_stages;   // TESQuest* → lastStage
static DWORD      g_lastPoll = 0;

void QuestSync_SetMonitored(const std::vector<std::string>& questIds) {
    // Server list kept for protocol compat; monitoring is all-quests now.
    (void)questIds;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_enabled = true;
}

void QuestSync_Reset() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_enabled   = false;
    g_baselined = false;
    g_stages.clear();
}

void QuestSync_Tick() {
    DWORD now = GetTickCount();
    if (now - g_lastPoll < 5000) return;
    g_lastPoll = now;

    if (!g_network.isConnected()) return;

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_enabled) return;

    void* dh = *(void**)kDataHandlerPtr;
    if (!dh) return;

    struct Node { void* item; Node* next; };
    int sent = 0;
    bool deferred = false;   // hit the send cap — don't mark baseline done

    for (Node* n = (Node*)((char*)dh + kDH_questList); n; n = n->next) {
        void* q = n->item;
        if (!q) continue;
        const char* name = *(const char**)((char*)q + kQuest_edNameData);
        uint16_t    len  = *(uint16_t*)((char*)q + kQuest_edNameLen);
        if (!name || len == 0 || len > 64) continue;

        int stage = *(uint8_t*)((char*)q + kQuest_stageIndex);

        auto it = g_stages.find(q);
        if (it == g_stages.end()) {
            if (sent >= kMaxSendsPerPoll) { deferred = true; continue; }
            g_stages[q] = stage;
            // Baseline: upload pre-existing non-zero progress once.
            if (g_baselined || stage == 0) continue;
        } else if (stage == it->second) {
            continue;
        } else if (stage < it->second) {
            it->second = stage;   // local regression (console tinkering) — don't send
            continue;
        } else {
            if (sent >= kMaxSendsPerPoll) { deferred = true; continue; }
            it->second = stage;
        }

        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"QUEST_STAGE\",\"questId\":\"%.*s\",\"stage\":%d}",
            (int)len, name, stage);
        g_network.send(buf);
        ++sent;
    }

    if (!deferred) g_baselined = true;
}
