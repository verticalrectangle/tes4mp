#include "ui.h"
#include "game_hooks.h"

void UI_Init()     {}
void UI_Tick()     { GameHooks_Tick(); }
void UI_Shutdown() {}
