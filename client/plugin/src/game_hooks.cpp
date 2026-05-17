#include "game_hooks.h"
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
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include <cmath>

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

// ── Chat overlay ring buffer ───────────────────────────────────────────────────
static constexpr int CHAT_LINES = 8;
static std::string       g_chatLines[CHAT_LINES];
static std::mutex        g_chatMutex;
static std::atomic<bool> g_chatDirty{false};
static std::atomic<bool> g_chatVisible{false};

// ── Ghost NPC system ──────────────────────────────────────────────────────────
// Ghost NPC template FormID — discovered automatically at runtime by scanning
// the first cell that has a non-player NPC (ACHR) present.
static uint32_t g_ghostTemplateFormID = 0;

// Verified from OBSE GameForms.h: TESObjectCELL::objectList at +0x048.
// ObjectListEntry = { TESObjectREFR* refr; ObjectListEntry* next; }
static constexpr ptrdiff_t CELL_OBJLIST_OFF = 0x048;

// Verified from OBSE NiObjects.h: NiAVObject members.
// m_localTranslate at +0x054, m_worldTranslate at +0x088.
// We write both: local persists through UpdateTransforms; world takes effect immediately.
static constexpr ptrdiff_t NINODE_LOCAL_TRANSLATE_X  = 0x054;
static constexpr ptrdiff_t NINODE_LOCAL_TRANSLATE_Y  = 0x058;
static constexpr ptrdiff_t NINODE_LOCAL_TRANSLATE_Z  = 0x05C;
static constexpr ptrdiff_t NINODE_WORLD_TRANSLATE_X  = 0x088;
static constexpr ptrdiff_t NINODE_WORLD_TRANSLATE_Y  = 0x08C;
static constexpr ptrdiff_t NINODE_WORLD_TRANSLATE_Z  = 0x090;

// Memory offsets reused from pos_sync (defined here for ghost write path)
static constexpr ptrdiff_t GH_OFF_REF_BASE    = 0x01C;
static constexpr ptrdiff_t GH_OFF_REF_NINODE  = 0x008;
static constexpr ptrdiff_t GH_OFF_REF_POS_X   = 0x02C;
static constexpr ptrdiff_t GH_OFF_REF_POS_Y   = 0x030;
static constexpr ptrdiff_t GH_OFF_REF_POS_Z   = 0x034;
static constexpr ptrdiff_t GH_OFF_REF_ROT_Z   = 0x040;
static constexpr ptrdiff_t GH_OFF_REF_CELL    = 0x048;
// TESForm layout (verified from OBSE GameForms.h):
// +0x004 typeID (UInt8), +0x008 flags (UInt32), +0x00C refID/formID (UInt32)
static constexpr ptrdiff_t GH_OFF_TYPE_ID     = 0x004;
static constexpr ptrdiff_t GH_OFF_FORM_ID     = 0x00C;
static constexpr uint8_t   FORM_TYPE_ACHR     = 0x32;  // NPC placement ref (kFormType_ACHR)
static constexpr uint8_t   FORM_TYPE_NPC      = 0x23;  // TESNPC base form  (kFormType_NPC)
static constexpr uintptr_t GH_PLAYER_PTR      = 0x00B333C4;

struct GhostState {
    void*  ref;     // TESObjectREFR* — null if ref not yet found
    float  x, y, z, rotZ;
    bool   spawning; // placeatme ran; next tick scans for ref
};

// Ghost states — only accessed from game thread (GameHooks_Tick)
static std::map<std::string, GhostState> g_ghosts;

// Events from background thread to game thread
enum class GhostEvt { Appear, Leave, PosUpdate };
struct GhostEvent {
    GhostEvt    type;
    std::string charId;
    float       x, y, z, rotZ;
    // appearance is left as raw JSON; applied when Phase 2 ref tracking is ready
};
static std::queue<GhostEvent> g_ghostQ;
static std::mutex             g_ghostQMtx;

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

// Push a message into the chat overlay ring buffer.
// Before login (overlay hidden) also shows via the HUD Message notification.
// Thread-safe: may be called from PollLoop.
static void ChatPush(const std::string& msg) {
    if (msg.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_chatMutex);
        for (int i = 0; i < CHAT_LINES - 1; ++i)
            g_chatLines[i] = g_chatLines[i + 1];
        g_chatLines[CHAT_LINES - 1] = msg;
    }
    g_chatDirty = true;

    if (!g_chatVisible) {
        // Overlay not open yet — fall back to the HUD notification system.
        EnqueueCmd("Message \"" + SanitiseForCmd(msg) + "\"");
    }
}

// Auth-phase helper: plain HUD notification (never goes to the chat overlay).
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

// ── Ghost NPC management (game thread only) ───────────────────────────────────

// Collect all refs in the current cell whose base form matches targetFormID.
static std::set<void*> GetRefsOfBaseForm(uint32_t targetFormID) {
    std::set<void*> out;
    if (!CELL_OBJLIST_OFF) return out;  // scanning disabled

    void* player = *(void**)GH_PLAYER_PTR;
    if (!player) return out;
    void* cell = *(void**)((char*)player + GH_OFF_REF_CELL);
    if (!cell) return out;

    struct Node { void* item; Node* next; };
    Node* node = (Node*)((char*)cell + CELL_OBJLIST_OFF);
    while (node) {
        void* ref = node->item;
        if (ref) {
            void* base = *(void**)((char*)ref + GH_OFF_REF_BASE);
            if (base && *(uint32_t*)((char*)base + GH_OFF_FORM_ID) == targetFormID)
                out.insert(ref);
        }
        node = node->next;
    }
    return out;
}

// Auto-discover the ghost template FormID by scanning the current cell for any
// placed NPC reference (typeID == FORM_TYPE_ACHR). We use the first one we find
// as the template — works with any loaded NPC, no console action required.
static void DiscoverGhostTemplate() {
    return; // cell object list layout unverified — disabled until offsets confirmed
    if (g_ghostTemplateFormID) return;
    void* player = *(void**)GH_PLAYER_PTR;
    if (!player) return;
    void* cell = *(void**)((char*)player + GH_OFF_REF_CELL);
    if (!cell) return;

    DBG("DiscoverGhostTemplate: scanning cell=" + std::to_string((uintptr_t)cell));
    struct Node { void* item; Node* next; };
    Node* node = (Node*)((char*)cell + CELL_OBJLIST_OFF);
    int limit = 0;
    while (node && ++limit <= 256) {
        DBG("DiscoverGhostTemplate: node=" + std::to_string((uintptr_t)node)
            + " item=" + std::to_string((uintptr_t)node->item));
        void* ref = node->item;
        if (ref && ref != player) {
            uint8_t tid = *(uint8_t*)((char*)ref + GH_OFF_TYPE_ID);
            DBG("DiscoverGhostTemplate: ref=" + std::to_string((uintptr_t)ref)
                + " tid=" + std::to_string((int)tid));
            if (tid == FORM_TYPE_ACHR) {
                void* base = *(void**)((char*)ref + GH_OFF_REF_BASE);
                if (base) {
                    uint32_t fid = *(uint32_t*)((char*)base + GH_OFF_FORM_ID);
                    if (fid) {
                        g_ghostTemplateFormID = fid;
                        DBG("Ghost template FormID: " + std::to_string(fid));
                        return;
                    }
                }
            }
        }
        node = node->next;
    }
    DBG("DiscoverGhostTemplate: done (limit=" + std::to_string(limit) + ")");
}

// Collect all refs currently claimed by live ghosts (to exclude from scans).
static std::set<void*> ClaimedGhostRefs() {
    std::set<void*> claimed;
    for (auto& [id, gh] : g_ghosts)
        if (gh.ref) claimed.insert(gh.ref);
    return claimed;
}

// Spawn a ghost NPC via placeatme and immediately scan for it.
// Returns the new TESObjectREFR* or null if template not known / spawn failed.
static void* SpawnGhostNow() {
    if (!g_ghostTemplateFormID) return nullptr;

    auto before = GetRefsOfBaseForm(g_ghostTemplateFormID);

    char buf[64];
    snprintf(buf, sizeof(buf), "player.placeatme %08X 1", g_ghostTemplateFormID);
    RunCmd(buf);

    void* player = *(void**)GH_PLAYER_PTR;
    if (!player) return nullptr;
    void* cell = *(void**)((char*)player + GH_OFF_REF_CELL);
    if (!cell) return nullptr;

    struct Node { void* item; Node* next; };
    Node* node = (Node*)((char*)cell + CELL_OBJLIST_OFF);
    while (node) {
        void* ref = node->item;
        if (ref && !before.count(ref)) {
            void* base = *(void**)((char*)ref + GH_OFF_REF_BASE);
            if (base && *(uint32_t*)((char*)base + GH_OFF_FORM_ID) == g_ghostTemplateFormID)
                return ref;
        }
        node = node->next;
    }
    return nullptr;
}

// Write position to a ghost ref's logical fields and NiNode transforms.
// Local translate persists through engine UpdateTransforms; world takes immediate visual effect.
static void WriteGhostPos(void* ref, float x, float y, float z, float rotZ) {
    *(float*)((char*)ref + GH_OFF_REF_POS_X) = x;
    *(float*)((char*)ref + GH_OFF_REF_POS_Y) = y;
    *(float*)((char*)ref + GH_OFF_REF_POS_Z) = z;
    *(float*)((char*)ref + GH_OFF_REF_ROT_Z) = rotZ;

    void* ni = *(void**)((char*)ref + GH_OFF_REF_NINODE);
    if (ni) {
        *(float*)((char*)ni + NINODE_LOCAL_TRANSLATE_X) = x;
        *(float*)((char*)ni + NINODE_LOCAL_TRANSLATE_Y) = y;
        *(float*)((char*)ni + NINODE_LOCAL_TRANSLATE_Z) = z;
        *(float*)((char*)ni + NINODE_WORLD_TRANSLATE_X) = x;
        *(float*)((char*)ni + NINODE_WORLD_TRANSLATE_Y) = y;
        *(float*)((char*)ni + NINODE_WORLD_TRANSLATE_Z) = z;
    }
}

// Process ghost events queued by PollLoop — called from game thread only.
static void TickGhosts() {
    DiscoverGhostTemplate();

    std::queue<GhostEvent> local;
    {
        std::lock_guard<std::mutex> lk(g_ghostQMtx);
        std::swap(local, g_ghostQ);
    }

    while (!local.empty()) {
        GhostEvent ev = std::move(local.front()); local.pop();

        switch (ev.type) {

        case GhostEvt::Appear: {
            auto& gh = g_ghosts[ev.charId];
            if (!gh.ref && !gh.spawning) {
                void* ref = SpawnGhostNow();
                if (ref) {
                    gh.ref      = ref;
                    gh.spawning = false;
                } else {
                    gh.spawning = true; // retry on next tick if scanning is enabled later
                }
            }
            gh.x = ev.x; gh.y = ev.y; gh.z = ev.z; gh.rotZ = ev.rotZ;
            if (gh.ref) WriteGhostPos(gh.ref, gh.x, gh.y, gh.z, gh.rotZ);
            break;
        }

        case GhostEvt::Leave:
            // Mark for delete via console; ref memory will be freed by engine.
            // We can't call MarkForDelete directly without the function address,
            // so just drop the ref from our map and let the engine GC it eventually.
            // TODO: EnqueueCmd("prid <refId>; disable") once we have the formID.
            g_ghosts.erase(ev.charId);
            break;

        case GhostEvt::PosUpdate: {
            auto it = g_ghosts.find(ev.charId);
            if (it == g_ghosts.end()) break;
            auto& gh = it->second;
            gh.x = ev.x; gh.y = ev.y; gh.z = ev.z; gh.rotZ = ev.rotZ;
            if (gh.ref) WriteGhostPos(gh.ref, gh.x, gh.y, gh.z, gh.rotZ);
            break;
        }

        }
    }

    // Retry ref resolution for ghosts whose placeatme already ran but whose ref
    // we couldn't identify yet. Scan for unclaimed template refs in the cell.
    if (g_ghostTemplateFormID) {
        auto claimed = ClaimedGhostRefs();
        auto inCell  = GetRefsOfBaseForm(g_ghostTemplateFormID);
        for (auto& [charId, gh] : g_ghosts) {
            if (!gh.spawning || gh.ref) continue;
            for (void* ref : inCell) {
                if (!claimed.count(ref)) {
                    gh.ref      = ref;
                    gh.spawning = false;
                    claimed.insert(ref);
                    WriteGhostPos(ref, gh.x, gh.y, gh.z, gh.rotZ);
                    break;
                }
            }
        }
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
    }
    // New character: game already has the right stats from character creation.
    // The first CHAR_SAVE tick will push them to the server.

    std::string name = json::getStr(raw, "name");
    EnqueueCmd("Message \"[TES4MP] Connected as " + SanitiseForCmd(name) + "\"");
}

// ── Background poll loop ───────────────────────────────────────────────────────

static void PollLoop() {
    while (g_running) {
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
                ChatPush("[TES4MP] Received: " + std::to_string(pkt.intField) + "x " + pkt.strField);
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
                ChatPush("[TES4MP] " + pkt.strField);
                break;
            case PacketType::PrivateMsg:
                ChatPush("[PM from " + pkt.strField2 + "] " + pkt.strField);
                break;
            case PacketType::DungeonCleared:
                ChatPush("[TES4MP] " + pkt.strField2 + " cleared " + pkt.strField);
                break;
            case PacketType::PlayerJoin:
                ChatPush("[TES4MP] " + pkt.strField + " joined (" + pkt.strField2 + ")");
                break;
            case PacketType::PlayerLeave:
                ChatPush("[TES4MP] " + pkt.strField + " left");
                break;
            case PacketType::PartyInvite:
                ChatPush("[TES4MP] Party invite from " + pkt.strField +
                         " — type: /party accept " + pkt.strField);
                break;
            case PacketType::Kick:
                ChatPush("[TES4MP] Kicked: " + pkt.strField);
                g_network.disconnect();
                g_chatVisible = false;
                g_phase = AuthPhase::Idle;
                g_ghosts.clear(); // drop ghost state on disconnect
                PosSync_Stop();
                break;

            case PacketType::PlayerPos: {
                GhostEvent ev;
                ev.type   = GhostEvt::PosUpdate;
                ev.charId = pkt.strField;
                ev.x      = JF(pkt.raw, "x");
                ev.y      = JF(pkt.raw, "y");
                ev.z      = JF(pkt.raw, "z");
                ev.rotZ   = JF(pkt.raw, "rot");
                std::lock_guard<std::mutex> lk(g_ghostQMtx);
                g_ghostQ.push(ev);
                break;
            }

            case PacketType::GhostAppear: {
                GhostEvent ev;
                ev.type   = GhostEvt::Appear;
                ev.charId = pkt.strField;
                ev.x      = JF(pkt.raw, "x");
                ev.y      = JF(pkt.raw, "y");
                ev.z      = JF(pkt.raw, "z");
                ev.rotZ   = JF(pkt.raw, "rot");
                {
                    std::lock_guard<std::mutex> lk(g_ghostQMtx);
                    g_ghostQ.push(ev);
                }
                // Show in chat so we know someone entered our cell
                std::string name = json::getStr(pkt.raw, "char_name");
                if (!name.empty())
                    ChatPush("[TES4MP] " + name + " is nearby.");
                break;
            }

            case PacketType::GhostLeave: {
                GhostEvent ev;
                ev.type   = GhostEvt::Leave;
                ev.charId = pkt.strField;
                std::lock_guard<std::mutex> lk(g_ghostQMtx);
                g_ghostQ.push(ev);
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

    // Process ghost NPC events (spawn, move, despawn)
    TickGhosts();
}

void GameHooks_Shutdown() {
    PosSync_Stop();
    g_running = false;
    if (g_pollThread.joinable()) g_pollThread.join();
}
