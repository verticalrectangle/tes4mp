#include "game_hooks.h"
#include "ghost_system.h"
#include "d3d_hook.h"
#include "network.h"
#include "config.h"
#include "json_util.h"
#include "pos_sync.h"
#include "appearance.h"
#include "../include/obse_types.h"
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <sstream>
#include <fstream>

static OBSEConsoleInterface*   g_console   = nullptr;
static OBSEMessagingInterface* g_messaging = nullptr;
static PluginHandle            g_handle    = 0;

static void DBG(const std::string& msg) {
    static std::mutex dbgMtx;
    std::lock_guard<std::mutex> lk(dbgMtx);
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << msg << "\n";
}

// ── Game-thread command queue ──────────────────────────────────────────────────
static std::queue<std::string> g_cmdQueue;
static std::mutex              g_cmdMutex;
static std::atomic<bool>       g_running{false};
static std::thread             g_pollThread;


// ── Ghost NPC system ──────────────────────────────────────────────────────────
// ghost_system.cpp owns all state; slots are filled dynamically via PlaceAtMe + cell scan.

static constexpr int GHOST_SLOTS = 4;

// Parse a float from a JSON string without external library.
static float JF(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    auto p = s.find(k);
    if (p == std::string::npos) return 0.f;
    p += k.size();
    while (p < s.size() && s[p] == ' ') p++;
    return static_cast<float>(std::atof(s.c_str() + p));
}

// (Token persistence is handled by Network::loadOrCreateToken — %APPDATA%\TES4MP\token.txt)

// ── Console command helpers ───────────────────────────────────────────────────

static void RunCmd(const std::string& cmd) {
    if (g_console)
        g_console->RunScriptLine2(cmd.c_str(), nullptr, true);
}

static void EnqueueCmd(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(g_cmdMutex);
    g_cmdQueue.push(cmd);
}

// Public API — used by ghost_system via the GhostCmdFn callback.
void GameHooks_EnqueueCmd(const char* cmd) {
    std::lock_guard<std::mutex> lk(g_cmdMutex);
    g_cmdQueue.push(cmd);
}

// Sanitise a string for use inside a quoted console command argument.
static std::string SanitiseForCmd(const std::string& s, size_t maxLen = 200) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if      (c == '"')  { out += '\''; }
        else if (c == '%')  { out += '%'; out += '%'; }
        else                  out += c;
    }
    if (out.size() > maxLen) out = out.substr(0, maxLen - 3) + "...";
    return out;
}

static void EnqueueMsg(const std::string& msg) {
    static const size_t MAX_LINES = 10;

    size_t start = 0, count = 0;
    while (count < MAX_LINES) {
        size_t nl = msg.find('\n', start);
        std::string line = (nl == std::string::npos)
            ? msg.substr(start) : msg.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            EnqueueCmd("Message \"" + SanitiseForCmd(line) + "\"");
            ++count;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (count == MAX_LINES && msg.find('\n', start) != std::string::npos)
        EnqueueCmd("Message \"[...output truncated]\"");
}

// ── Player name IPC (bridge for ShowNameMenu text capture) ────────────────────
// TESActorBase::TESFullName fullName is at offset 0xA0 (GameForms.h line 1365).
// TESFullName inherits BaseFormComponent (vtable at +0x000), BSStringT name at +0x004.
// BSStringT::m_data (char*) is at +0x000 within BSStringT.
// Combined: baseForm + 0xA4 = char* to player's name string.

static std::string ReadPlayerName() {
    void* player = *(void**)0x00B333C4;
    if (!player) return {};
    void* baseForm = *(void**)((char*)player + 0x01C);
    if (!baseForm) return {};
    char* data = *(char**)((char*)baseForm + 0xA4);
    return data ? std::string(data) : std::string{};
}

// ── Auth state machine ────────────────────────────────────────────────────────

enum class NetEvent { None, ServerHello };
static std::atomic<NetEvent> g_netEvent{NetEvent::None};
static std::mutex            g_netMutex;

static void SetNetEvent(NetEvent e) {
    std::lock_guard<std::mutex> lk(g_netMutex);
    g_netEvent = e;
}

// Not used for button-polling anymore, kept for scriptfuncs.cpp compatibility.
static std::atomic<int> g_buttonResult{-2};
void GameHooks_SetButtonResult(int v) { g_buttonResult.store(v); }

enum class AuthPhase { Idle, WaitingHello, Done };
static std::atomic<AuthPhase> g_phase{AuthPhase::Idle};

// Drive auth from game thread.
static void TickAuth() {
    NetEvent ev;
    {
        std::lock_guard<std::mutex> lk(g_netMutex);
        ev = g_netEvent.exchange(NetEvent::None);
    }

    if (ev == NetEvent::ServerHello && g_phase == AuthPhase::WaitingHello) {
        std::string tok  = Network::loadOrCreateToken();
        std::string name = ReadPlayerName();
        if (name.empty()) name = "Adventurer";
        DBG("TickAuth: sending HELLO name=" + name);
        g_network.send(json::obj({
            json::str("type",  "HELLO"),
            json::str("token", tok),
            json::str("name",  name),
        }));
        // Stays in WaitingHello until server replies with CHAR_LOAD
    }
}

// ── Periodic stat checkpoint ──────────────────────────────────────────────────

static const char* SKILL_NAMES[21] = {
    "Armorer","Athletics","Blade","Block","Blunt",
    "Hand To Hand","Heavy Armor","Alchemy","Alteration",
    "Conjuration","Destruction","Illusion","Mysticism","Restoration",
    "Acrobatics","Light Armor","Marksman","Mercantile",
    "Security","Sneak","Speechcraft"
};
static const char* ATTR_NAMES[8] = {
    "Strength","Intelligence","Willpower","Agility",
    "Speed","Endurance","Personality","Luck"
};

struct StatSnapshot { int skills[21]; int attrs[8]; std::string name; bool valid; };
static StatSnapshot    g_lastSent       = {};
static AppearanceData  g_lastAppearance = {};

static bool ReadAndSendCharSave() {
    void* player = *(void**)0x00B333C4;
    if (!player) return false;
    void* baseForm = *(void**)((char*)player + 0x01C);
    if (!baseForm) return false;

    typedef float (__fastcall *GetActorValueFn)(void*, void*, unsigned int);
    GetActorValueFn getAV = ((GetActorValueFn*)(*(void***)player))[0xA1];

    int   level   = (int)(*(short*)((char*)baseForm + 0x032));
    float posX    = *(float*)((char*)player + 0x02C);
    float posY    = *(float*)((char*)player + 0x030);
    float posZ    = *(float*)((char*)player + 0x034);
    int   health  = (int)getAV(player, nullptr, 8);
    int   magicka = (int)getAV(player, nullptr, 9);
    int   stamina = (int)getAV(player, nullptr, 10);

    int curSkills[21], curAttrs[8];
    for (int i = 0; i < 21; ++i) curSkills[i] = (int)getAV(player, nullptr, 0x0C + i);
    for (int i = 0; i < 8;  ++i) curAttrs[i]  = (int)getAV(player, nullptr, i);

    bool skillsDirty = !g_lastSent.valid;
    if (!skillsDirty)
        for (int i = 0; i < 21 && !skillsDirty; ++i)
            if (curSkills[i] != g_lastSent.skills[i]) skillsDirty = true;

    bool attrsDirty = !g_lastSent.valid;
    if (!attrsDirty)
        for (int i = 0; i < 8 && !attrsDirty; ++i)
            if (curAttrs[i] != g_lastSent.attrs[i]) attrsDirty = true;

    std::string curName = ReadPlayerName();

    std::ostringstream pkt;
    pkt << "{\"type\":\"CHAR_SAVE\""
        << ",\"level\":"   << level
        << ",\"health\":"  << health
        << ",\"magicka\":" << magicka
        << ",\"stamina\":" << stamina
        << ",\"pos_x\":"   << posX
        << ",\"pos_y\":"   << posY
        << ",\"pos_z\":"   << posZ
        << ",\"name\":\""  << SanitiseForCmd(curName) << "\"";

    if (skillsDirty) {
        pkt << ",\"skills\":{";
        for (int i = 0; i < 21; ++i) {
            if (i) pkt << ',';
            pkt << '"' << SKILL_NAMES[i] << "\":" << curSkills[i];
        }
        pkt << '}';
    }
    if (attrsDirty) {
        pkt << ",\"attributes\":{";
        for (int i = 0; i < 8; ++i) {
            if (i) pkt << ',';
            pkt << '"' << ATTR_NAMES[i] << "\":" << curAttrs[i];
        }
        pkt << '}';
    }
    pkt << '}';

    g_network.send(pkt.str());
    memcpy(g_lastSent.skills, curSkills, sizeof(curSkills));
    memcpy(g_lastSent.attrs,  curAttrs,  sizeof(curAttrs));
    g_lastSent.name  = curName;
    g_lastSent.valid = true;

    // Resync appearance whenever it changes (covers character creation screen updates)
    AppearanceData ap = Appearance_ReadLocal();
    if (ap.valid && memcmp(&ap, &g_lastAppearance, sizeof(ap)) != 0) {
        g_network.send(Appearance_ToJson(ap));
        g_lastAppearance = ap;
    }

    return true;
}

// ── CHAR_LOAD application ─────────────────────────────────────────────────────

static void ApplyActorValues(const std::string& raw, const std::string& key) {
    std::string marker = "\"" + key + "\":{";
    auto pos = raw.find(marker);
    if (pos == std::string::npos) return;
    pos += marker.size();
    auto end = raw.find('}', pos);
    if (end == std::string::npos) return;
    std::string block = raw.substr(pos, end - pos);

    size_t i = 0;
    while (i < block.size()) {
        auto q1 = block.find('"', i);
        if (q1 == std::string::npos) break;
        auto q2 = block.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string name = block.substr(q1 + 1, q2 - q1 - 1);

        auto colon = block.find(':', q2);
        if (colon == std::string::npos) break;
        size_t ns = colon + 1;
        while (ns < block.size() && block[ns] == ' ') ns++;
        size_t ne = ns;
        while (ne < block.size() && (std::isdigit(block[ne]) || block[ne] == '-' || block[ne] == '.')) ne++;
        if (ne > ns)
            EnqueueCmd("player.setav " + name + " " + block.substr(ns, ne - ns));
        i = ne + 1;
    }
}

static void ApplyCharLoad(const std::string& raw) {
    DBG("ApplyCharLoad: " + raw.substr(0, 120));

    g_phase = AuthPhase::Done;
    PosSync_Start();

    bool isNew = json::getBool(raw, "is_new");
    if (!isNew) {
        // Returning player — restore their saved state.
        int level = json::getInt(raw, "level");
        if (level > 0)
            EnqueueCmd("player.setlevel " + std::to_string(level));

        int gold = json::getInt(raw, "gold");
        if (gold > 0)
            EnqueueCmd("player.additem gold001 " + std::to_string(gold));

        ApplyActorValues(raw, "skills");
        ApplyActorValues(raw, "attributes");

        std::string cell = json::getStr(raw, "cell");
        if (!cell.empty())
            EnqueueCmd("coc \"" + SanitiseForCmd(cell) + "\"");
    } else {
        // New character — server tells us where to start.
        std::string startQuest = json::getStr(raw, "start_quest");
        int         startStage = json::getInt(raw, "start_quest_stage");
        std::string startCell  = json::getStr(raw, "start_cell");
        if (!startQuest.empty() && startStage > 0)
            EnqueueCmd("setstage " + startQuest + " " + std::to_string(startStage));
        if (!startCell.empty())
            EnqueueCmd("coc " + startCell);
    }

    std::string name = json::getStr(raw, "name");
    EnqueueCmd("Message \"[TES4MP] Connected as " + SanitiseForCmd(name) + "\"");
}

// ── Background poll loop ───────────────────────────────────────────────────────

static void PollLoop() {
    DWORD lastPing = 0;
    while (g_running) {
        // Keepalive — lets server detect crashes within 15s
        DWORD now = GetTickCount();
        if (g_network.isConnected() && (now - lastPing) >= 5000) {
            g_network.send("{\"type\":\"PING\"}");
            lastPing = now;
        }

        Packet pkt;
        while (g_network.pollPacket(pkt)) {
            switch (pkt.type) {

            // Auth events → signal game thread
            case PacketType::ServerHello:
                DBG("PollLoop: got ServerHello");
                SetNetEvent(NetEvent::ServerHello);
                break;


            // Character state applied on game thread via queue
            case PacketType::CharLoad:
                // EnqueueCmd is thread-safe; ApplyCharLoad calls it internally
                ApplyCharLoad(pkt.raw);
                break;

            // Quest sync
            case PacketType::QuestStage: {
                std::ostringstream ss; ss << "setstage " << pkt.strField << " " << pkt.intField;
                EnqueueCmd(ss.str());
                break;
            }
            case PacketType::QuestSync:
                for (auto& q : json::getQuestArray(pkt.raw)) {
                    std::ostringstream ss; ss << "setstage " << q.questId << " " << q.stage;
                    EnqueueCmd(ss.str());
                }
                break;

            // Admin actions
            case PacketType::GiveItem: {
                std::ostringstream ss; ss << "player.additem " << pkt.strField << " " << pkt.intField;
                EnqueueCmd(ss.str());
                EnqueueMsg("[TES4MP] Received: " + std::to_string(pkt.intField) + "x " + pkt.strField);
                break;
            }
            case PacketType::TakeItem: {
                std::ostringstream ss; ss << "player.removeitem " << pkt.strField << " " << pkt.intField;
                EnqueueCmd(ss.str());
                break;
            }
            case PacketType::AddGold: {
                std::ostringstream ss; ss << "player.additem gold001 " << pkt.intField;
                EnqueueCmd(ss.str());
                break;
            }
            case PacketType::Teleport:
                if (!pkt.strField.empty())
                    EnqueueCmd("coc \"" + pkt.strField + "\"");
                break;
            case PacketType::SetLevel: {
                std::ostringstream ss; ss << "player.setlevel " << pkt.intField;
                EnqueueCmd(ss.str());
                break;
            }
            case PacketType::SetSkill:
            case PacketType::SetAttr: {
                std::ostringstream ss; ss << "player.setav " << pkt.strField << " " << pkt.intField;
                EnqueueCmd(ss.str());
                break;
            }

            // Notifications (routed to chat overlay when in-game, HUD otherwise)
            case PacketType::CommandResult:
            case PacketType::Message:
            case PacketType::ServerAnnouncement:
                EnqueueMsg("[TES4MP] " + pkt.strField);
                break;
            case PacketType::PrivateMsg:
                EnqueueMsg("[PM from " + pkt.strField2 + "] " + pkt.strField);
                break;
            case PacketType::DungeonCleared:
                EnqueueMsg("[TES4MP] " + pkt.strField2 + " cleared " + pkt.strField);
                break;
            case PacketType::PlayerJoin:
                EnqueueMsg("[TES4MP] " + pkt.strField + " joined (" + pkt.strField2 + ")");
                break;
            case PacketType::PlayerLeave:
                EnqueueMsg("[TES4MP] " + pkt.strField + " left");
                break;
            case PacketType::PartyInvite:
                EnqueueMsg("[TES4MP] Party invite from " + pkt.strField +
                         " — type: /party accept " + pkt.strField);
                break;
            case PacketType::Kick:
                EnqueueMsg("[TES4MP] Kicked: " + pkt.strField);
                g_network.disconnect();
                g_phase = AuthPhase::Idle;
                GhostSystem_Shutdown();
                PosSync_Stop();
                break;

            case PacketType::PlayerPos:
                GhostSystem_OnPosUpdate(
                    pkt.strField,
                    JF(pkt.raw, "x"), JF(pkt.raw, "y"), JF(pkt.raw, "z"),
                    JF(pkt.raw, "rot"), (int)JF(pkt.raw, "anim"));
                break;

            case PacketType::GhostAppear: {
                std::string charName = json::getStr(pkt.raw, "char_name");
                // "race" and "gender" live inside the appearance sub-object
                auto raceId = static_cast<uint32_t>(JF(pkt.raw, "race"));
                auto gender = static_cast<int>(JF(pkt.raw, "gender"));
                GhostSystem_OnAppear(
                    pkt.strField, charName, raceId, gender,
                    JF(pkt.raw, "x"), JF(pkt.raw, "y"), JF(pkt.raw, "z"),
                    JF(pkt.raw, "rot"), (int)JF(pkt.raw, "anim"));
                if (!charName.empty())
                    EnqueueMsg("[TES4MP] " + charName + " is nearby.");
                break;
            }

            case PacketType::GhostLeave:
                GhostSystem_OnLeave(pkt.strField);
                break;

            default:
                break;
            }
        }

        Sleep(100);
    }
}

// ── OBSE messaging callback ───────────────────────────────────────────────────

static void AttemptConnect() {
    if (g_network.isConnected()) return;
    Config cfg = LoadConfig();
    DBG("connecting to " + cfg.host + ":" + std::to_string(cfg.port));
    if (g_network.connect(cfg.host, cfg.port)) {
        DBG("connect OK");
        g_phase = AuthPhase::WaitingHello;
    } else {
        DBG("connect FAILED");
        EnqueueMsg("[TES4MP] Could not connect to " + cfg.host + ":" + std::to_string(cfg.port));
    }
}

static void OnOBSEMessage(OBSEMessagingInterface::Message* msg) {
    DBG("OnOBSEMessage type=" + std::to_string(msg->type));

    if (msg->type == OBSEMessagingInterface::kMessage_PostLoadGame) {
        DBG("PostLoadGame fired");
        AttemptConnect();
    } else if (msg->type == OBSEMessagingInterface::kMessage_SaveGame &&
               g_phase == AuthPhase::Idle) {
        // First save of a new game (jail cell auto-save after character creation).
        // Character name and appearance are already set by the creation screen.
        DBG("SaveGame fired (Idle) — connecting");
        AttemptConnect();
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void GameHooks_Init(OBSEInterface* obse, PluginHandle pluginHandle) {
    g_handle    = pluginHandle;
    g_console   = static_cast<OBSEConsoleInterface*>(obse->QueryInterface(kInterface_Console));
    g_messaging = static_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));

    if (g_messaging)
        g_messaging->RegisterListener(g_handle, "OBSE", OnOBSEMessage);

    // Ghost system: slots filled dynamically via PlaceAtMe + cell scan on GHOST_APPEAR.
    GhostSystem_Init(GHOST_SLOTS, GameHooks_EnqueueCmd);
    D3DHook_Init(GhostSystem_OnFrame);

    g_running    = true;
    g_pollThread = std::thread(PollLoop);
}

void GameHooks_Tick() {
    // Drain queued console commands (game thread only)
    {
        std::lock_guard<std::mutex> lk(g_cmdMutex);
        while (!g_cmdQueue.empty()) {
            std::string cmd = g_cmdQueue.front();
            g_cmdQueue.pop();
            DBG("RunCmd: " + cmd);
            RunCmd(cmd);
            DBG("RunCmd done");
        }
    }

    // Drive auth state machine
    if (g_phase == AuthPhase::WaitingHello)
        TickAuth();

    // Periodic stat checkpoint — must run on game thread (vtable calls into engine)
    if (g_phase == AuthPhase::Done && g_network.isConnected()) {
        static DWORD lastSave = 0;
        DWORD now = GetTickCount();
        if (lastSave == 0 || (now - lastSave) >= 15000) {
            ReadAndSendCharSave();
            lastSave = now;
        }
    }

    // Ghost anim commands generated by GhostSystem_OnFrame are already in the queue above;
    // GhostSystem_OnFrame itself runs from the D3D9 Present hook for per-frame position writes.
}

void GameHooks_Shutdown() {
    PosSync_Stop();
    GhostSystem_Shutdown();
    D3DHook_Shutdown();
    g_running = false;
    if (g_pollThread.joinable()) g_pollThread.join();
}
