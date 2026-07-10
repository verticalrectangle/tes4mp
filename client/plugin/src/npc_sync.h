#pragma once
#include <string>
#include <cstdint>

// Called from GameHooks_Tick() once per tick (~200ms).
// cellKey: current cell key string from pos_sync (empty = no cell).
void NpcSync_Tick(const std::string& cellKey);

// Apply an NPC_KILLED packet from the server — kill the ref on this client.
void NpcSync_OnKilled(uint32_t refId);

// Apply an NPC_KILL_SYNC packet (cell entry) — disable listed refs silently.
void NpcSync_OnKillSync(const uint32_t* refs, int count);

// Apply an ITEM_SYNC packet — remove items from a container ref.
void NpcSync_OnItemSync(uint32_t containerRefId, uint32_t itemFormId, int count);

// Apply a WORLD_ITEM_SYNC packet — disable loose item refs a peer picked up
// (broadcast) or that were taken before we entered the cell (entry push).
void NpcSync_OnWorldItemSync(const uint32_t* refs, int count);

// Apply a CONTAINER_STATE packet (cell entry) — remove already-looted items.
struct ContainerEntry { uint32_t refId; uint32_t formId; int count; };
void NpcSync_OnContainerState(const ContainerEntry* entries, int count);

// ── NPC HP authority sync ─────────────────────────────────────────────────────

// Server-assigned NPC simulation authority for a cell (CELL_AUTHORITY packet).
// Thread-safe.
void NpcSync_SetAuthority(const char* cellKey, bool authority);

// Queue an NPC_HP batch from the cell authority for application on the next
// game-thread tick (we must read local actor HP to merge). Thread-safe.
struct NpcHpEntry { uint32_t ref; int hp; };
void NpcSync_OnHpSync(const NpcHpEntry* entries, int count);

// A peer damaged an NPC we simulate — apply it (console cmds only; thread-safe).
void NpcSync_OnDamageRequest(uint32_t refId, int amount);
