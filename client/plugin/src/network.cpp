#include "network.h"
#include "json_util.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <ctime>

// ws2_32 linked via CMakeLists target_link_libraries

Network g_network;

Network::Network() : m_sock(INVALID_SOCKET), m_connected(false) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

Network::~Network() {
    disconnect();
    WSACleanup();
}

static void CleanupUpdateLeftovers();

bool Network::connect(const std::string& host, int port) {
    if (m_connected) disconnect();
    CleanupUpdateLeftovers();
    m_binNeed = 0;
    m_binBuf.clear();

    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) return false;

    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return false;

    int r = ::connect(m_sock, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r == SOCKET_ERROR) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    m_connected = true;
    m_thread = std::thread(&Network::recvLoop, this);
    return true;
}

void Network::disconnect() {
    m_connected = false;
    if (m_sock != INVALID_SOCKET) {
        shutdown(m_sock, SD_BOTH);
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    if (m_thread.joinable()) m_thread.join();
}

bool Network::isConnected() const {
    return m_connected;
}

void Network::send(const std::string& line) {
    if (!m_connected) return;
    std::string data = line + "\n";
    // Send directly from game thread — small packets, non-blocking enough for our use
    ::send(m_sock, data.c_str(), (int)data.size(), 0);
}

bool Network::pollPacket(Packet& out) {
    std::lock_guard<std::mutex> lk(m_inMutex);
    if (m_incoming.empty()) return false;
    out = std::move(m_incoming.front());
    m_incoming.pop();
    return true;
}

void Network::recvLoop() {
    char buf[4096];
    while (m_connected) {
        int n = recv(m_sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            m_connected = false;
            break;
        }
        // Length-aware append — the DLL update stream is binary (NUL bytes).
        m_recvBuf.append(buf, (size_t)n);

        for (;;) {
            // Binary mode: a DLL_UPDATE header promised m_binNeed raw bytes.
            if (m_binNeed > 0) {
                size_t take = m_binNeed < m_recvBuf.size() ? m_binNeed
                                                           : m_recvBuf.size();
                m_binBuf.append(m_recvBuf, 0, take);
                m_recvBuf.erase(0, take);
                m_binNeed -= take;
                if (m_binNeed > 0) break;      // wait for more data
                finishDllUpdate();             // resume line mode below
            }
            // Line mode — each line is one JSON packet
            size_t pos = m_recvBuf.find('\n');
            if (pos == std::string::npos) break;
            std::string line = m_recvBuf.substr(0, pos);
            m_recvBuf.erase(0, pos + 1);
            if (!line.empty()) dispatchLine(line);
        }
    }
    // Push a synthetic disconnect packet so the script knows
    Packet p;
    p.type     = PacketType::Kick;
    p.strField = "disconnected from server";
    std::lock_guard<std::mutex> lk(m_inMutex);
    m_incoming.push(p);
}

// Own DLL path on disk (empty if somehow not loaded under our name).
static std::string SelfDllPath() {
    HMODULE mod = GetModuleHandleA("TES4MP.dll");
    if (!mod) return {};
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::string(path, n) : std::string{};
}

void Network::pushMessage(const std::string& text) {
    Packet p;
    p.type     = PacketType::Message;
    p.strField = text;
    std::lock_guard<std::mutex> lk(m_inMutex);
    m_incoming.push(p);
}

// The full new DLL has arrived. A loaded DLL's file can't be overwritten but
// CAN be renamed — move the running one aside and write the update in its
// place; the next launch loads it. (One manual install of an updater-aware
// build is required first; after that, partners never copy files again.)
void Network::finishDllUpdate() {
    std::string data;
    data.swap(m_binBuf);

    if (data.size() < 2 || data[0] != 'M' || data[1] != 'Z') {
        pushMessage("[TES4MP] Update download corrupt — not applied.");
        return;
    }
    std::string path = SelfDllPath();
    if (path.empty()) {
        pushMessage("[TES4MP] Update failed: cannot locate own DLL.");
        return;
    }
    std::string old = path + ".old";
    DeleteFileA(old.c_str());  // leftover from an even earlier update
    if (!MoveFileExA(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        pushMessage("[TES4MP] Update failed: could not move current DLL aside "
                    "- copy dist/TES4MP.dll manually.");
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.write(data.data(), (std::streamsize)data.size())) {
        // Try to roll back so the install isn't left with no DLL at all.
        MoveFileExA(old.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        pushMessage("[TES4MP] Update failed writing new DLL - copy "
                    "dist/TES4MP.dll manually.");
        return;
    }
    out.close();
    pushMessage("[TES4MP] Updated! Restart Oblivion to finish.");
}

// Best-effort cleanup of the previous update's renamed-aside DLL. Called on
// the first connect attempt (safely outside the loader lock).
static void CleanupUpdateLeftovers() {
    static bool done = false;
    if (done) return;
    done = true;
    std::string path = SelfDllPath();
    if (!path.empty()) DeleteFileA((path + ".old").c_str());
}

void Network::dispatchLine(const std::string& line) {
    using namespace json;

    // Auto-update header: the next `size` raw bytes are the new DLL.
    if (line.find("\"DLL_UPDATE\"") != std::string::npos) {
        long size = json::getInt(line, "size");
        if (size >= 64 * 1024 && size <= 64 * 1024 * 1024) {
            m_binNeed = (size_t)size;
            m_binBuf.clear();
            m_binBuf.reserve(m_binNeed);
            pushMessage("[TES4MP] Client out of date - downloading update ("
                        + std::to_string(size / 1024) + " KB)...");
        }
        return;
    }

    Packet p;
    p.raw = line;

    std::string type = getStr(line, "type");

    if (type == "NEED_NAME") {
        p.type     = PacketType::NeedName;
        p.strField = getStr(line, "reason");  // optional retry message
    } else if (type == "SERVER_HELLO") {
        p.type     = PacketType::ServerHello;
        p.strField = getStr(line, "motd");
    } else if (type == "CHAR_LOAD") {
        p.type = PacketType::CharLoad;
        // raw holds full JSON for game_hooks to apply
    } else if (type == "QUEST_STAGE") {
        p.type      = PacketType::QuestStage;
        p.strField  = getStr(line, "questId");
        p.intField  = getInt(line, "stage");
        p.strField2 = getStr(line, "src");
    } else if (type == "QUEST_SYNC") {
        p.type = PacketType::QuestSync;
    } else if (type == "PLAYER_LIST") {
        p.type = PacketType::PlayerList;
    } else if (type == "PLAYER_JOIN") {
        p.type      = PacketType::PlayerJoin;
        p.strField  = getStr(line, "name");
        p.strField2 = getStr(line, "role");
    } else if (type == "PLAYER_LEAVE") {
        p.type     = PacketType::PlayerLeave;
        p.strField = getStr(line, "name");
    } else if (type == "COMMAND_RESULT") {
        p.type     = PacketType::CommandResult;
        p.strField = getStr(line, "text");
    } else if (type == "SERVER_ANNOUNCEMENT") {
        p.type     = PacketType::ServerAnnouncement;
        p.strField = getStr(line, "text");
    } else if (type == "PRIVATE_MSG") {
        p.type      = PacketType::PrivateMsg;
        p.strField  = getStr(line, "text");
        p.strField2 = getStr(line, "from");
    } else if (type == "MESSAGE") {
        p.type     = PacketType::Message;
        p.strField = getStr(line, "text");
    } else if (type == "KICK") {
        p.type     = PacketType::Kick;
        p.strField = getStr(line, "reason");
        m_connected = false;
    } else if (type == "PONG") {
        p.type = PacketType::Pong;
    } else if (type == "GIVE_ITEM") {
        p.type     = PacketType::GiveItem;
        p.strField = getStr(line, "item_id");
        p.intField = getInt(line, "count");
    } else if (type == "TAKE_ITEM") {
        p.type     = PacketType::TakeItem;
        p.strField = getStr(line, "item_id");
        p.intField = getInt(line, "count");
    } else if (type == "TELEPORT") {
        p.type     = PacketType::Teleport;
        p.strField = getStr(line, "cell");
        // x/y/z are in raw if needed
    } else if (type == "SET_LEVEL") {
        p.type     = PacketType::SetLevel;
        p.intField = getInt(line, "level");
    } else if (type == "SET_SKILL") {
        p.type     = PacketType::SetSkill;
        p.strField = getStr(line, "skill");
        p.intField = getInt(line, "value");
    } else if (type == "SET_ATTR") {
        p.type     = PacketType::SetAttr;
        p.strField = getStr(line, "attr");
        p.intField = getInt(line, "value");
    } else if (type == "ADD_GOLD") {
        p.type     = PacketType::AddGold;
        p.intField = getInt(line, "amount");
    } else if (type == "SET_BOUNTY") {
        p.type      = PacketType::SetBounty;
        p.strField  = getStr(line, "hold");
        p.intField  = getInt(line, "amount");
    } else if (type == "DUNGEON_CLEARED") {
        p.type      = PacketType::DungeonCleared;
        p.strField  = getStr(line, "dungeon_id");
        p.strField2 = getStr(line, "clearer");
    } else if (type == "PARTY_INVITE") {
        p.type     = PacketType::PartyInvite;
        p.strField = getStr(line, "from");
    } else if (type == "PLAYER_POS") {
        p.type     = PacketType::PlayerPos;
        p.strField = getStr(line, "char_id");
    } else if (type == "GHOST_APPEAR") {
        p.type     = PacketType::GhostAppear;
        p.strField = getStr(line, "char_id");
    } else if (type == "GHOST_LEAVE") {
        p.type     = PacketType::GhostLeave;
        p.strField = getStr(line, "char_id");
    } else if (type == "NPC_KILLED") {
        p.type     = PacketType::NpcKilled;
        p.intField = (int)getInt(line, "ref_id");
    } else if (type == "NPC_KILL_SYNC") {
        p.type = PacketType::NpcKillSync;
        p.raw  = line;
    } else if (type == "ITEM_SYNC") {
        p.type = PacketType::ItemSync;
        p.raw  = line;
    } else if (type == "CONTAINER_STATE") {
        p.type = PacketType::ContainerState;
        p.raw  = line;
    } else if (type == "PLAYER_DIED") {
        p.type     = PacketType::PlayerDied;
        p.strField = getStr(line, "char_id");
    } else if (type == "WEATHER_SYNC") {
        p.type     = PacketType::WeatherSync;
        p.intField = (int)getInt(line, "weather_id");
    } else if (type == "REVEAL_MARKERS") {
        p.type = PacketType::RevealMarkers;
    } else if (type == "EQUIP_SYNC") {
        p.type     = PacketType::EquipSync;
        p.strField = getStr(line, "char_id");
    } else if (type == "CELL_AUTHORITY") {
        p.type     = PacketType::CellAuthority;
        p.strField = getStr(line, "cell");
        p.intField = getBool(line, "authority") ? 1 : 0;
    } else if (type == "NPC_HP") {
        p.type = PacketType::NpcHp;
    } else if (type == "NPC_DAMAGE") {
        p.type = PacketType::NpcDamage;
    } else if (type == "NPC_SPAWNS") {
        p.type = PacketType::NpcSpawns;
    } else if (type == "NPC_DAMAGE_SID") {
        p.type = PacketType::NpcDamageSid;
    } else if (type == "WORLD_ITEM_SYNC") {
        p.type = PacketType::WorldItemSync;
    } else if (type == "NPC_POS") {
        p.type = PacketType::NpcPos;
    } else if (type == "DAMAGE_TAKEN") {
        p.type     = PacketType::DamageTaken;
        p.intField = getInt(line, "amount");
        p.strField = getStr(line, "from");
    } else {
        return;
    }

    std::lock_guard<std::mutex> lk(m_inMutex);
    m_incoming.push(p);
}

// Persist token to %APPDATA%\TES4MP\token.txt
std::string Network::loadOrCreateToken() {
    char appData[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
    std::string dir  = std::string(appData) + "\\TES4MP";
    std::string path = dir + "\\token.txt";

    CreateDirectoryA(dir.c_str(), nullptr);

    std::ifstream in(path);
    if (in.good()) {
        std::string tok;
        std::getline(in, tok);
        if (tok.size() >= 16) return tok;
    }

    // Generate a simple random token (not cryptographic, good enough for local server auth)
    srand((unsigned)time(nullptr) ^ (unsigned)(uintptr_t)&appData);
    std::ostringstream ss;
    for (int i = 0; i < 32; ++i)
        ss << std::hex << (rand() % 16);
    std::string tok = ss.str();

    std::ofstream out(path);
    out << tok;
    return tok;
}
