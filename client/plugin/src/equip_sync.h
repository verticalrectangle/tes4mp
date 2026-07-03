#pragma once

// Polls the local player's worn equipment (game thread, self rate-limited to 2s)
// and sends EQUIP_UPDATE when it changes. Call from GameHooks_Tick once connected.
void EquipSync_Tick();

// Reset the "last sent" cache (e.g. on disconnect) so the next connect re-sends.
void EquipSync_Reset();
