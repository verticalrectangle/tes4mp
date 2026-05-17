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
