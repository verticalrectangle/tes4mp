#pragma once
#include <string>
#include <vector>

// Set the server's monitored quest list (editor IDs) from CHAR_LOAD.
// Thread-safe; resets resolution state.
void QuestSync_SetMonitored(const std::vector<std::string>& questIds);

// Poll monitored quests' stages and report changes as QUEST_STAGE packets.
// Game thread only (reads engine memory); self rate-limited to 5s.
void QuestSync_Tick();

// Clear state on disconnect so the next connect re-baselines.
void QuestSync_Reset();
