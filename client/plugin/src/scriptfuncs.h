#pragma once
#include "../include/obse_types.h"

// Registers all TES4MP script commands with OBSE.
// Call once from OBSEPlugin_Load.
void RegisterScriptFunctions(OBSEInterface* obse);

// Opcode base — must not conflict with other installed OBSE plugins.
// Change this if you have a collision. Range: 0x3800 – 0x380F
constexpr UInt32 kOpcodeBase = 0x3800;
