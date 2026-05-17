#pragma once
// Minimal OBSE type definitions derived from the real OBSE 0021 SDK (PluginAPI.h / CommandTable.h).
// Avoids pulling in the full SDK header chain.

#include <cstdint>

typedef uint32_t UInt32;
typedef uint16_t UInt16;
typedef uint8_t  UInt8;
typedef int32_t  SInt32;

typedef UInt32 PluginHandle;

// Opaque game types
struct Script          {};
struct ScriptEventList {};
struct TESObjectREFR   {};
struct ParamInfo;

// Real COMMAND_ARGS from CommandTable.h
#define COMMAND_ARGS \
    ParamInfo* paramInfo, void* arg1, TESObjectREFR* thisObj, UInt32 arg3, \
    Script* scriptObj, ScriptEventList* eventList, double* result, UInt32* opcodeOffsetPtr

#define PASS_COMMAND_ARGS \
    paramInfo, arg1, thisObj, arg3, scriptObj, eventList, result, opcodeOffsetPtr

typedef bool (*Cmd_Execute)(COMMAND_ARGS);

// ParamType enum (from CommandTable.h)
enum ParamType {
    kParamType_String  = 0x00,
    kParamType_Integer = 0x01,
    kParamType_Float   = 0x02,
    kParamType_ObjectRef = 0x04,
};

struct ParamInfo {
    const char* typeStr;
    UInt32      typeID;
    UInt32      isOptional;
};

struct CommandInfo {
    const char* longName;
    const char* shortName;
    UInt32      opcode;
    const char* helpString;
    UInt16      needsParent;
    UInt16      numParams;
    ParamInfo*  params;
    Cmd_Execute execute;
    void*       parse;
    void*       eval;
    UInt32      flags;
};

// OBSEStringVarInterface (kInterface_StringVar = 2)
struct OBSEStringVarInterface {
    UInt32 version;
    // Assign a string as the return value of a script function.
    // Call as: g_strInterface->Assign(PASS_COMMAND_ARGS, "value")
    bool (*Assign)(COMMAND_ARGS, const char* newValue);
    void (*Register)(OBSEStringVarInterface* intfc);
};

// OBSEInterface — matches real layout from PluginAPI.h
struct OBSEInterface {
    UInt32 obseVersion;
    UInt32 oblivionVersion;
    UInt32 editorVersion;
    UInt32 isEditor;
    bool   (*RegisterCommand)(CommandInfo* info);
    void   (*SetOpcodeBase)(UInt32 opcode);
    void*  (*QueryInterface)(UInt32 id);
    PluginHandle (*GetPluginHandle)(void);
    bool   (*RegisterTypedCommand)(CommandInfo* info, UInt32 retnType);
    const char* (*GetOblivionDirectory)();
    bool   (*GetPluginLoaded)(const char* pluginName);
    UInt32 (*GetPluginVersion)(const char* pluginName);
};

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

// Interface IDs (from PluginAPI.h)
enum {
    kInterface_Console       = 0,
    kInterface_Serialization = 1,
    kInterface_StringVar     = 2,
    kInterface_IO            = 3,
    kInterface_Messaging     = 4,
    kInterface_ArrayVar      = 5,
};

// OBSEScriptInterface (kInterface_Script = 7)
// As of OBSE 0020, use ExtractArgsEx instead of the old ExtractArgs.
struct OBSEArrayElement {};  // opaque

struct OBSEScriptInterface {
    bool (*CallFunction)(Script* funcScript, TESObjectREFR* callingObj,
                         TESObjectREFR* container, OBSEArrayElement* result,
                         UInt8 numArgs, ...);
    UInt32 (*GetFunctionParams)(Script* funcScript, UInt8* paramTypesOut);
    // ExtractArgsEx: pass arg1 as scriptDataIn, opcodeOffsetPtr as scriptDataOffset
    bool (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptDataIn,
                          UInt32* scriptDataOffset, Script* scriptObj,
                          ScriptEventList* eventList, ...);
    bool (*ExtractFormatStringArgs)(UInt32 fmtStringPos, char* buffer,
                                    ParamInfo* paramInfo, void* scriptDataIn,
                                    UInt32* scriptDataOffset, Script* scriptObj,
                                    ScriptEventList* eventList, UInt32 maxParams, ...);
    bool (*IsUserFunction)(Script* scriptObj);
};

enum {
    kInterface_Script = 7,
};

// OBSEConsoleInterface (kInterface_Console = 0)
struct OBSEConsoleInterface {
    UInt32 version;
    bool (*RunScriptLine)(const char* buf);
    // Runs cmd silently. callingRefr may be NULL.
    bool (*RunScriptLine2)(const char* buf, TESObjectREFR* callingRefr, bool bSuppressOutput);
};

// OBSEMessagingInterface (kInterface_Messaging = 4)
struct OBSEMessagingInterface {
    struct Message {
        const char* sender;
        UInt32      type;
        UInt32      dataLen;
        void*       data;
    };
    typedef void (*EventCallback)(Message* msg);
    enum { kVersion = 1 };
    enum {
        kMessage_PostLoad = 0,
        kMessage_ExitGame,
        kMessage_ExitToMainMenu,
        kMessage_LoadGame,
        kMessage_SaveGame,
        kMessage_Precompile,
        kMessage_PreLoadGame,
        kMessage_ExitGame_Console,
        kMessage_PostLoadGame,  // = 8, data = bool* (true if load succeeded)
        kMessage_PostPostLoad,
        kMessage_RuntimeScriptError,
    };
    UInt32 version;
    bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
    bool (*Dispatch)(PluginHandle sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};
