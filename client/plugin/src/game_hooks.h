#pragma once

// Forward declaration — avoid pulling OBSE types into callers.
struct OBSEInterface;
struct OBSEMessagingInterface;
typedef unsigned int PluginHandle;

// Call once during OBSEPlugin_Load.
void GameHooks_Init(OBSEInterface* obse, PluginHandle pluginHandle);

// Called by TES4MP_ButtonCallback registered command to deliver MessageBox results.
void GameHooks_SetButtonResult(int buttonIndex);

// Called from the main packet dispatch loop (game thread).
// Applies QUEST_STAGE / QUEST_SYNC packets by running console commands.
// Call this regularly (e.g. from a Windows timer callback or the game_hooks background thread).
void GameHooks_Tick();

// Stop background threads on unload.
void GameHooks_Shutdown();

// Thread-safe: enqueue a console command to run on the next game tick.
// Used by ghost_system and other subsystems from any thread.
void GameHooks_EnqueueCmd(const char* cmd);

// Like GameHooks_EnqueueCmd, but the line executes ON the given reference
// (RunScriptLine2 callingRefr — like RunBatchScript's run-on-ref mode).
// Replaces prid chains: hex formID literals don't parse in this compile path.
// refr must stay valid until the next game tick (~200ms).
void GameHooks_EnqueueCmdOnRef(void* refr, const char* cmd);

// Returns the player's current raw HP (from the last CHAR_SAVE checkpoint, 0 if unknown).
int GameHooks_GetPlayerHp();

// True when it's safe to walk engine object lists: in-world and not in
// menu mode (loading screens count as menus — cell lists mutate during them).
bool GameHooks_IsSafeToScan();

