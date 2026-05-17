local luasql = require("luasql.sqlite3")

local M   = {}
local env = nil
local conn = nil

-- ── Helpers ───────────────────────────────────────────────────────────────────

local function esc(s)  return conn:escape(tostring(s)) end

local function exec(sql)
    local ok, err = conn:execute(sql)
    if not ok then error("DB exec error: " .. tostring(err) .. "\nSQL: " .. sql) end
    return ok
end

local function query(sql)
    local cur, err = conn:execute(sql)
    if not cur then error("DB query error: " .. tostring(err) .. "\nSQL: " .. sql) end
    return cur
end

-- LuaSQL 2.8/Lua 5.5: fetch() returns {} instead of nil on empty result.
local function rowOrNil(row)
    if type(row) ~= "table" or not next(row) then return nil end
    return row
end

-- Fetch all rows from a cursor into an array of independent tables.
local function fetchAll(cur)
    local rows = {}
    while true do
        local row = cur:fetch({}, "a")
        if not row or not next(row) then break end
        local r = {}
        for k, v in pairs(row) do r[k] = v end
        rows[#rows + 1] = r
    end
    cur:close()
    return rows
end

function M.transaction(fn)
    exec("BEGIN")
    local ok, err = pcall(fn)
    collectgarbage()  -- finalize any unreferenced cursors before COMMIT
    if ok then exec("COMMIT") else exec("ROLLBACK"); error(err) end
end

-- ── Schema ────────────────────────────────────────────────────────────────────

local TABLES = {
[[CREATE TABLE IF NOT EXISTS characters (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    token       TEXT    NOT NULL UNIQUE,
    name        TEXT    NOT NULL UNIQUE,
    role        TEXT    NOT NULL DEFAULT 'player',
    race        TEXT    NOT NULL DEFAULT 'Imperial',
    class       TEXT    NOT NULL DEFAULT 'Warrior',
    birthsign   TEXT    NOT NULL DEFAULT 'The Warrior',
    gender      INTEGER NOT NULL DEFAULT 0,
    level       INTEGER NOT NULL DEFAULT 1,
    playtime    INTEGER NOT NULL DEFAULT 0,
    last_cell   TEXT    NOT NULL DEFAULT '',
    pos_x       REAL    NOT NULL DEFAULT 0,
    pos_y       REAL    NOT NULL DEFAULT 0,
    pos_z       REAL    NOT NULL DEFAULT 0,
    created_at        INTEGER NOT NULL,
    last_seen         INTEGER NOT NULL,
    tutorial_complete INTEGER NOT NULL DEFAULT 0
)]],
[[CREATE TABLE IF NOT EXISTS character_skills (
    char_id INTEGER NOT NULL REFERENCES characters(id),
    skill   TEXT    NOT NULL,
    value   INTEGER NOT NULL DEFAULT 5,
    PRIMARY KEY (char_id, skill)
)]],
[[CREATE TABLE IF NOT EXISTS character_attributes (
    char_id INTEGER NOT NULL REFERENCES characters(id),
    attr    TEXT    NOT NULL,
    value   INTEGER NOT NULL DEFAULT 40,
    PRIMARY KEY (char_id, attr)
)]],
[[CREATE TABLE IF NOT EXISTS character_vitals (
    char_id INTEGER PRIMARY KEY REFERENCES characters(id),
    health  REAL    NOT NULL DEFAULT 100,
    magicka REAL    NOT NULL DEFAULT 100,
    stamina REAL    NOT NULL DEFAULT 100,
    fame    INTEGER NOT NULL DEFAULT 0,
    infamy  INTEGER NOT NULL DEFAULT 0,
    gold    INTEGER NOT NULL DEFAULT 100
)]],
[[CREATE TABLE IF NOT EXISTS character_quests (
    char_id  INTEGER NOT NULL REFERENCES characters(id),
    quest_id TEXT    NOT NULL,
    stage    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (char_id, quest_id)
)]],
[[CREATE TABLE IF NOT EXISTS global_quests (
    quest_id   TEXT    PRIMARY KEY,
    stage      INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL
)]],
[[CREATE TABLE IF NOT EXISTS dungeon_clears (
    dungeon_id     TEXT    PRIMARY KEY,
    char_id        INTEGER NOT NULL REFERENCES characters(id),
    char_name      TEXT    NOT NULL DEFAULT '',
    cleared_at     INTEGER NOT NULL,
    cooldown_until INTEGER NOT NULL DEFAULT 0
)]],
[[CREATE TABLE IF NOT EXISTS faction_ranks (
    char_id    INTEGER NOT NULL REFERENCES characters(id),
    faction_id TEXT    NOT NULL,
    rank       INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (char_id, faction_id)
)]],
[[CREATE TABLE IF NOT EXISTS bounties (
    char_id INTEGER NOT NULL REFERENCES characters(id),
    hold    TEXT    NOT NULL,
    amount  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (char_id, hold)
)]],
[[CREATE TABLE IF NOT EXISTS parties (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    leader_char_id INTEGER NOT NULL REFERENCES characters(id),
    created_at     INTEGER NOT NULL
)]],
[[CREATE TABLE IF NOT EXISTS party_members (
    party_id INTEGER NOT NULL REFERENCES parties(id),
    char_id  INTEGER NOT NULL REFERENCES characters(id),
    PRIMARY KEY (party_id, char_id)
)]],
[[CREATE TABLE IF NOT EXISTS audit_log (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    char_id    INTEGER NOT NULL REFERENCES characters(id),
    timestamp  INTEGER NOT NULL,
    event_type TEXT    NOT NULL,
    detail     TEXT
)]],
[[CREATE TABLE IF NOT EXISTS character_appearance (
    char_id  INTEGER PRIMARY KEY REFERENCES characters(id),
    data     TEXT    NOT NULL
)]],
}

local SKILLS = {
    "Armorer","Athletics","Blade","Block","Blunt","Hand To Hand","Heavy Armor",
    "Alchemy","Alteration","Conjuration","Destruction","Illusion","Mysticism","Restoration",
    "Acrobatics","Light Armor","Marksman","Mercantile","Security","Sneak","Speechcraft",
}
local ATTRIBUTES = {
    "Strength","Intelligence","Willpower","Agility","Speed","Endurance","Personality","Luck",
}

-- ── Open / Close ──────────────────────────────────────────────────────────────

local function execClose(sql)
    local r, err = conn:execute(sql)
    if not r then error("DB execClose error: " .. tostring(err) .. "\nSQL: " .. sql:sub(1, 80)) end
    if type(r) == "userdata" then r:close() end
end

function M.open(path)
    env  = luasql.sqlite3()
    conn = assert(env:connect(path))
    execClose("PRAGMA foreign_keys = ON")
    execClose("PRAGMA journal_mode = WAL")
    for _, tbl in ipairs(TABLES) do
        execClose(tbl)
    end
end

function M.close()
    if conn then conn:close() end
    if env  then env:close()  end
end

-- ── Accounts ──────────────────────────────────────────────────────────────────

-- ── Characters ────────────────────────────────────────────────────────────────

function M.getCharByToken(token)
    local cur = query(("SELECT * FROM characters WHERE token='%s'"):format(esc(token)))
    local row = cur:fetch({}, "a")
    cur:close()
    return rowOrNil(row)
end

function M.getCharacterById(id)
    local cur = query(("SELECT * FROM characters WHERE id=%d"):format(tonumber(id)))
    local row = cur:fetch({}, "a")
    cur:close()
    return rowOrNil(row)
end

function M.getCharacterByName(name)
    local cur = query(("SELECT * FROM characters WHERE name='%s'"):format(esc(name)))
    local row = cur:fetch({}, "a")
    cur:close()
    return rowOrNil(row)
end

function M.createCharByToken(token, name)
    local now = os.time()
    exec(("INSERT INTO characters (token,name,created_at,last_seen) VALUES('%s','%s',%d,%d)")
        :format(esc(token), esc(name), now, now))
    local char = M.getCharacterByName(name)
    if not char then return nil end
    local cid = tonumber(char.id)

    -- No default skills/attrs — the client pushes real values via CHAR_SAVE after connection.
    -- Default vitals only (shown on CHAR_LOAD before first save checkpoint).
    exec(("INSERT INTO character_vitals (char_id,health,magicka,stamina,fame,infamy,gold) VALUES(%d,100,100,100,0,0,100)"):format(cid))

    return M.getCharacterById(cid)
end

function M.setTutorialComplete(char_id)
    exec(("UPDATE characters SET tutorial_complete=1 WHERE id=%d"):format(tonumber(char_id)))
end

function M.updateCharacterSeen(char_id)
    exec(("UPDATE characters SET last_seen=%d WHERE id=%d"):format(os.time(), tonumber(char_id)))
end

function M.updateCharacterPosition(char_id, cell, x, y, z)
    exec(("UPDATE characters SET last_cell='%s',pos_x=%f,pos_y=%f,pos_z=%f WHERE id=%d")
        :format(esc(cell or ""), tonumber(x) or 0, tonumber(y) or 0, tonumber(z) or 0, tonumber(char_id)))
end

function M.updateCharacterLevel(char_id, level)
    exec(("UPDATE characters SET level=%d WHERE id=%d"):format(tonumber(level), tonumber(char_id)))
end

-- Bulk update from a CHAR_SAVE checkpoint. Each argument may be nil to skip that field.
function M.updateCharacterFull(char_id, level, skills, attrs, health, magicka, stamina, pos_x, pos_y, pos_z)
    local cid = tonumber(char_id)
    if level then
        exec(("UPDATE characters SET level=%d WHERE id=%d"):format(tonumber(level), cid))
    end
    if pos_x and pos_y and pos_z then
        exec(("UPDATE characters SET pos_x=%f,pos_y=%f,pos_z=%f WHERE id=%d")
            :format(tonumber(pos_x), tonumber(pos_y), tonumber(pos_z), cid))
    end
    if skills then
        local parts = {}
        for name, value in pairs(skills) do
            parts[#parts + 1] = ("(%d,'%s',%d)"):format(cid, esc(name), tonumber(value) or 0)
        end
        if #parts > 0 then
            exec("INSERT OR REPLACE INTO character_skills (char_id,skill,value) VALUES "
                .. table.concat(parts, ","))
        end
    end
    if attrs then
        local parts = {}
        for name, value in pairs(attrs) do
            parts[#parts + 1] = ("(%d,'%s',%d)"):format(cid, esc(name), tonumber(value) or 0)
        end
        if #parts > 0 then
            exec("INSERT OR REPLACE INTO character_attributes (char_id,attr,value) VALUES "
                .. table.concat(parts, ","))
        end
    end
    if health or magicka or stamina then
        local v = M.getCharacterVitals(cid) or {}
        M.setCharacterVitals(cid,
            tonumber(health)  or tonumber(v.health)  or 100,
            tonumber(magicka) or tonumber(v.magicka) or 100,
            tonumber(stamina) or tonumber(v.stamina) or 100,
            tonumber(v.fame)   or 0,
            tonumber(v.infamy) or 0,
            tonumber(v.gold)   or 100)
    end
end

-- Skills

function M.getCharacterSkills(char_id)
    local cur = query(("SELECT skill,value FROM character_skills WHERE char_id=%d"):format(tonumber(char_id)))
    local skills = {}
    local row = cur:fetch({}, "a")
    while row and row.skill do
        skills[row.skill] = tonumber(row.value)
        row = cur:fetch({}, "a")
    end
    cur:close()
    return skills
end

function M.setCharacterSkill(char_id, skill, value)
    exec(("INSERT INTO character_skills (char_id,skill,value) VALUES(%d,'%s',%d) ON CONFLICT(char_id,skill) DO UPDATE SET value=%d")
        :format(tonumber(char_id), esc(skill), tonumber(value), tonumber(value)))
end

-- Attributes

function M.getCharacterAttributes(char_id)
    local cur = query(("SELECT attr,value FROM character_attributes WHERE char_id=%d"):format(tonumber(char_id)))
    local attrs = {}
    local row = cur:fetch({}, "a")
    while row and row.attr do
        attrs[row.attr] = tonumber(row.value)
        row = cur:fetch({}, "a")
    end
    cur:close()
    return attrs
end

function M.setCharacterAttribute(char_id, attr, value)
    exec(("INSERT INTO character_attributes (char_id,attr,value) VALUES(%d,'%s',%d) ON CONFLICT(char_id,attr) DO UPDATE SET value=%d")
        :format(tonumber(char_id), esc(attr), tonumber(value), tonumber(value)))
end

-- Vitals

function M.getCharacterVitals(char_id)
    local cur = query(("SELECT * FROM character_vitals WHERE char_id=%d"):format(tonumber(char_id)))
    local row = cur:fetch({}, "a")
    cur:close()
    return rowOrNil(row)
end

function M.setCharacterVitals(char_id, health, magicka, stamina, fame, infamy, gold)
    exec(("INSERT INTO character_vitals (char_id,health,magicka,stamina,fame,infamy,gold) VALUES(%d,%f,%f,%f,%d,%d,%d) ON CONFLICT(char_id) DO UPDATE SET health=%f,magicka=%f,stamina=%f,fame=%d,infamy=%d,gold=%d")
        :format(tonumber(char_id),
                tonumber(health) or 100, tonumber(magicka) or 100, tonumber(stamina) or 100,
                tonumber(fame) or 0, tonumber(infamy) or 0, tonumber(gold) or 100,
                tonumber(health) or 100, tonumber(magicka) or 100, tonumber(stamina) or 100,
                tonumber(fame) or 0, tonumber(infamy) or 0, tonumber(gold) or 100))
end

function M.addGold(char_id, amount)
    exec(("UPDATE character_vitals SET gold=MAX(0,gold+%d) WHERE char_id=%d"):format(tonumber(amount), tonumber(char_id)))
end

-- ── Character quests ──────────────────────────────────────────────────────────

function M.getCharacterQuests(char_id)
    local cur = query(("SELECT quest_id,stage FROM character_quests WHERE char_id=%d"):format(tonumber(char_id)))
    local quests = {}
    local row = cur:fetch({}, "a")
    while row and row.quest_id do
        quests[row.quest_id] = tonumber(row.stage)
        row = cur:fetch({}, "a")
    end
    cur:close()
    return quests
end

function M.upsertCharacterQuest(char_id, quest_id, stage)
    exec(("INSERT INTO character_quests (char_id,quest_id,stage) VALUES(%d,'%s',%d) ON CONFLICT(char_id,quest_id) DO UPDATE SET stage=%d")
        :format(tonumber(char_id), esc(quest_id), tonumber(stage), tonumber(stage)))
end

-- ── Global quests (unchanged) ─────────────────────────────────────────────────

function M.getAllGlobalQuests()
    local cur = query("SELECT quest_id,stage FROM global_quests")
    local quests = {}
    local row = cur:fetch({}, "a")
    while row and row.quest_id do
        quests[row.quest_id] = tonumber(row.stage)
        row = cur:fetch({}, "a")
    end
    cur:close()
    return quests
end

function M.getGlobalQuestStage(quest_id)
    local cur = query(("SELECT stage FROM global_quests WHERE quest_id='%s'"):format(esc(quest_id)))
    local row = cur:fetch({}, "n")
    cur:close()
    return (row and row[1]) and tonumber(row[1]) or 0
end

function M.upsertGlobalQuest(quest_id, stage)
    exec(("INSERT INTO global_quests (quest_id,stage,updated_at) VALUES('%s',%d,%d) ON CONFLICT(quest_id) DO UPDATE SET stage=%d,updated_at=%d")
        :format(esc(quest_id), tonumber(stage), os.time(), tonumber(stage), os.time()))
end

-- ── Dungeon clears ────────────────────────────────────────────────────────────

function M.getDungeonClear(dungeon_id)
    local cur = query(("SELECT * FROM dungeon_clears WHERE dungeon_id='%s'"):format(esc(dungeon_id)))
    local row = cur:fetch({}, "a")
    cur:close()
    return rowOrNil(row)
end

function M.setDungeonClear(dungeon_id, char_id, char_name, cooldown_hours)
    local now     = os.time()
    local cooldown = now + (tonumber(cooldown_hours) or 0) * 3600
    exec(("INSERT INTO dungeon_clears (dungeon_id,char_id,char_name,cleared_at,cooldown_until) VALUES('%s',%d,'%s',%d,%d) ON CONFLICT(dungeon_id) DO UPDATE SET char_id=%d,char_name='%s',cleared_at=%d,cooldown_until=%d")
        :format(esc(dungeon_id), tonumber(char_id), esc(char_name or ""), now, cooldown,
                tonumber(char_id), esc(char_name or ""), now, cooldown))
end

-- ── Faction ranks ─────────────────────────────────────────────────────────────

function M.getFactionRanks(char_id)
    local cur = query(("SELECT faction_id,rank FROM faction_ranks WHERE char_id=%d"):format(tonumber(char_id)))
    local ranks = {}
    local row = cur:fetch({}, "a")
    while row and row.faction_id do
        ranks[row.faction_id] = tonumber(row.rank)
        row = cur:fetch({}, "a")
    end
    cur:close()
    return ranks
end

function M.setFactionRank(char_id, faction_id, rank)
    exec(("INSERT INTO faction_ranks (char_id,faction_id,rank) VALUES(%d,'%s',%d) ON CONFLICT(char_id,faction_id) DO UPDATE SET rank=%d")
        :format(tonumber(char_id), esc(faction_id), tonumber(rank), tonumber(rank)))
end

-- ── Bounties ──────────────────────────────────────────────────────────────────

function M.getBounties(char_id)
    local cur = query(("SELECT hold,amount FROM bounties WHERE char_id=%d"):format(tonumber(char_id)))
    local bounties = {}
    local row = cur:fetch({}, "a")
    while row and row.hold do
        bounties[row.hold] = tonumber(row.amount)
        row = cur:fetch({}, "a")
    end
    cur:close()
    return bounties
end

function M.setBounty(char_id, hold, amount)
    exec(("INSERT INTO bounties (char_id,hold,amount) VALUES(%d,'%s',%d) ON CONFLICT(char_id,hold) DO UPDATE SET amount=%d")
        :format(tonumber(char_id), esc(hold), tonumber(amount), tonumber(amount)))
end

-- ── Parties ───────────────────────────────────────────────────────────────────

function M.createParty(leader_char_id)
    exec(("INSERT INTO parties (leader_char_id,created_at) VALUES(%d,%d)"):format(tonumber(leader_char_id), os.time()))
    local cur = query("SELECT last_insert_rowid() AS id")
    local row = cur:fetch({}, "a")
    cur:close()
    local party_id = tonumber(row and row.id)
    if party_id then
        exec(("INSERT INTO party_members (party_id,char_id) VALUES(%d,%d)"):format(party_id, tonumber(leader_char_id)))
    end
    return party_id
end

function M.getPartyByChar(char_id)
    local cur = query(("SELECT pm.party_id FROM party_members pm WHERE pm.char_id=%d"):format(tonumber(char_id)))
    local row = cur:fetch({}, "n")
    cur:close()
    return row and tonumber(row[1])
end

function M.getPartyMembers(party_id)
    local cur = query(("SELECT char_id FROM party_members WHERE party_id=%d"):format(tonumber(party_id)))
    local members = {}
    local row = cur:fetch({}, "n")
    while row and row[1] do
        members[#members + 1] = tonumber(row[1])
        row = cur:fetch({}, "n")
    end
    cur:close()
    return members
end

function M.getPartyLeader(party_id)
    local cur = query(("SELECT leader_char_id FROM parties WHERE id=%d"):format(tonumber(party_id)))
    local row = cur:fetch({}, "n")
    cur:close()
    return row and tonumber(row[1])
end

function M.addPartyMember(party_id, char_id)
    exec(("INSERT OR IGNORE INTO party_members (party_id,char_id) VALUES(%d,%d)"):format(tonumber(party_id), tonumber(char_id)))
end

function M.removePartyMember(char_id)
    local party_id = M.getPartyByChar(char_id)
    if not party_id then return end
    exec(("DELETE FROM party_members WHERE char_id=%d"):format(tonumber(char_id)))
    -- Delete party if empty
    local members = M.getPartyMembers(party_id)
    if #members == 0 then
        exec(("DELETE FROM parties WHERE id=%d"):format(party_id))
    elseif M.getPartyLeader(party_id) == char_id then
        -- Transfer leadership to first remaining member
        exec(("UPDATE parties SET leader_char_id=%d WHERE id=%d"):format(members[1], party_id))
    end
end

function M.deleteParty(party_id)
    exec(("DELETE FROM party_members WHERE party_id=%d"):format(tonumber(party_id)))
    exec(("DELETE FROM parties WHERE id=%d"):format(tonumber(party_id)))
end

-- ── Character appearance ──────────────────────────────────────────────────────

function M.saveAppearance(char_id, json_blob)
    exec(("INSERT OR REPLACE INTO character_appearance (char_id,data) VALUES(%d,'%s')")
        :format(tonumber(char_id), esc(json_blob)))
end

function M.getAppearance(char_id)
    local cur = query(("SELECT data FROM character_appearance WHERE char_id=%d"):format(tonumber(char_id)))
    local row = rowOrNil(cur:fetch({}, "a"))
    cur:close()
    return row and row.data or nil
end

function M.updateCharacterName(char_id, name)
    exec(("UPDATE characters SET name='%s' WHERE id=%d"):format(esc(name), tonumber(char_id)))
end

-- ── Audit log ─────────────────────────────────────────────────────────────────

function M.logAudit(char_id, event_type, detail)
    exec(("INSERT INTO audit_log (char_id,timestamp,event_type,detail) VALUES(%d,%d,'%s','%s')")
        :format(tonumber(char_id) or 0, os.time(), esc(event_type), esc(detail or "")))
end

function M.clearAuditFlags(char_id)
    exec(("DELETE FROM audit_log WHERE char_id=%d AND event_type LIKE 'SUSPICIOUS_%%'")
        :format(tonumber(char_id)))
end

function M.getAuditLog(char_id, limit)
    local cur = query(("SELECT * FROM audit_log WHERE char_id=%d ORDER BY timestamp DESC LIMIT %d")
        :format(tonumber(char_id), tonumber(limit) or 20))
    return fetchAll(cur)
end

return M
