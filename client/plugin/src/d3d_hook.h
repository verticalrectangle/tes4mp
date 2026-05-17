#pragma once

// D3D9 Present hook — gives per-frame execution on the game/render thread.
// Same technique used by ENB and OBGE.
//
// Call D3DHook_Init() once from OBSEPlugin_Load.
// The callback fires just before each frame is presented.

typedef void (*D3DFrameCallback)();

void D3DHook_Init(D3DFrameCallback cb);
void D3DHook_Shutdown();
