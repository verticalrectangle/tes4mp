#include "game_hooks.h"
#include "ghost_system.h"
#include "npc_sync.h"
#include "npc_spawn_sync.h"
#include "equip_sync.h"
#include "quest_sync.h"
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
#include <vector>
#include <cmath>
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
    f << "[" << (GetTickCount() / 1000) << "s] " << msg << "\n";
}

// ── Game-thread command queue ──────────────────────────────────────────────────
struct QueuedCmd {
    void*       refr;  // executes on this ref (null = global/console context)
    std::string cmd;
};
static std::queue<QueuedCmd>   g_cmdQueue;
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

// Parse an array of unsigned ints for a key, e.g. "items":[123,456].
// Tolerates {} (cjson encodes an empty Lua table as an object).
static std::vector<uint32_t> ParseUintArray(const std::string& s, const char* key) {
    std::vector<uint32_t> out;
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return out;
    p = s.find_first_of("[{", p + k.size());
    if (p == std::string::npos) return out;
    char close = (s[p] == '[') ? ']' : '}';
    size_t end = s.find(close, p);
    if (end == std::string::npos) return out;
    size_t i = p + 1;
    while (i < end) {
        while (i < end && !std::isdigit((unsigned char)s[i])) ++i;
        if (i >= end) break;
        char* e;
        unsigned long v = strtoul(s.c_str() + i, &e, 10);
        out.push_back((uint32_t)v);
        i = (size_t)(e - s.c_str());
    }
    return out;
}

// Parse an array of strings for a key, e.g. "monitored":["MQ00","MQ01"].
static std::vector<std::string> ParseStrArray(const std::string& s, const char* key) {
    std::vector<std::string> out;
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return out;
    p = s.find('[', p + k.size());
    if (p == std::string::npos) return out;
    size_t end = s.find(']', p);
    if (end == std::string::npos) return out;
    size_t i = p;
    while (i < end) {
        size_t q1 = s.find('"', i + 1);
        if (q1 == std::string::npos || q1 > end) break;
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > end) break;
        out.push_back(s.substr(q1 + 1, q2 - q1 - 1));
        i = q2;
    }
    return out;
}

// Parse NPC_SPAWNS payload: "spawns":[{"sid":..,"base":..,"x":..,"y":..,"z":..,"hp":..},...]
static std::vector<SpawnEntry> ParseSpawnArray(const std::string& s) {
    std::vector<SpawnEntry> out;
    size_t p = s.find("\"spawns\"");
    if (p == std::string::npos) return out;
    while (true) {
        size_t sp = s.find("\"sid\"", p);
        if (sp == std::string::npos) break;
        size_t objEnd = s.find('}', sp);
        if (objEnd == std::string::npos) break;
        std::string obj = s.substr(sp, objEnd - sp);
        SpawnEntry e{};
        e.sid  = (uint32_t)strtoul(obj.c_str() + 6, nullptr, 10);
        e.base = (uint32_t)(int)JF(obj, "base");
        e.x    = JF(obj, "x");
        e.y    = JF(obj, "y");
        e.z    = JF(obj, "z");
        e.hp   = (int)JF(obj, "hp");
        if (e.sid && e.base) out.push_back(e);
        p = objEnd;
    }
    return out;
}

// Parse NPC_HP payload: "npcs":[{"ref":N,"hp":M},...]
static std::vector<NpcHpEntry> ParseNpcHpArray(const std::string& s) {
    std::vector<NpcHpEntry> out;
    size_t p = s.find("\"npcs\"");
    if (p == std::string::npos) return out;
    size_t end = s.find(']', p);
    if (end == std::string::npos) return out;
    while (true) {
        size_t rp = s.find("\"ref\"", p);
        if (rp == std::string::npos || rp > end) break;
        size_t hp = s.find("\"hp\"", rp);
        if (hp == std::string::npos || hp > end) break;
        NpcHpEntry e;
        e.ref = (uint32_t)strtoul(s.c_str() + rp + 6, nullptr, 10);
        e.hp  = (int)strtol(s.c_str() + hp + 5, nullptr, 10);
        out.push_back(e);
        p = hp + 5;
    }
    return out;
}

// (Token persistence is handled by Network::loadOrCreateToken — %APPDATA%\TES4MP\token.txt)

// ── Console command helpers ───────────────────────────────────────────────────

static void RunCmd(const std::string& cmd, void* refr = nullptr) {
    if (!g_console) return;
    bool ok = g_console->RunScriptLine2(cmd.c_str(), (TESObjectREFR*)refr, true);
    if (!ok)
        DBG("RunCmd FAILED: " + cmd + (refr ? " (on ref)" : ""));
}

static void EnqueueCmd(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(g_cmdMutex);
    g_cmdQueue.push({ nullptr, cmd });
}

// Public API — used by ghost_system via the GhostCmdFn callback.
void GameHooks_EnqueueCmd(const char* cmd) {
    std::lock_guard<std::mutex> lk(g_cmdMutex);
    g_cmdQueue.push({ nullptr, cmd });
}

void GameHooks_EnqueueCmdOnRef(void* refr, const char* cmd) {
    std::lock_guard<std::mutex> lk(g_cmdMutex);
    g_cmdQueue.push({ refr, cmd });
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

static uint32_t SampleWeatherId() {
    // Sky singleton global pointer — Oblivion 1.2.416 (same pattern as kPlayerPtr)
    static constexpr uintptr_t kSkyPtr = 0x00B13A94;
    void* sky = *(void**)kSkyPtr;
    if (!sky) return 0;
    void* weather = *(void**)((char*)sky + 0x018);  // Sky::weather018
    if (!weather) return 0;
    return *(uint32_t*)((char*)weather + 0x00C);    // TESForm::refID
}

static int SamplePlayerHp() {
    void* player = *(void**)0x00B333C4;
    if (!player) return 0;
    typedef float(__fastcall* GetAVFn)(void*, void*, unsigned int);
    GetAVFn fn = ((GetAVFn*)(*(void***)player))[0xA1];
    return (int)fn(player, nullptr, 8);
}

static std::string ReadPlayerName() {
    void* player = *(void**)0x00B333C4;
    if (!player) return {};
    void* baseForm = *(void**)((char*)player + 0x01C);
    if (!baseForm) return {};
    char* data = *(char**)((char*)baseForm + 0xA4);
    return data ? std::string(data) : std::string{};
}

// True once a world is loaded (player exists and sits in a cell) — the
// earliest point where connecting and running console commands is safe.
static bool InWorld() {
    void* player = *(void**)0x00B333C4;
    if (!player) return false;
    return *(void**)((char*)player + 0x040) != nullptr;  // kRef_parentCell
}

// Oblivion 1.2.416: bool IsMenuMode() — same routine xOBSE binds at 0x00578F60.
// Only safe to CALL from the SetTimer tick (message-pump/game thread, which
// stalls during loads). The Present hook must not call engine code mid-load,
// so it reads the cached sample below with a freshness check instead.
static bool GameIsMenuMode() {
    return ((unsigned char(__cdecl*)())0x00578F60)() != 0;
}

static std::atomic<bool>  g_menuModeCached{true};
static std::atomic<DWORD> g_lastTickMs{0};
static std::atomic<DWORD> g_transitionGuardUntil{0};

// Raised the moment we issue a teleport (coc/cow): the tick-staleness check
// has a ~500ms blind window at the start of a load, and ghost/NPC scans in
// that window walk a world that is being torn down.
static void BeginTransitionGuard(DWORD ms) {
    g_transitionGuardUntil.store(GetTickCount() + ms);
}

// Current interior cell's editor ID via ExtraEditorID (empty if none).
// Cell ExtraDataList sits at cell+0x028; BaseExtraList::m_data at +0x004.
static std::string CurrentCellEditorId() {
    void* player = *(void**)0x00B333C4;
    if (!player) return {};
    void* cell = *(void**)((char*)player + 0x040);
    if (!cell) return {};
    void* ed = *(void**)((char*)cell + 0x02C);
    while (ed) {
        uint8_t t = ((uint8_t(__fastcall*)(void*))(*(void***)ed)[1])(ed);
        if (t == 0x0A) {  // kExtraData_EditorID
            const char* s = *(const char**)((char*)ed + 0x008);
            return s ? std::string(s) : std::string{};
        }
        ed = *(void**)((char*)ed + 0x004);
    }
    return {};
}

// Join teleport, executed from the tick once the world is live. coc into the
// cell the player is already standing in unloads it mid-frame and crashes —
// the editor-ID check skips that case.
static std::mutex  g_cocMtx;
static std::string g_pendingCoc;

static void SetPendingCoc(const std::string& cellId) {
    std::lock_guard<std::mutex> lk(g_cocMtx);
    g_pendingCoc = cellId;
}

static void TickPendingCoc() {
    std::string target;
    {
        std::lock_guard<std::mutex> lk(g_cocMtx);
        if (g_pendingCoc.empty()) return;
        target = g_pendingCoc;
        g_pendingCoc.clear();
    }
    // "cow ..." (exterior worldspace teleport) passes through unchecked
    if (target.rfind("cow ", 0) == 0) {
        BeginTransitionGuard(3000);  // scans pause while the world streams in
        EnqueueCmd(target);
        return;
    }
    std::string cur = CurrentCellEditorId();
    if (!cur.empty() && _stricmp(cur.c_str(), target.c_str()) == 0) {
        DBG("TickPendingCoc: already in " + target + ", skipping coc");
        return;
    }
    BeginTransitionGuard(3000);
    EnqueueCmd("coc \"" + target + "\"");
}

// Cached cell editor ID, sampled on the safe tick — read by pos_sync's thread.
static std::mutex  g_cellEdMtx;
static std::string g_cellEdCached;

std::string GameHooks_GetCellEditorId() {
    std::lock_guard<std::mutex> lk(g_cellEdMtx);
    return g_cellEdCached;
}

// Strict gate — for memory WALKS only (cell lists, extra data, ghost refs).
// These crash against a world mid-load/teardown.
bool GameHooks_IsSafeToScan() {
    if (!InWorld()) return false;
    DWORD now = GetTickCount();
    if ((int)(g_transitionGuardUntil.load() - now) > 0) return false;
    // If the game tick hasn't run recently the game is loading (WM_TIMER
    // doesn't pump) — treat as unsafe rather than trust a stale sample.
    DWORD age = now - g_lastTickMs.load();
    if (age > 500) return false;
    return !g_menuModeCached.load();
}

// Loose gate — for console command execution. Commands don't walk memory;
// they only need a live world. (Loading screens don't pump WM_TIMER, so the
// tick — and therefore the queue — naturally stalls during real loads.)
static bool SafeToRunCmds() {
    return InWorld();
}

// ── Auth state machine ────────────────────────────────────────────────────────

enum class NetEvent { None, ServerHello };
static std::atomic<NetEvent> g_netEvent{NetEvent::None};
static std::mutex            g_netMutex;

static void SetNetEvent(NetEvent e) {
    std::lock_guard<std::mutex> lk(g_netMutex);
    g_netEvent = e;
}

static std::atomic<int> g_playerHp{0};
int GameHooks_GetPlayerHp() { return g_playerHp.load(); }

// Not used for button-polling anymore, kept for scriptfuncs.cpp compatibility.
static std::atomic<int> g_buttonResult{-2};
void GameHooks_SetButtonResult(int v) { g_buttonResult.store(v); }

enum class AuthPhase { Idle, WaitingHello, Done };
static std::atomic<AuthPhase> g_phase{AuthPhase::Idle};
static std::atomic<bool>      g_sendInitialSave{false};

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
        // Pre-chargen join: no name yet. Use a token-derived placeholder
        // (names are UNIQUE server-side); CHAR_SAVE renames once chargen ends.
        if (name.empty()) name = "Adventurer-" + tok.substr(0, 4);
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
    g_playerHp.store(health);
    int   magicka = (int)getAV(player, nullptr, 9);
    int   stamina = (int)getAV(player, nullptr, 10);

    int curSkills[21], curAttrs[8];
    for (int i = 0; i < 21; ++i) curSkills[i] = (int)getAV(player, nullptr, 0x0C + i);
    for (int i = 0; i < 8;  ++i) curAttrs[i]  = (int)getAV(player, nullptr, i);

    // Loading screens / mid-chargen can report every AV as 0. Persisting that
    // bricks the character (Strength 0 = zero carry weight = can't move on the
    // next CHAR_LOAD). Skip the checkpoint entirely — a later tick sends real data.
    {
        int attrSum = 0;
        for (int i = 0; i < 8; ++i) attrSum += curAttrs[i];
        if (attrSum == 0) return false;
    }

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

// ── Server-driven character creation ──────────────────────────────────────────
// A new game connects immediately (readiness poll below); the server owns the
// start. We teleport to the server start cell and chain the chargen menus
// ourselves — no vanilla tutorial, no autosave dependency. Menu transitions are
// detected via the engine's IsMenuMode() (0x00578F60, xOBSE binding).

struct ChargenState {
    enum Step { Inactive, WaitVanillaMenu, Teleport, WaitWorld,
                NextMenu, WaitMenuOpen, WaitMenuClose, Finish };
    Step        step        = Inactive;
    bool        includeRace = true;   // false if the vanilla race menu already ran
    int         menuIdx     = 0;
    DWORD       stepMs      = 0;
    DWORD       beganMs     = 0;
    std::string startCell, startQuest;
    int         startStage  = 0;
};
static ChargenState g_chargen;
static std::mutex   g_chargenMtx;

static const char* kChargenMenus[4] = {
    "ShowRaceMenu", "ShowClassMenu", "ShowBirthsignMenu", "ShowNameMenu"
};

static void ChargenBegin(const std::string& cell, const std::string& quest, int stage) {
    std::lock_guard<std::mutex> lk(g_chargenMtx);
    g_chargen            = {};
    g_chargen.step       = ChargenState::WaitVanillaMenu;
    g_chargen.startCell  = cell;
    g_chargen.startQuest = quest;
    g_chargen.startStage = stage;
    g_chargen.beganMs    = GetTickCount();
    DBG("ChargenBegin cell=" + cell);
}

static void ChargenCancel() {
    std::lock_guard<std::mutex> lk(g_chargenMtx);
    g_chargen.step = ChargenState::Inactive;
}


static void TickChargen() {  // game thread only (IsMenuMode + console cmds)
    std::lock_guard<std::mutex> lk(g_chargenMtx);
    ChargenState& c = g_chargen;
    if (c.step == ChargenState::Inactive) return;

    DWORD now = GetTickCount();
    // Failsafe: never wedge the join forever (menus stuck / IsMenuMode misread)
    if (c.step != ChargenState::Finish && now - c.beganMs > 10u * 60u * 1000u)
        c.step = ChargenState::Finish;

    switch (c.step) {
    case ChargenState::WaitVanillaMenu:
        // A new game opens the race menu on its own — let the player finish it.
        if (GameIsMenuMode()) { c.includeRace = false; return; }
        c.step = ChargenState::Teleport;
        return;

    case ChargenState::Teleport:
        if (!c.startQuest.empty() && c.startStage > 0)
            EnqueueCmd("setstage " + c.startQuest + " " + std::to_string(c.startStage));
        if (!c.startCell.empty()) {
            BeginTransitionGuard(3000);
            EnqueueCmd("coc " + c.startCell);
        }
        c.stepMs = now;
        c.step   = ChargenState::WaitWorld;
        return;

    case ChargenState::WaitWorld:
        if (now - c.stepMs < 3000) return;  // let the cell load settle
        c.menuIdx = c.includeRace ? 0 : 1;
        c.step    = ChargenState::NextMenu;
        return;

    case ChargenState::NextMenu:
        if (c.menuIdx >= 4) { c.step = ChargenState::Finish; return; }
        EnqueueCmd(kChargenMenus[c.menuIdx]);
        c.stepMs = now;
        c.step   = ChargenState::WaitMenuOpen;
        return;

    case ChargenState::WaitMenuOpen:
        if (GameIsMenuMode()) { c.step = ChargenState::WaitMenuClose; return; }
        if (now - c.stepMs > 5000) c.step = ChargenState::NextMenu;  // re-issue
        return;

    case ChargenState::WaitMenuClose:
        if (GameIsMenuMode()) return;  // player still choosing
        c.menuIdx++;
        c.step = ChargenState::NextMenu;
        return;

    case ChargenState::Finish:
        c.step = ChargenState::Inactive;
        g_sendInitialSave = true;  // upload real starting stats now
        EnqueueCmd("Message \"[TES4MP] Character created - welcome!\"");
        return;

    case ChargenState::Inactive:
        return;
    }
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
        // Never restore a value <= 0 — legit saves don't contain them, and a
        // corrupted checkpoint (e.g. Strength 0) would freeze the player.
        if (ne > ns && strtol(block.c_str() + ns, nullptr, 10) > 0)
            EnqueueCmd("player.setav " + name + " " + block.substr(ns, ne - ns));
        i = ne + 1;
    }
}

static void ApplyCharLoad(const std::string& raw) {
    DBG("ApplyCharLoad: " + raw.substr(0, 120));

    g_phase = AuthPhase::Done;
    PosSync_Start();

    // Server PvP mode — ghosts become hittable and local hits are reported
    GhostSystem_SetPvp(json::getBool(raw, "pvp"));

    // Quests the server wants stage reports for
    QuestSync_SetMonitored(ParseStrArray(raw, "monitored"));

    bool isNew = json::getBool(raw, "is_new");
    if (!isNew) {
        // Returning player — restore their saved state.
        // Gold is intentionally NOT re-applied: the local save already carries it,
        // and additem on every join duplicated it session after session.
        int level = json::getInt(raw, "level");
        if (level > 0)
            EnqueueCmd("player.setlevel " + std::to_string(level));

        ApplyActorValues(raw, "skills");
        ApplyActorValues(raw, "attributes");

        std::string cell = json::getStr(raw, "cell");
        if (!cell.empty())
            SetPendingCoc(SanitiseForCmd(cell));  // deferred + same-cell-safe
    } else {
        // New character — server tells us where to start.
        int gold = json::getInt(raw, "gold");
        if (gold > 0)
            EnqueueCmd("player.additem gold001 " + std::to_string(gold));

        std::string startQuest = json::getStr(raw, "start_quest");
        int         startStage = json::getInt(raw, "start_quest_stage");
        std::string startCell  = json::getStr(raw, "start_cell");

        // No player name yet = chargen hasn't finished — take over the new-game
        // flow: teleport to the server start and chain the chargen menus.
        std::string curName = ReadPlayerName();
        if (curName.empty() || curName == "Prisoner") {
            ChargenBegin(startCell, startQuest, startStage);
        } else {
            // Chargen already complete (joined after character creation).
            if (!startQuest.empty() && startStage > 0)
                EnqueueCmd("setstage " + startQuest + " " + std::to_string(startStage));
            if (!startCell.empty())
                EnqueueCmd("coc " + startCell);
            // Upload actual starting stats — race/class/birthsign already applied.
            g_sendInitialSave = true;
        }
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
                if ((int)JF(pkt.raw, "cow") == 1) {
                    // Exterior target: cow <worldspace> <cellX> <cellY>
                    int ws = (int)JF(pkt.raw, "ws");
                    int cx = (int)JF(pkt.raw, "cx");
                    int cy = (int)JF(pkt.raw, "cy");
                    if (ws == 60) {  // Tamriel (0x3C) — the only mapped worldspace
                        std::ostringstream ss;
                        ss << "cow Tamriel " << cx << " " << cy;
                        SetPendingCoc(ss.str());
                    } else {
                        EnqueueMsg("[TES4MP] Target is in an unmapped worldspace.");
                    }
                } else if (!pkt.strField.empty()) {
                    SetPendingCoc(SanitiseForCmd(pkt.strField));
                }
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
                EquipSync_Reset();
                QuestSync_Reset();
                ChargenCancel();
                break;

            case PacketType::PlayerPos:
                GhostSystem_OnPosUpdate(
                    pkt.strField,
                    JF(pkt.raw, "x"), JF(pkt.raw, "y"), JF(pkt.raw, "z"),
                    JF(pkt.raw, "rot"), (int)JF(pkt.raw, "anim"), (int)JF(pkt.raw, "hp"));
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
                {
                    auto equip = ParseUintArray(pkt.raw, "equipment");
                    if (!equip.empty())
                        GhostSystem_OnEquip(pkt.strField, equip.data(), (int)equip.size());
                }
                if (!charName.empty()) {
                    std::string cellName = json::getStr(pkt.raw, "cell_name");
                    bool isFastTravel    = (JF(pkt.raw, "fast_travel") != 0.f);
                    if (isFastTravel && !cellName.empty())
                        EnqueueMsg("[TES4MP] " + charName + " fast-traveled to " + cellName + ".");
                    else
                        EnqueueMsg("[TES4MP] " + charName + " is nearby.");
                }
                break;
            }

            case PacketType::GhostLeave:
                GhostSystem_OnLeave(pkt.strField);
                break;

            case PacketType::NpcKilled:
                NpcSync_OnKilled((uint32_t)pkt.intField);
                break;

            case PacketType::NpcKillSync: {
                // raw = {"type":"NPC_KILL_SYNC","refs":[refId,...]}
                // Parse array manually (avoid pulling in a full JSON lib on game thread)
                std::vector<uint32_t> refs;
                const std::string& r = pkt.raw;
                size_t p = r.find("\"refs\"");
                if (p != std::string::npos) {
                    size_t lb = r.find('[', p);
                    size_t rb = r.find(']', lb != std::string::npos ? lb : 0);
                    if (lb != std::string::npos && rb != std::string::npos) {
                        std::string arr = r.substr(lb + 1, rb - lb - 1);
                        size_t i = 0;
                        while (i < arr.size()) {
                            while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',')) ++i;
                            if (i >= arr.size()) break;
                            char* end;
                            long v = strtol(arr.c_str() + i, &end, 10);
                            if (end == arr.c_str() + i) break;
                            refs.push_back((uint32_t)v);
                            i = (size_t)(end - arr.c_str());
                        }
                    }
                }
                if (!refs.empty())
                    NpcSync_OnKillSync(refs.data(), (int)refs.size());
                break;
            }

            case PacketType::ItemSync: {
                auto cref  = (uint32_t)(int)JF(pkt.raw, "container_ref_id");
                auto iform = (uint32_t)(int)JF(pkt.raw, "item_form_id");
                auto cnt   = (int)JF(pkt.raw, "count");
                if (cref && iform && cnt > 0)
                    NpcSync_OnItemSync(cref, iform, cnt);
                break;
            }

            case PacketType::ContainerState: {
                // raw = {"type":"CONTAINER_STATE","containers":[{"ref_id":N,"items":[{"form_id":M,"count":K}]}]}
                // Simple linear scan — containers are few per cell
                std::vector<ContainerEntry> entries;
                const std::string& s = pkt.raw;
                size_t pos = 0;
                while ((pos = s.find("\"ref_id\"", pos)) != std::string::npos) {
                    pos += 8;
                    char* e1;
                    long cref = strtol(s.c_str() + pos, &e1, 10);
                    if (e1 == s.c_str() + pos) { ++pos; continue; }
                    // find items array for this container
                    size_t itemsStart = s.find("\"items\"", pos);
                    size_t nextRef    = s.find("\"ref_id\"", pos + 1);
                    if (itemsStart == std::string::npos) break;
                    size_t lb2 = s.find('[', itemsStart);
                    size_t rb2 = s.find(']', lb2 != std::string::npos ? lb2 : 0);
                    if (lb2 == std::string::npos || rb2 == std::string::npos) break;
                    if (nextRef != std::string::npos && lb2 > nextRef) { pos = nextRef; continue; }
                    std::string arr2 = s.substr(lb2 + 1, rb2 - lb2 - 1);
                    // parse {form_id:M,count:K} objects
                    size_t j = 0;
                    while (j < arr2.size()) {
                        size_t fpos = arr2.find("\"form_id\"", j);
                        if (fpos == std::string::npos) break;
                        fpos += 9;
                        char* ef; long fid = strtol(arr2.c_str() + fpos, &ef, 10);
                        size_t cpos = arr2.find("\"count\"", fpos);
                        if (cpos == std::string::npos) break;
                        cpos += 7;
                        char* ec; long cnt2 = strtol(arr2.c_str() + cpos, &ec, 10);
                        if (fid > 0 && cnt2 > 0)
                            entries.push_back({ (uint32_t)cref, (uint32_t)fid, (int)cnt2 });
                        j = (size_t)(ec - arr2.c_str());
                    }
                    pos = rb2 + 1;
                }
                if (!entries.empty())
                    NpcSync_OnContainerState(entries.data(), (int)entries.size());
                break;
            }

            case PacketType::PlayerDied:
                // A peer died — despawn their ghost immediately
                GhostSystem_OnLeave(pkt.strField);
                break;

            case PacketType::WeatherSync: {
                char buf[32];
                snprintf(buf, sizeof(buf), "fw %X", (uint32_t)pkt.intField);
                GameHooks_EnqueueCmd(buf);
                break;
            }

            case PacketType::RevealMarkers:
                GameHooks_EnqueueCmd("tmm 1");
                break;

            case PacketType::EquipSync: {
                auto items = ParseUintArray(pkt.raw, "items");
                GhostSystem_OnEquip(pkt.strField, items.data(), (int)items.size());
                break;
            }

            case PacketType::CellAuthority:
                NpcSync_SetAuthority(pkt.strField.c_str(), pkt.intField != 0);
                break;

            case PacketType::NpcHp: {
                auto npcs = ParseNpcHpArray(pkt.raw);
                if (!npcs.empty())
                    NpcSync_OnHpSync(npcs.data(), (int)npcs.size());
                break;
            }

            case PacketType::NpcSpawns: {
                auto spawns = ParseSpawnArray(pkt.raw);
                NpcSpawnSync_OnSnapshot(spawns.data(), (int)spawns.size());
                break;
            }

            case PacketType::NpcDamage:
                NpcSync_OnDamageRequest(
                    (uint32_t)(int)JF(pkt.raw, "ref_id"),
                    (int)JF(pkt.raw, "amount"));
                break;

            case PacketType::DamageTaken: {
                int amount = pkt.intField;
                if (amount > 0 && amount <= 100) {
                    std::ostringstream ss;
                    ss << "player.damageav health " << amount;
                    EnqueueCmd(ss.str());
                    EnqueueMsg("[TES4MP] " + pkt.strField + " hit you for "
                               + std::to_string(amount) + "!");
                }
                break;
            }

            default:
                break;
            }
        }

        Sleep(100);
    }
}

// ── OBSE messaging callback ───────────────────────────────────────────────────

static void AttemptConnect(bool verbose = true) {
    if (g_network.isConnected()) return;
    Config cfg = LoadConfig();
    DBG("connecting to " + cfg.host + ":" + std::to_string(cfg.port));
    if (g_network.connect(cfg.host, cfg.port)) {
        DBG("connect OK");
        g_phase = AuthPhase::WaitingHello;
    } else {
        DBG("connect FAILED");
        if (verbose)
            EnqueueMsg("[TES4MP] Could not connect to " + cfg.host + ":" + std::to_string(cfg.port)
                       + " (F10 to retry)");
    }
}

static void OnOBSEMessage(OBSEMessagingInterface::Message* msg) {
    DBG("OnOBSEMessage type=" + std::to_string(msg->type));

    if (msg->type == OBSEMessagingInterface::kMessage_PostLoadGame) {
        DBG("PostLoadGame fired");
        // Deferred from GameHooks_Init — D3D device exists by now
        D3DHook_Init(GhostSystem_OnFrame);
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
    // D3DHook_Init is deferred to PostLoadGame so it runs after DXVK/wined3d is ready.
    GhostSystem_Init(GHOST_SLOTS, GameHooks_EnqueueCmdOnRef);

    g_running    = true;
    g_pollThread = std::thread(PollLoop);
}

void GameHooks_Tick() {
    // Refresh the scan-safety sample (engine call — safe here, on the pump thread)
    g_menuModeCached.store(InWorld() ? GameIsMenuMode() : true);
    g_lastTickMs.store(GetTickCount());

    // Refresh the cached cell editor ID (~1s cadence; pos_sync reads it)
    if (GameHooks_IsSafeToScan()) {
        static DWORD lastCellEd = 0;
        DWORD now = GetTickCount();
        if (now - lastCellEd >= 1000) {
            lastCellEd = now;
            std::string ed = CurrentCellEditorId();
            std::lock_guard<std::mutex> lk(g_cellEdMtx);
            g_cellEdCached = std::move(ed);
        }
    }

    // Drain queued console commands when a world exists AND no menu is up:
    // lines executed while a (chargen) menu is open fail and are lost —
    // starting gold, tutorial stage and quest sync all vanished that way.
    // They queue and run on the first menu-free tick instead. Chargen's own
    // Show*Menu commands are always enqueued while menus are closed, so no
    // exception is needed.
    if (SafeToRunCmds() && !g_menuModeCached.load()) {
        // Teleports still wait for menus/loads to clear (coc mid-load crashes)
        if (GameHooks_IsSafeToScan()) TickPendingCoc();
        std::lock_guard<std::mutex> lk(g_cmdMutex);
        while (!g_cmdQueue.empty()) {
            QueuedCmd qc = std::move(g_cmdQueue.front());
            g_cmdQueue.pop();
            DBG("RunCmd: " + qc.cmd);
            RunCmd(qc.cmd, qc.refr);
            DBG("RunCmd done");
        }
    }

    // Connection readiness: as soon as a world exists, install the render hook
    // and connect — works for new games (no PostLoadGame ever fires there),
    // loaded saves, and reconnects. F10 forces an immediate retry.
    if (InWorld()) {
        static bool d3dInit = false;
        if (!d3dInit) {
            d3dInit = true;
            D3DHook_Init(GhostSystem_OnFrame);
        }

        static bool  f10Down     = false;
        bool         down        = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool         f10Pressed  = down && !f10Down;
        f10Down = down;

        // F9 = teleport to a fellow player (server picks someone in a cell)
        static bool f9Down = false;
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9 && !f9Down && g_phase == AuthPhase::Done && g_network.isConnected()) {
            g_network.send("{\"type\":\"TP_REQUEST\"}");
            EnqueueMsg("[TES4MP] Teleporting to a party member...");
        }
        f9Down = f9;

        if (g_phase == AuthPhase::Idle && !g_network.isConnected()) {
            static DWORD lastAuto  = 0;
            static bool  firstTry  = true;
            DWORD now = GetTickCount();
            if (f10Pressed) {
                EnqueueMsg("[TES4MP] Connecting...");
                AttemptConnect(true);
            } else if (lastAuto == 0 || now - lastAuto >= 15000) {
                lastAuto = now;
                AttemptConnect(firstTry);  // only announce the first silent failure
                firstTry = false;
            }
        }
    }

    // Server-driven chargen state machine (new characters joining pre-chargen)
    TickChargen();

    // Drive auth state machine
    if (g_phase == AuthPhase::WaitingHello)
        TickAuth();

    // Periodic stat checkpoint — must run on game thread (vtable calls into engine)
    if (g_phase == AuthPhase::Done && g_network.isConnected()) {
        // Immediate upload for new characters: captures race/class/birthsign starting stats
        if (g_sendInitialSave.exchange(false)) {
            ReadAndSendCharSave();
        }

        static DWORD lastSave = 0;
        DWORD now = GetTickCount();
        if (lastSave == 0 || (now - lastSave) >= 15000) {
            ReadAndSendCharSave();
            lastSave = now;
        }
    }

    // Sample HP every tick (~200ms) so ghost health names stay current during combat.
    // Also detect death transition and notify peers so our ghost despawns.
    if (g_phase == AuthPhase::Done && g_network.isConnected()) {
        static int prevHp = 0;
        int hp = SamplePlayerHp();
        g_playerHp.store(hp);
        if (prevHp > 0 && hp <= 0) {
            g_network.send("{\"type\":\"PLAYER_DIED\"}");
            prevHp = 0;
        } else {
            prevHp = hp;
        }
    }

    // Worn-equipment checkpoint (self rate-limited to 2s).
    // Gated on scan safety: walking extra-data lists during a loading screen
    // dereferences half-built structures and crashes.
    if (g_phase == AuthPhase::Done && g_network.isConnected() && GameHooks_IsSafeToScan()) {
        EquipSync_Tick();
    }

    // Quest stage detection → QUEST_STAGE reports (self rate-limited to 5s)
    if (g_phase == AuthPhase::Done && g_network.isConnected() && GameHooks_IsSafeToScan()) {
        QuestSync_Tick();
    }

    // PvP: report HP drops on ghost actors as hits on their owners (~1s)
    if (g_phase == AuthPhase::Done && g_network.isConnected()) {
        static DWORD lastHitPoll = 0;
        DWORD now = GetTickCount();
        if (now - lastHitPoll >= 1000) {
            lastHitPoll = now;
            GhostHit hits[8];
            int n = GhostSystem_PollHits(hits, 8);
            for (int i = 0; i < n; ++i) {
                // charId is server-issued and numeric; embed only if it stays that way
                bool numeric = hits[i].charId[0] != '\0';
                for (const char* c = hits[i].charId; *c && numeric; ++c)
                    if (!std::isdigit((unsigned char)*c)) numeric = false;
                if (!numeric) continue;
                char buf[96];
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"PLAYER_HIT\",\"target_char_id\":%s,\"amount\":%d}",
                    hits[i].charId, hits[i].amount);
                g_network.send(buf);
            }
        }
    }

    // Sample weather and report changes to server for cross-player sync
    if (g_phase == AuthPhase::Done && g_network.isConnected()) {
        static uint32_t lastWeatherId = 0;
        uint32_t wid = SampleWeatherId();
        if (wid != 0 && wid != lastWeatherId) {
            lastWeatherId = wid;
            char buf[48];
            snprintf(buf, sizeof(buf), "{\"type\":\"WEATHER_REPORT\",\"weather_id\":%u}", wid);
            g_network.send(buf);
        }
    }

    // NPC kill + container loot scan — mirrors world.lua cellKey logic
    if (g_phase == AuthPhase::Done && g_network.isConnected()) {
        PlayerState ps = PosSync_GetLocal();
        std::string cellKey;
        if (ps.valid) {
            if (ps.worldspaceFormID == 0) {
                // Interior: key is the cell formID
                if (ps.cellFormID != 0) {
                    char tmp[16];
                    snprintf(tmp, sizeof(tmp), "%u", ps.cellFormID);
                    cellKey = tmp;
                }
            } else {
                // Exterior: bucket by zone (EXTERIOR_ZONE = 4096*3 = 12288)
                static constexpr float kZone = 12288.f;
                int gx = (int)std::floor(ps.x / kZone);
                int gy = (int)std::floor(ps.y / kZone);
                char tmp[48];
                snprintf(tmp, sizeof(tmp), "E%u:%d:%d", ps.worldspaceFormID, gx, gy);
                cellKey = tmp;
            }
        }
        if (GameHooks_IsSafeToScan())
            NpcSync_Tick(cellKey);
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
