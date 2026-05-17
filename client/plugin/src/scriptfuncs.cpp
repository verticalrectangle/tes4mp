#include "scriptfuncs.h"
#include "game_hooks.h"
#include "network.h"
#include "json_util.h"
#include <string>

static OBSEStringVarInterface* g_strInterface  = nullptr;
static OBSEScriptInterface*    g_scriptInterface = nullptr;
static std::string g_localRole;
static Packet      g_currentPacket;

static std::vector<json::QuestEntry> g_syncEntries;

// Wrapper matching the ExtractArgsEx signature from OBSE 0020+
#define ExtractArgsEx(...) g_scriptInterface->ExtractArgsEx(__VA_ARGS__)

// Assign a C string as the return value of a script function
static bool AssignStr(COMMAND_ARGS, const char* s) {
    *result = 0;
    if (g_strInterface)
        return g_strInterface->Assign(PASS_COMMAND_ARGS, s);
    return false;
}

// ---- Command implementations ----

// TES4MP_Connect "host" port "charName" "token" -> 1/0
static bool Cmd_TES4MP_Connect_Execute(COMMAND_ARGS) {
    *result = 0;
    char* host  = nullptr;
    int   port  = 7777;
    char* name  = nullptr;
    char* token = nullptr;

    if (!ExtractArgsEx(paramInfo, arg1, opcodeOffsetPtr, scriptObj, eventList,
                     &host, &port, &name, &token))
        return true;

    if (g_network.connect(host, port)) {
        std::string pkt = json::obj({
            json::str("type",  "AUTH"),
            json::str("name",  name  ? name  : ""),
            json::str("token", token ? token : ""),
        });
        g_network.send(pkt);
        *result = 1;
    }
    return true;
}

// TES4MP_IsConnected -> 1/0
static bool Cmd_TES4MP_IsConnected_Execute(COMMAND_ARGS) {
    *result = g_network.isConnected() ? 1.0 : 0.0;
    return true;
}

// TES4MP_PollPacket -> 1 if packet dequeued, 0 if queue empty
static bool Cmd_TES4MP_PollPacket_Execute(COMMAND_ARGS) {
    if (g_network.pollPacket(g_currentPacket)) {
        *result = 1.0;
        if (g_currentPacket.type == PacketType::QuestSync)
            g_syncEntries = json::getQuestArray(g_currentPacket.raw);
    } else {
        *result = 0.0;
        g_currentPacket = Packet{};
    }
    return true;
}

// TES4MP_GetPacketType -> int
static bool Cmd_TES4MP_GetPacketType_Execute(COMMAND_ARGS) {
    *result = (double)static_cast<int>(g_currentPacket.type);
    return true;
}

// TES4MP_GetPacketStr -> string (questId / name / text / role / reason)
static bool Cmd_TES4MP_GetPacketStr_Execute(COMMAND_ARGS) {
    return AssignStr(PASS_COMMAND_ARGS, g_currentPacket.strField.c_str());
}

// TES4MP_GetPacketStr2 -> string (src / scope)
static bool Cmd_TES4MP_GetPacketStr2_Execute(COMMAND_ARGS) {
    return AssignStr(PASS_COMMAND_ARGS, g_currentPacket.strField2.c_str());
}

// TES4MP_GetPacketInt -> int (stage / playerId)
static bool Cmd_TES4MP_GetPacketInt_Execute(COMMAND_ARGS) {
    *result = (double)g_currentPacket.intField;
    return true;
}

// TES4MP_GetRole -> string
static bool Cmd_TES4MP_GetRole_Execute(COMMAND_ARGS) {
    return AssignStr(PASS_COMMAND_ARGS, g_localRole.c_str());
}

// TES4MP_SendQuestStage "questId" stage
static bool Cmd_TES4MP_SendQuestStage_Execute(COMMAND_ARGS) {
    *result = 0;
    char* questId = nullptr;
    int   stage   = 0;

    if (!ExtractArgsEx(paramInfo, arg1, opcodeOffsetPtr, scriptObj, eventList, &questId, &stage))
        return true;

    std::string pkt = json::obj({
        json::str("type",    "QUEST_STAGE"),
        json::str("questId", questId ? questId : ""),
        json::num("stage",   stage),
    });
    g_network.send(pkt);
    *result = 1;
    return true;
}

// TES4MP_SendCommand "text"
static bool Cmd_TES4MP_SendCommand_Execute(COMMAND_ARGS) {
    *result = 0;
    char* text = nullptr;

    if (!ExtractArgsEx(paramInfo, arg1, opcodeOffsetPtr, scriptObj, eventList, &text))
        return true;

    std::string pkt = json::obj({
        json::str("type", "COMMAND"),
        json::str("text", text ? text : ""),
    });
    g_network.send(pkt);
    *result = 1;
    return true;
}

// TES4MP_GetToken -> string (loads/creates persistent token)
static bool Cmd_TES4MP_GetToken_Execute(COMMAND_ARGS) {
    static std::string token;
    if (token.empty()) token = Network::loadOrCreateToken();
    return AssignStr(PASS_COMMAND_ARGS, token.c_str());
}

// TES4MP_GetSyncCount -> int
static bool Cmd_TES4MP_GetSyncCount_Execute(COMMAND_ARGS) {
    *result = (double)g_syncEntries.size();
    return true;
}

// TES4MP_ButtonCallback result:int  — called via RunScriptLine2("TES4MP_ButtonCallback (GetButtonPressed)")
// Delivers the MessageBox button-press result to the C++ auth state machine.
static bool Cmd_TES4MP_ButtonCallback_Execute(COMMAND_ARGS) {
    int val = -1;
    ExtractArgsEx(paramInfo, arg1, opcodeOffsetPtr, scriptObj, eventList, &val);
    GameHooks_SetButtonResult(val);
    *result = 0;
    return true;
}

// TES4MP_GetSyncEntry index -> 1 if valid (loads entry into g_currentPacket fields)
static bool Cmd_TES4MP_GetSyncEntry_Execute(COMMAND_ARGS) {
    int idx = 0;
    if (!ExtractArgsEx(paramInfo, arg1, opcodeOffsetPtr, scriptObj, eventList, &idx)) {
        *result = 0; return true;
    }
    if (idx < 0 || idx >= (int)g_syncEntries.size()) {
        *result = 0; return true;
    }
    g_currentPacket.strField = g_syncEntries[idx].questId;
    g_currentPacket.intField = g_syncEntries[idx].stage;
    *result = 1;
    return true;
}

// ---- Parameter descriptors ----

static ParamInfo kParams_Connect[] = {
    { "host",  kParamType_String,  0 },
    { "port",  kParamType_Integer, 0 },
    { "name",  kParamType_String,  0 },
    { "token", kParamType_String,  0 },
};
static ParamInfo kParams_SendQuestStage[] = {
    { "questId", kParamType_String,  0 },
    { "stage",   kParamType_Integer, 0 },
};
static ParamInfo kParams_SendCommand[] = {
    { "text", kParamType_String, 0 },
};
static ParamInfo kParams_ButtonCallback[] = {
    { "result", kParamType_Integer, 0 },
};
static ParamInfo kParams_GetSyncEntry[] = {
    { "index", kParamType_Integer, 0 },
};

// ---- Registration ----

void RegisterScriptFunctions(OBSEInterface* obse) {
    g_strInterface    = (OBSEStringVarInterface*)obse->QueryInterface(kInterface_StringVar);
    g_scriptInterface = (OBSEScriptInterface*)obse->QueryInterface(kInterface_Script);

    // Register StringVar so our commands can use %z format and return strings
    if (g_strInterface) g_strInterface->Register(g_strInterface);

    obse->SetOpcodeBase(kOpcodeBase);

#define REG(name, helpStr, refReq, nParams, params) \
    do { \
        static CommandInfo ci = { \
            #name, "", 0, helpStr, refReq, nParams, params, \
            Cmd_##name##_Execute, nullptr, nullptr, 0 \
        }; \
        obse->RegisterCommand(&ci); \
    } while(0)

    REG(TES4MP_ButtonCallback,  "Deliver MessageBox result to auth SM", 0, 1, kParams_ButtonCallback);
    REG(TES4MP_Connect,        "Connect to TES4MP server",            0, 4, kParams_Connect);
    REG(TES4MP_IsConnected,    "Returns 1 if connected to server",    0, 0, nullptr);
    REG(TES4MP_PollPacket,     "Dequeue next packet, returns 1/0",    0, 0, nullptr);
    REG(TES4MP_GetPacketType,  "Returns type int of current packet",  0, 0, nullptr);
    REG(TES4MP_GetPacketStr,   "Returns string field of packet",      0, 0, nullptr);
    REG(TES4MP_GetPacketStr2,  "Returns second string field",         0, 0, nullptr);
    REG(TES4MP_GetPacketInt,   "Returns int field of packet",         0, 0, nullptr);
    REG(TES4MP_GetRole,        "Returns local player role string",    0, 0, nullptr);
    REG(TES4MP_SendQuestStage, "Send quest stage update to server",   0, 2, kParams_SendQuestStage);
    REG(TES4MP_SendCommand,    "Send command string to server",       0, 1, kParams_SendCommand);
    REG(TES4MP_GetToken,       "Load or create persistent auth token",0, 0, nullptr);
    REG(TES4MP_GetSyncCount,   "Count of quests in last QUEST_SYNC",  0, 0, nullptr);
    REG(TES4MP_GetSyncEntry,   "Load sync entry by index",            0, 1, kParams_GetSyncEntry);

#undef REG
}
