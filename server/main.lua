-- TES4MP Dedicated Server
-- Run with: lua server/main.lua

package.path = "./?.lua;" .. package.path

local socket   = require("socket")
local json     = require("cjson")
local store    = require("server.db.store")
local session  = require("server.modules.session")
local auth     = require("server.modules.auth")
local quests   = require("server.modules.quests")
local commands = require("server.modules.commands")
local world    = require("server.modules.world")

-- ── Config ────────────────────────────────────────────────────────────────────

local function loadJSON(path)
    local f, err = io.open(path, "r")
    if not f then error("Cannot open " .. path .. ": " .. tostring(err)) end
    local data = f:read("*a"); f:close()
    return json.decode(data)
end

local configPath  = (arg and arg[1] and arg[1]:match("%.json$")) and arg[1] or "server/config.json"
local config      = loadJSON(configPath)
local questConfig = loadJSON("server/data/quests.json")

config.port        = math.floor(config.port)
config.max_players = math.floor(config.max_players)
config.tick_ms     = math.floor(config.tick_ms)

-- ── Init ──────────────────────────────────────────────────────────────────────

store.open(config.db_path)
quests.init(questConfig)
world.init(config)
commands.init(config)

print(("[server] %s starting on %s:%d"):format(config.server_name, config.host, config.port))

local srv = assert(socket.bind(config.host, config.port, 32))
srv:settimeout(0)

-- ── Client state ──────────────────────────────────────────────────────────────
-- clients[sock] = {
--   buffer   string
--   state    "pending_hello" | "in_game"
--   char_id  int | nil
--   addr     string
-- }

local clients = {}
local sockets = { srv }

local function removeClient(sock)
    local info = clients[sock]
    if info and info.char_id then
        session.disconnect(info.char_id)
    end
    for i, s in ipairs(sockets) do
        if s == sock then table.remove(sockets, i); break end
    end
    clients[sock] = nil
    sock:close()
end

-- ── Packet dispatch ───────────────────────────────────────────────────────────

local function handlePacket(sock, info, raw)
    local ok, pkt = pcall(json.decode, raw)
    if not ok or type(pkt) ~= "table" then return end

    local t = tostring(pkt.type or "")

    -- ── Auth-phase packets (any state) ────────────────────────────────────────

    if t == "HELLO" then
        if info.state ~= "pending_hello" then return end
        if session.count() >= config.max_players then
            sock:send(json.encode({ type = "KICK", reason = "server full" }) .. "\n")
            removeClient(sock); return
        end
        auth.handleHello(sock, info, pkt, config)
        return

    elseif t == "PING" then
        sock:send(json.encode({ type = "PONG" }) .. "\n")
        return
    end

    -- ── In-game-only packets ──────────────────────────────────────────────────

    if info.state ~= "in_game" then return end
    local cid = info.char_id

    if t == "QUEST_STAGE" then
        quests.handleQuestStage(cid, tostring(pkt.questId or ""), tonumber(pkt.stage) or 0)

    elseif t == "COMMAND" then
        commands.handle(cid, tostring(pkt.text or ""))

    elseif t == "POSITION_UPDATE" then
        world.handlePositionUpdate(cid, pkt)

    elseif t == "DUNGEON_CLEAR" then
        world.handleDungeonClear(cid, pkt)

    elseif t == "DUNGEON_QUERY" then
        world.handleDungeonQuery(cid, pkt)

    elseif t == "FACTION_RANK" then
        world.handleFactionRank(cid, pkt)

    elseif t == "CRIME_REPORT" then
        world.handleCrimeReport(cid, pkt)

    elseif t == "BOUNTY_PAID" then
        world.handleBountyPaid(cid, pkt)

    elseif t == "CHAR_SAVE" then
        world.handleCharSave(cid, pkt)

    elseif t == "APPEARANCE" then
        world.handleAppearance(cid, pkt)

    elseif t == "NPC_KILLED" then
        world.handleNpcKilled(cid, pkt)

    elseif t == "ITEM_TAKEN" then
        world.handleItemTaken(cid, pkt)

    elseif t == "PLAYER_DIED" then
        world.handlePlayerDied(cid, pkt)

    elseif t == "WEATHER_REPORT" then
        world.handleWeatherReport(cid, pkt)

    elseif t == "EQUIP_UPDATE" then
        world.handleEquipUpdate(cid, pkt)

    elseif t == "NPC_HP" then
        world.handleNpcHp(cid, pkt)

    elseif t == "NPC_DAMAGE" then
        world.handleNpcDamage(cid, pkt)

    elseif t == "PLAYER_HIT" then
        world.handlePlayerHit(cid, pkt)

    elseif t == "TP_REQUEST" then
        world.handleTpRequest(cid)

    elseif t == "NPC_SPAWNS" then
        world.handleNpcSpawns(cid, pkt)

    elseif t == "NPC_DAMAGE_SID" then
        world.handleNpcDamageSid(cid, pkt)
    end
end

-- ── Main loop ─────────────────────────────────────────────────────────────────

print("[server] listening — waiting for connections")

local tick = 0

while true do
    local readable, _, _ = socket.select(sockets, nil, config.tick_ms / 1000)

    for _, sock in ipairs(readable or {}) do
        if sock == srv then
            local client, cerr = srv:accept()
            if client then
                client:settimeout(0)
                table.insert(sockets, client)
                local addr = tostring(client:getpeername())
                clients[client] = {
                    buffer     = "",
                    state      = "pending_hello",
                    char_id    = nil,
                    addr       = addr,
                    last_heard = socket.gettime(),
                }
                -- Send server hello so client knows to show login prompt
                client:send(json.encode({
                    type    = "SERVER_HELLO",
                    name    = config.server_name,
                    motd    = config.motd,
                    version = 2,
                }) .. "\n")
                print(("[server] connection from %s"):format(addr))
            end
        else
            local info = clients[sock]
            if info then
                local data, serr, partial = sock:receive("*l")
                local line = data or partial
                if line and #line > 0 then
                    info.last_heard = socket.gettime()
                    info.buffer = info.buffer .. line
                    if data then
                        local ok, err = pcall(handlePacket, sock, info, info.buffer)
                        if not ok then
                            print(("[server] packet error from %s: %s"):format(info.addr, tostring(err)))
                        end
                        info.buffer = ""
                    end
                end
                if serr == "closed" then
                    removeClient(sock)
                end
            end
        end
    end

    -- Kill clients that haven't sent anything in 15s (crash / hard disconnect)
    local now = socket.gettime()
    local timed_out = {}
    for sock, info in pairs(clients) do
        if sock ~= srv and (now - (info.last_heard or now)) > 15 then
            timed_out[#timed_out + 1] = sock
        end
    end
    for _, sock in ipairs(timed_out) do
        print(("[server] timeout: %s"):format(clients[sock].addr))
        removeClient(sock)
    end

    tick = tick + 1
end
