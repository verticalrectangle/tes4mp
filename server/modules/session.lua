-- session.lua — Online session management, keyed by char_id (integer).
-- auth.lua calls addSession() after completing the auth flow.

local store  = require("server.db.store")

local M = {}

-- sessions[char_id] = { client, char_id, char, name, role, token, cell, last_pos, cached_skills, cached_attrs }
local sessions = {}

-- ── Internal helpers ──────────────────────────────────────────────────────────

local function sendTo(client, pkt)
    local ok = client:send(pkt .. "\n")
    return ok ~= nil
end

local function playerListPacket()
    local json    = require("cjson")
    local players = {}
    for _, sess in pairs(sessions) do
        players[#players + 1] = {
            name  = sess.name,
            role  = sess.role,
            level = tonumber(sess.char.level) or 1,
        }
    end
    return json.encode({ type = "PLAYER_LIST", players = players })
end

local function buildQuestSync(char_id)
    local json    = require("cjson")
    local quests  = {}
    local globals = store.getAllGlobalQuests()
    for qid, stage in pairs(globals) do
        quests[#quests + 1] = { questId = qid, stage = stage, scope = "global" }
    end
    local personal = store.getCharacterQuests(char_id)
    for qid, stage in pairs(personal) do
        if not globals[qid] then
            quests[#quests + 1] = { questId = qid, stage = stage, scope = "personal" }
        end
    end
    return json.encode({ type = "QUEST_SYNC", quests = quests })
end

-- Returns skills, attrs, vitals tables and the encoded CHAR_LOAD packet.
-- Callers that need the raw tables (addSession) use the first three return values
-- to seed the session cache without a second DB round-trip.
local function buildCharLoad(char, is_new, config)
    local cid    = tonumber(char.id)
    local skills = store.getCharacterSkills(cid)
    local attrs  = store.getCharacterAttributes(cid)
    local vitals = store.getCharacterVitals(cid) or {}
    local json   = require("cjson")
    local bounties = store.getBounties(cid)
    local quests = require("server.modules.quests")
    local pkt = json.encode({
        type       = "CHAR_LOAD",
        is_new     = is_new or false,
        pvp        = (config and config.pvp == true) or false,
        monitored  = quests.getMonitored(),
        name       = char.name,
        race       = char.race,
        class      = char.class,
        birthsign  = char.birthsign,
        gender     = tonumber(char.gender) or 0,
        level      = tonumber(char.level)  or 1,
        skills     = skills,
        attributes = attrs,
        health     = tonumber(vitals.health)  or 100,
        magicka    = tonumber(vitals.magicka) or 100,
        stamina    = tonumber(vitals.stamina) or 100,
        fame       = tonumber(vitals.fame)    or 0,
        infamy     = tonumber(vitals.infamy)  or 0,
        gold       = tonumber(vitals.gold)    or 100,
        bounties   = bounties,
        -- Tutorial skip — sent on every login until server marks it complete.
        -- Also mirrored into `cell`: the returning-player client path only
        -- teleports via `cell`, and a reconnect before the first CHAR_SAVE
        -- must still deliver the start teleport.
        start_cell        = (tonumber(char.tutorial_complete) or 0) == 0 and "ImperialSewers03"    or nil,
        start_quest       = (tonumber(char.tutorial_complete) or 0) == 0 and "MQ01"               or nil,
        start_quest_stage = (tonumber(char.tutorial_complete) or 0) == 0 and 90                   or nil,
        cell              = (tonumber(char.tutorial_complete) or 0) == 0 and "ImperialSewers03"
                            or (char.last_cell ~= "" and char.last_cell or nil),
    })
    return skills, attrs, pkt
end

-- ── Public API ────────────────────────────────────────────────────────────────

-- Called by auth.lua after the token is resolved to a character.
function M.addSession(sock, info, char, config, is_new)
    local json    = require("cjson")
    local char_id = tonumber(char.id)

    local skills, attrs, charLoadPkt = buildCharLoad(char, is_new, config)
    local appearance = store.getAppearance(char_id)
    local equipment  = store.getEquipment(char_id)

    sessions[char_id] = {
        client        = sock,
        char_id       = char_id,
        char          = char,
        name          = char.name,
        role          = char.role or "player",
        token         = char.token,
        cached_skills = skills,
        cached_attrs  = attrs,
        cell          = "",
        appearance    = appearance,
        equipment     = equipment,
        last_pos      = { x=0, y=0, z=0, t=0 },
    }

    -- Push character state immediately (no AUTH_OK needed — CHAR_LOAD is the signal)
    sendTo(sock, charLoadPkt)
    sendTo(sock, buildQuestSync(char_id))
    sendTo(sock, playerListPacket())
    -- Reveal all map markers and sync current weather for coop consistency
    sendTo(sock, json.encode({ type = "REVEAL_MARKERS" }))
    local world = require("server.modules.world")
    local wid = world.getCurrentWeather()
    if wid and wid ~= 0 then
        sendTo(sock, json.encode({ type = "WEATHER_SYNC", weather_id = wid }))
    end

    local joinPkt = json.encode({ type = "PLAYER_JOIN", name = char.name, role = char.role or "player" })
    M.broadcast(joinPkt, char_id)
    M.broadcast(playerListPacket())

    print(("[session] %s connected from %s"):format(char.name, info.addr or "?"))
end

function M.count()
    local n = 0
    for _ in pairs(sessions) do n = n + 1 end
    return n
end

function M.getByCharId(char_id)
    return sessions[tonumber(char_id)]
end

function M.getByName(name)
    local lower = name:lower()
    for _, sess in pairs(sessions) do
        if sess.name:lower() == lower then return sess end
    end
    return nil
end

function M.getAll()
    return sessions
end

function M.sendTo(char_id, pkt)
    local sess = sessions[tonumber(char_id)]
    if sess then sendTo(sess.client, pkt) end
end

function M.broadcast(pkt, exclude_char_id)
    for cid, sess in pairs(sessions) do
        if cid ~= exclude_char_id then
            sendTo(sess.client, pkt)
        end
    end
end

-- Broadcast only to members of the same party (or everyone if no party).
function M.broadcastParty(char_id, pkt)
    local party_id = store.getPartyByChar(char_id)
    if not party_id then
        M.sendTo(char_id, pkt)
        return
    end
    local members = store.getPartyMembers(party_id)
    for _, cid in ipairs(members) do
        M.sendTo(cid, pkt)
    end
end

-- Broadcast to players in the same cell (optionally excluding one char_id).
function M.broadcastToCell(cell, pkt, exclude_char_id)
    if not cell or cell == "" then return end
    for cid, sess in pairs(sessions) do
        if cid ~= exclude_char_id and sess.cell == cell then
            sendTo(sess.client, pkt)
        end
    end
end

-- Return all sessions in a given cell, excluding one char_id.
function M.getInCell(cell, exclude_char_id)
    local list = {}
    if not cell or cell == "" then return list end
    for cid, sess in pairs(sessions) do
        if cid ~= exclude_char_id and sess.cell == cell then
            list[#list + 1] = sess
        end
    end
    return list
end

function M.updateCell(char_id, cell)
    local sess = sessions[tonumber(char_id)]
    if sess then sess.cell = cell end
end

function M.updatePosCache(char_id, x, y, z, t)
    local sess = sessions[tonumber(char_id)]
    if sess then sess.last_pos = { x=x, y=y, z=z, t=t } end
end

function M.getPosCache(char_id)
    local sess = sessions[tonumber(char_id)]
    return sess and sess.last_pos or nil
end

function M.cacheEquipment(char_id, json_blob)
    local sess = sessions[tonumber(char_id)]
    if sess then sess.equipment = json_blob end
end

function M.getEquipmentCached(char_id)
    local sess = sessions[tonumber(char_id)]
    return sess and sess.equipment or nil
end

function M.cacheAppearance(char_id, json_blob)
    local sess = sessions[tonumber(char_id)]
    if sess then sess.appearance = json_blob end
end

function M.getAppearanceCached(char_id)
    local sess = sessions[tonumber(char_id)]
    return sess and sess.appearance or nil
end

function M.disconnect(char_id)
    char_id = tonumber(char_id)
    local sess = sessions[char_id]
    if not sess then return end
    sessions[char_id] = nil

    local json = require("cjson")

    -- Notify cell members this ghost is leaving; hand over cell authority if held
    if sess.cell and sess.cell ~= "" then
        M.broadcastToCell(sess.cell,
            json.encode({ type = "GHOST_LEAVE", char_id = tostring(char_id) }), char_id)
        local world = require("server.modules.world")
        world.releaseAuthority(sess.cell, char_id)
    end

    M.broadcast(json.encode({ type = "PLAYER_LEAVE", name = sess.name }))
    M.broadcast(playerListPacket())

    store.updateCharacterSeen(char_id)
    store.removePartyMember(char_id)
    print(("[session] %s disconnected"):format(sess.name))
end

function M.kick(char_id, reason)
    local sess = sessions[tonumber(char_id)]
    if not sess then return end
    local json = require("cjson")
    sendTo(sess.client, json.encode({ type = "KICK", reason = reason or "kicked" }))
    sess.client:close()
    M.disconnect(char_id)
end

function M.kickByName(name, reason)
    local sess = M.getByName(name)
    if sess then M.kick(sess.char_id, reason); return true end
    return false
end

function M.updateStatCache(char_id, skills, attrs, level)
    local sess = sessions[tonumber(char_id)]
    if not sess then return end
    if skills then sess.cached_skills  = skills end
    if attrs  then sess.cached_attrs   = attrs  end
    if level  then sess.char.level     = level  end
end

return M
