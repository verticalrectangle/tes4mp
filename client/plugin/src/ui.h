#pragma once

// Initialise the hidden hotkey window.
// F10 opens the command dialog; typed text is sent as a COMMAND packet.
void UI_Init();

// Process pending Windows messages for our hidden window.
// Call regularly from the game-thread timer.
void UI_Tick();

void UI_Shutdown();
