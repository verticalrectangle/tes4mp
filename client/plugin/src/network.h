#pragma once
// This file is compiled as a Windows DLL via mingw-w64 cross-compiler.
// SOCKET and Winsock types are intentionally not included here —
// they live in network.cpp which includes winsock2.h directly.
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>

// Forward-declare SOCKET as uintptr_t to keep the header platform-agnostic.
// The .cpp will static_assert that sizes match.
#ifdef _WIN32
#  include <winsock2.h>
#else
   typedef uintptr_t SOCKET;
   static constexpr SOCKET INVALID_SOCKET = (SOCKET)(~0);
#endif

// Client/server protocol version — must match server auth.lua PROTOCOL_VERSION.
// Sent in HELLO; mismatched clients are kicked with an update message.
static constexpr int TES4MP_PROTOCOL_VERSION = 4;

// Packet types the client will receive from the server.
enum class PacketType : int {
    None            = 0,
    NeedName        = 1,   // first run: server has no character for this token
    QuestStage      = 3,   // questId, stage, scope
    QuestSync       = 4,   // quests array
    PlayerList      = 5,
    PlayerJoin      = 6,
    PlayerLeave     = 7,
    CommandResult   = 8,
    Message         = 9,
    Kick            = 10,
    Pong            = 11,
    ServerHello     = 12,  // name, motd, version
    CharLoad        = 14,  // full character state to apply
    GiveItem        = 15,  // item_id, count → player.additem
    TakeItem        = 16,  // item_id, count → player.removeitem
    Teleport        = 17,  // cell [x y z]  → coc / moveto
    SetLevel        = 18,  // level          → player.setlevel
    SetSkill        = 19,  // skill, value   → player.setav
    SetAttr         = 20,  // attr, value    → player.setav
    AddGold         = 21,  // amount         → player.additem gold001
    SetBounty       = 22,  // hold, amount   → player.setcrimegold
    DungeonCleared  = 23,  // dungeon_id, clearer
    DungeonStatus   = 24,  // dungeon_id, cleared, clearer
    FactionRank     = 25,  // char_name, faction_id, rank
    PrivateMsg      = 26,  // from, text
    ServerAnnouncement = 27, // text
    PartyInvite     = 28,  // from
    PartyUpdate     = 29,  // members array
    PlayerPos       = 30,  // another player's position (char_id + x,y,z,rot in raw)
    GhostAppear     = 31,  // another player entered our cell (char_id + appearance in raw)
    GhostLeave      = 32,  // another player left our cell (char_id in strField)
    NpcKilled       = 33,  // server confirms NPC death — ref_id in intField
    NpcKillSync     = 34,  // cell-entry kill list — ref_id array in raw
    ItemSync        = 35,  // server broadcasts ITEM_TAKEN — raw JSON
    ContainerState  = 36,  // cell-entry container state — raw JSON
    PlayerDied      = 37,  // peer died — ghost should despawn (char_id in strField)
    WeatherSync     = 38,  // server-authoritative weather — intField = formId
    RevealMarkers   = 39,  // reveal all map markers on join
    EquipSync       = 40,  // peer equipment changed — char_id in strField, items in raw
    CellAuthority   = 41,  // NPC sim authority for a cell — cell in strField, intField = 0/1
    NpcHp           = 42,  // authority NPC health batch — cell + npcs array in raw
    NpcDamage       = 43,  // damage request routed to us (authority) — ref_id/amount in raw
    DamageTaken     = 44,  // PvP damage to apply — intField = amount, strField = attacker name
    NpcSpawns       = 45,  // authority dynamic spawn snapshot — raw JSON
    NpcDamageSid    = 46,  // follower damage on a replica, routed to us (authority) — sid/amount in raw
    WorldItemSync   = 47,  // loose world items taken — refs array in raw
    NpcPos          = 48,  // authority NPC position stream — cell + npcs array in raw
};

struct Packet {
    PacketType  type;
    std::string raw;        // full JSON string (for complex payloads)
    std::string strField;   // questId / name / text / reason / role / item_id / cell / skill / attr
    int         intField;   // stage / count / level / value / amount
    std::string strField2;  // scope / src / hold / from
};

class Network {
public:
    Network();
    ~Network();

    bool connect(const std::string& host, int port);
    void disconnect();
    bool isConnected() const;

    void send(const std::string& line);

    // Returns false if queue is empty
    bool pollPacket(Packet& out);

    // Token persisted to %APPDATA%\TES4MP\token.txt on first connect
    static std::string loadOrCreateToken();

private:
    void recvLoop();
    void dispatchLine(const std::string& line);
    void finishDllUpdate();
    void pushMessage(const std::string& text);

    SOCKET                  m_sock;
    std::atomic<bool>       m_connected;
    std::thread             m_thread;

    std::queue<Packet>      m_incoming;
    std::mutex              m_inMutex;

    std::queue<std::string> m_outgoing;
    std::mutex              m_outMutex;

    std::string             m_recvBuf;

    // Auto-update: after a DLL_UPDATE header the next m_binNeed bytes on the
    // wire are the raw new TES4MP.dll (not newline-JSON).
    size_t                  m_binNeed = 0;
    std::string             m_binBuf;
};

extern Network g_network;
