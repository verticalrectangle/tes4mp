#include "quest_sync.h"
#include "game_hooks.h"
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
static constexpr ptrdiff_t kQuest_formID     = 0x00C;  // TESForm::refID

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

// Server-pushed stage applies (see QuestSync_ApplyStage below)
struct PendingStage { std::string editorId; int stage; int retries; };
static std::vector<PendingStage> g_pendingStages;   // guarded by g_mtx

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
    g_pendingStages.clear();
}

// ── Server-pushed stage applies ───────────────────────────────────────────────
// setstage-by-editorID never compiles in the RunScriptLine path; resolve the
// TESQuest* here (game thread) and execute via the %R/GetFormFromMod chain.

void QuestSync_ApplyStage(const std::string& editorId, int stage) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_pendingStages.push_back({ editorId, stage, 0 });
}

// Walk the quest list for a case-insensitive editorName match → formID.
static uint32_t FindQuestFormId(const std::string& editorId) {
    void* dh = *(void**)kDataHandlerPtr;
    if (!dh) return 0;
    struct Node { void* item; Node* next; };
    for (Node* n = (Node*)((char*)dh + kDH_questList); n; n = n->next) {
        void* q = n->item;
        if (!q) continue;
        const char* name = *(const char**)((char*)q + kQuest_edNameData);
        uint16_t    len  = *(uint16_t*)((char*)q + kQuest_edNameLen);
        if (!name || len == 0) continue;
        if (editorId.size() == len && _strnicmp(name, editorId.c_str(), len) == 0)
            return *(uint32_t*)((char*)q + kQuest_formID);
    }
    return 0;
}

static void DrainPendingStages() {
    std::vector<PendingStage> pending;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        std::swap(pending, g_pendingStages);
    }
    std::vector<PendingStage> requeue;
    for (auto& p : pending) {
        uint32_t fid = FindQuestFormId(p.editorId);
        char line[64], raw[128];
        snprintf(raw, sizeof(raw), "setstage %s %d", p.editorId.c_str(), p.stage);
        if (fid) {
            snprintf(line, sizeof(line), "SetStage %%R %d", p.stage);
            GameHooks_EnqueueFormCmd(nullptr, fid, line, raw);
        } else if (p.retries < 10) {
            p.retries++;                 // quest list not loaded yet — retry
            requeue.push_back(p);
        } else {
            GameHooks_EnqueueCmd(raw);   // unknown quest (mod?) — legacy path
        }
    }
    if (!requeue.empty()) {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto& p : requeue) g_pendingStages.push_back(std::move(p));
    }
}

void QuestSync_Tick() {
    DrainPendingStages();   // every tick — stage parity shouldn't wait 5s

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
