-- world.lua — Dungeon clears, faction ranks, bounties, position tracking.

local store   = require("server.db.store")
local session = require("server.modules.session")

local M = {}

local config = nil
local currentWeather = 0  -- last weather formId reported by any player

function M.init(cfg)
    config = cfg
end

-- ── Cell authority ────────────────────────────────────────────────────────────
-- First player in a cell owns NPC simulation for it (sends NPC_HP batches).
-- Handover goes to any remaining cell member when the authority leaves.

local authorities = {}  -- cell key → char_id

local function sendAuthority(char_id, cell, isAuth)
    local json = require("cjson")
    session.sendTo(char_id, json.encode({
        type = "CELL_AUTHORITY", cell = cell, authority = isAuth }))
end

function M.getAuthority(cell)
    return authorities[cell]
end

function M.assignAuthority(cell, char_id)
    if cell == "" then return end
    if not authorities[cell] then
        authorities[cell] = char_id
        sendAuthority(char_id, cell, true)
    else
        sendAuthority(char_id, cell, false)
    end
end

function M.releaseAuthority(cell, char_id)
    if cell == "" or authorities[cell] ~= char_id then return end
    authorities[cell] = nil
    local remaining = session.getInCell(cell, char_id)
    if #remaining > 0 then
        authorities[cell] = remaining[1].char_id
        sendAuthority(remaining[1].char_id, cell, true)
    end
end

function M.getCurrentWeather()
    return currentWeather
end

-- ── Position ──────────────────────────────────────────────────────────────────

local MAX_MOVE_SPEED = 1000  -- units/s — generous cap; enchantments can push ~600

-- Exterior zone size: 3 Oblivion exterior cells (4096 units each).
-- Players within the same zone see each other without GHOST_APPEAR/LEAVE cycling.
local EXTERIOR_ZONE = 4096 * 3

local function cellKey(pkt, x, y)
    local ws = tonumber(pkt.ws) or 0
    if ws == 0 then
        -- Interior: use exact cell formID. Treat formID 0 as "no cell".
        -- Use integer formatting: cjson returns numbers as floats, and tostring(666001.0)
        -- produces "666001.0" in Lua 5.3+ — we need "666001" to match string cell keys.
        local fid = math.floor(tonumber(pkt.cell) or 0)
        return fid ~= 0 and ("%d"):format(fid) or ""
    else
        -- Exterior: bucket into zone grid so adjacent cells share a key.
        -- %d formatting throughout: cjson yields floats and "E60.0:..." would
        -- never match the client's "E60:..." key (breaks authority + TP).
        local gx = math.floor(x / EXTERIOR_ZONE)
        local gy = math.floor(y / EXTERIOR_ZONE)
        return ("E%d:%d:%d"):format(math.floor(ws), gx, gy)
    end
end

-- nil for NaN/Inf/non-numbers — clients can sample garbage during loading
-- screens, and cjson refuses to re-encode non-finite values (packet error).
local function finite(v)
    local n = tonumber(v)
    if not n or n ~= n or n == math.huge or n == -math.huge then return nil end
    return n
end

function M.handlePositionUpdate(char_id, pkt)
    local json = require("cjson")
    local socket = require("socket")

    local x   = finite(pkt.x)
    local y   = finite(pkt.y)
    local z   = finite(pkt.z)
    local rot = finite(pkt.rot)
    if not (x and y and z and rot) then return end  -- corrupt sample, drop
    local anim = math.floor(finite(pkt.anim) or 0)
    local now  = socket.gettime()
    local cell = cellKey(pkt, x, y)

    -- Speed validation (horizontal distance only — ignore Z for jumps/ledges).
    -- Fast travel (ft=1) is exempt: the client teleports, so the distance is expected.
    local isFastTravel = tonumber(pkt.ft) == 1
    local last = session.getPosCache(char_id)
    if not isFastTravel and last and last.t > 0 then
        local dt = now - last.t
        if dt > 0 then
            local dist  = math.sqrt((x - last.x)^2 + (y - last.y)^2)
            local speed = dist / dt
            if speed > MAX_MOVE_SPEED then
                store.logAudit(char_id, "SPEED_HACK",
                    ("%.1f > %d u/s"):format(speed, MAX_MOVE_SPEED))
                return  -- reject; no broadcast
            end
        end
    end
    session.updatePosCache(char_id, x, y, z, now)

    -- Interior cell editor ID (for TP_REQUEST teleports); sent when it changes
    if pkt.ced ~= nil then
        local sess0 = session.getByCharId(char_id)
        if sess0 then sess0.cell_ed = tostring(pkt.ced) end
    end

    -- Cell transition
    local sess = session.getByCharId(char_id)
    if not sess then return end
    local oldCell = sess.cell or ""

    if oldCell ~= cell then
        -- Tell old cell members this ghost is leaving
        if oldCell ~= "" then
            session.broadcastToCell(oldCell,
                json.encode({ type = "GHOST_LEAVE", char_id = tostring(char_id) }),
                char_id)
        end

        session.updateCell(char_id, cell)
        if oldCell ~= "" then M.releaseAuthority(oldCell, char_id) end
        if cell ~= "" then M.assignAuthority(cell, char_id) end

        if cell ~= "" then
            -- Tell new cell members this ghost is appearing (always, appearance optional)
            local appear    = session.getAppearanceCached(char_id)
            local appearpkt = { type = "GHOST_APPEAR", char_id = tostring(char_id),
                                 char_name = sess.name, x = x, y = y, z = z, rot = rot }
            if appear then
                local ok, decoded = pcall(json.decode, appear)
                if ok then appearpkt.appearance = decoded end
            end
            local equip = session.getEquipmentCached(char_id)
            if equip then
                local okE, decodedE = pcall(json.decode, equip)
                if okE then appearpkt.equipment = decodedE end
            end
            -- Pass fast-travel flag so peers show a destination HUD message
            if pkt.ft and tonumber(pkt.ft) == 1 then
                appearpkt.fast_travel = true
                appearpkt.cell_name   = tostring(pkt.cn or "")
            end
            session.broadcastToCell(cell, json.encode(appearpkt), char_id)

            -- Tell the moving player about everyone already in the new cell
            for _, other in ipairs(session.getInCell(cell, char_id)) do
                local otherpkt = { type = "GHOST_APPEAR", char_id = tostring(other.char_id),
                                   char_name = other.name,
                                   x = other.last_pos.x, y = other.last_pos.y,
                                   z = other.last_pos.z, rot = 0 }
                local otherAppear = session.getAppearanceCached(other.char_id)
                if otherAppear then
                    local ok2, decoded2 = pcall(json.decode, otherAppear)
                    if ok2 then otherpkt.appearance = decoded2 end
                end
                local otherEquip = session.getEquipmentCached(other.char_id)
                if otherEquip then
                    local ok3, decoded3 = pcall(json.decode, otherEquip)
                    if ok3 then otherpkt.equipment = decoded3 end
                end
                session.sendTo(char_id, json.encode(otherpkt))
            end

            -- Send kill list for dead NPCs in the new cell
            local killed = store.getKilledRefs(cell)
            if #killed > 0 then
                session.sendTo(char_id, json.encode({ type = "NPC_KILL_SYNC", refs = killed }))
            end

            -- Send looted container state for the new cell
            local containers = store.getContainerState(cell)
            if #containers > 0 then
                session.sendTo(char_id, json.encode({ type = "CONTAINER_STATE", containers = containers }))
            end
        end
    end

    -- Broadcast position to everyone in same cell
    if cell ~= "" then
        session.broadcastToCell(cell,
            json.encode({ type = "PLAYER_POS", char_id = tostring(char_id),
                          x = x, y = y, z = z, rot = rot, anim = anim,
                          hp = tonumber(pkt.hp) or 0 }),
            char_id)
    end
end

-- ── Appearance (write-once) ───────────────────────────────────────────────────

function M.handleAppearance(char_id, pkt)
    local json = require("cjson")

    -- Basic validation
    if type(pkt.race) ~= "number" then return end

    local blob = json.encode({
        race   = pkt.race,
        gender = pkt.gender,
        hair   = pkt.hair,
        eyes   = pkt.eyes,
        geo    = pkt.geo,
        tex    = pkt.tex,
    })
    store.saveAppearance(char_id, blob)
    session.cacheAppearance(char_id, blob)
    print(("[world] appearance stored for char %d"):format(char_id))

    -- Broadcast to current cell so nearby players who missed the cell-transition
    -- GHOST_APPEAR (race: APPEARANCE arrived after first POSITION_UPDATE) catch up.
    local sess = session.getByCharId(char_id)
    if sess and sess.cell and sess.cell ~= "" then
        local ok, decoded = pcall(json.decode, blob)
        if ok then
            session.broadcastToCell(sess.cell, json.encode({
                type       = "GHOST_APPEAR",
                char_id    = tostring(char_id),
                char_name  = sess.name,
                x = sess.last_pos.x, y = sess.last_pos.y, z = sess.last_pos.z,
                rot = 0,
                appearance = decoded,
            }), char_id)
        end
    end
end

-- ── Periodic character save (CHAR_SAVE) ──────────────────────────────────────
-- Validates each delta against stored state to bound cheating.
-- Suspicious gains are clamped and audit-logged; the save still proceeds.

local MAX_SKILL_DELTA = 5   -- max skill gain per 60s checkpoint
local MAX_ATTR_DELTA  = 6   -- max attribute gain per checkpoint (level-up gives ≤5 to one attr)
local MAX_LEVEL_DELTA = 1   -- can only level up once per 60s

-- A stat table where every value is 0 is a client sampling glitch (loading
-- screen / chargen), never a real character. Persisting it bricks the char.
local function allZero(t)
    local any = false
    for _, v in pairs(t) do
        any = true
        if (tonumber(v) or 0) ~= 0 then return false end
    end
    return any
end

function M.handleCharSave(char_id, pkt)
    local sess = session.getByCharId(char_id)
    if not sess then return end

    -- Sync name if it changed (names are UNIQUE — skip if another char owns it)
    local newName = tostring(pkt.name or ""):match("^%s*(.-)%s*$")
    if #newName >= 2 and #newName <= 24 and newName ~= sess.name then
        local existing = store.getCharacterByName(newName)
        if not existing then
            store.updateCharacterName(char_id, newName)
            sess.name = newName
            print(("[world] char %d renamed to %s"):format(char_id, newName))
        end
    end

    -- Validate level against session cache — no DB read needed
    local oldLevel = tonumber(sess.char.level) or 1
    local newLevel = tonumber(pkt.level)
    if newLevel then
        if newLevel < oldLevel then
            newLevel = oldLevel
        elseif newLevel - oldLevel > MAX_LEVEL_DELTA then
            store.logAudit(char_id, "SUSPICIOUS_LEVEL",
                ("level %d→%d in 15s"):format(oldLevel, newLevel))
            newLevel = oldLevel + MAX_LEVEL_DELTA
        end
    end

    if type(pkt.skills)     == "table" and allZero(pkt.skills)     then pkt.skills     = nil end
    if type(pkt.attributes) == "table" and allZero(pkt.attributes) then
        store.logAudit(char_id, "ZERO_STATS_DROPPED", "all-zero attribute checkpoint ignored")
        pkt.attributes = nil
    end

    local newSkills = nil
    if type(pkt.skills) == "table" then
        local oldSkills = sess.cached_skills or {}
        newSkills = {}
        for skill, val in pairs(pkt.skills) do
            local v   = math.max(0, math.min(100, tonumber(val) or 0))
            local old = tonumber(oldSkills[skill]) or v
            if v - old > MAX_SKILL_DELTA then
                store.logAudit(char_id, "SUSPICIOUS_SKILL",
                    ("%s %d→%d"):format(skill, old, v))
                v = old + MAX_SKILL_DELTA
            end
            newSkills[skill] = v
        end
    end

    local newAttrs = nil
    if type(pkt.attributes) == "table" then
        local oldAttrs = sess.cached_attrs or {}
        newAttrs = {}
        for attr, val in pairs(pkt.attributes) do
            local v   = math.max(0, math.min(100, tonumber(val) or 0))
            local old = tonumber(oldAttrs[attr]) or v
            if v - old > MAX_ATTR_DELTA then
                store.logAudit(char_id, "SUSPICIOUS_ATTR",
                    ("%s %d→%d"):format(attr, old, v))
                v = old + MAX_ATTR_DELTA
            end
            newAttrs[attr] = v
        end
    end

    -- Mark tutorial complete on first CHAR_SAVE so the skip stops firing
    if (tonumber(sess.char.tutorial_complete) or 0) == 0 then
        store.setTutorialComplete(char_id)
        sess.char.tutorial_complete = 1
    end

    -- Update in-memory cache before the DB write so future checkpoints
    -- compare against the validated values, not the pre-save ones
    session.updateStatCache(char_id, newSkills, newAttrs, newLevel)

    store.transaction(function()
        store.updateCharacterFull(char_id, newLevel, newSkills, newAttrs,
            tonumber(pkt.health), tonumber(pkt.magicka), tonumber(pkt.stamina),
            tonumber(pkt.pos_x),  tonumber(pkt.pos_y),   tonumber(pkt.pos_z))
    end)
end

-- ── Dungeon clears ────────────────────────────────────────────────────────────

function M.handleDungeonClear(char_id, pkt)
    local json      = require("cjson")
    local dungeon   = tostring(pkt.dungeon_id or "")
    if dungeon == "" then return end

    local sender    = session.getByCharId(char_id)
    if not sender   then return end

    local existing  = store.getDungeonClear(dungeon)
    local cooldown  = config and config.dungeon_cooldown_hours or 0

    -- Cooldown check
    if existing and tonumber(existing.cooldown_until) > os.time() then
        local hrs = math.ceil((tonumber(existing.cooldown_until) - os.time()) / 3600)
        session.sendTo(char_id, json.encode({
            type    = "DUNGEON_STATUS",
            dungeon_id = dungeon,
            cleared = true,
            clearer = existing.char_name,
            cooldown_hours = hrs,
        }))
        return
    end

    store.setDungeonClear(dungeon, char_id, sender.name, cooldown)
    session.broadcast(json.encode({
        type      = "DUNGEON_CLEARED",
        dungeon_id = dungeon,
        clearer   = sender.name,
    }))
    print(("[world] dungeon %s cleared by %s"):format(dungeon, sender.name))
end

function M.handleDungeonQuery(char_id, pkt)
    local json    = require("cjson")
    local dungeon = tostring(pkt.dungeon_id or "")
    if dungeon == "" then return end

    local rec = store.getDungeonClear(dungeon)
    if rec then
        session.sendTo(char_id, json.encode({
            type       = "DUNGEON_STATUS",
            dungeon_id = dungeon,
            cleared    = true,
            clearer    = rec.char_name,
            cleared_at = tonumber(rec.cleared_at),
            on_cooldown = tonumber(rec.cooldown_until) > os.time(),
        }))
    else
        session.sendTo(char_id, json.encode({
            type       = "DUNGEON_STATUS",
            dungeon_id = dungeon,
            cleared    = false,
        }))
    end
end

-- ── Faction ranks ─────────────────────────────────────────────────────────────

function M.handleFactionRank(char_id, pkt)
    local json      = require("cjson")
    local faction   = tostring(pkt.faction_id or "")
    local rank      = tonumber(pkt.rank)
    if faction == "" or not rank then return end

    local sender    = session.getByCharId(char_id)
    if not sender then return end

    local current   = store.getFactionRanks(char_id)[faction] or -1
    if rank <= current then return end  -- forward-progress only

    store.setFactionRank(char_id, faction, rank)
    session.broadcast(json.encode({
        type       = "FACTION_RANK",
        char_name  = sender.name,
        faction_id = faction,
        rank       = rank,
    }))
    print(("[world] %s faction %s → rank %d"):format(sender.name, faction, rank))
end

-- ── Bounties ──────────────────────────────────────────────────────────────────

function M.handleCrimeReport(char_id, pkt)
    local hold  = tostring(pkt.hold or "")
    local delta = tonumber(pkt.bounty_delta) or 0
    if hold == "" or delta <= 0 then return end

    local bounties = store.getBounties(char_id)
    local current  = bounties[hold] or 0
    store.setBounty(char_id, hold, current + delta)
end

function M.handleBountyPaid(char_id, pkt)
    local hold = tostring(pkt.hold or "")
    if hold == "" then return end
    store.setBounty(char_id, hold, 0)
end

-- ── Player death ─────────────────────────────────────────────────────────────

function M.handlePlayerDied(char_id, pkt)
    local json = require("cjson")
    local sess = session.getByCharId(char_id)
    if not sess or not sess.cell or sess.cell == "" then return end
    session.broadcastToCell(sess.cell,
        json.encode({ type = "PLAYER_DIED", char_id = tostring(char_id) }), char_id)
end

-- ── Weather sync ──────────────────────────────────────────────────────────────

function M.handleWeatherReport(char_id, pkt)
    local json = require("cjson")
    local wid = tonumber(pkt.weather_id) or 0
    if wid == 0 or wid == currentWeather then return end
    currentWeather = wid
    -- Broadcast to all players so everyone sees the same weather
    session.broadcast(json.encode({ type = "WEATHER_SYNC", weather_id = wid }))
end

-- ── NPC kill sync ─────────────────────────────────────────────────────────────

function M.handleNpcKilled(char_id, pkt)
    local json   = require("cjson")
    local ref_id = tonumber(pkt.ref_id) or 0
    local cell   = tostring(pkt.cell or "")
    if ref_id == 0 or cell == "" then return end
    -- Reject dynamic refs (formIds > 0xFF000000 are runtime-placed)
    if ref_id > 0xFF000000 then return end

    local sess = session.getByCharId(char_id)
    if not sess or sess.cell ~= cell then return end

    local respawnHours = config and config.npc_respawn_hours or 0
    store.setKilledRef(ref_id, cell, char_id, respawnHours)
    session.broadcastToCell(cell,
        json.encode({ type = "NPC_KILLED", ref_id = ref_id }), char_id)
    print(("[world] NPC %d killed by char %d in cell %s"):format(ref_id, char_id, cell))
end

-- ── Container loot sync ───────────────────────────────────────────────────────

function M.handleItemTaken(char_id, pkt)
    local json  = require("cjson")
    local cref  = tonumber(pkt.container_ref_id) or 0
    local iform = tonumber(pkt.item_form_id) or 0
    local count = tonumber(pkt.count) or 0
    local cell  = tostring(pkt.cell or "")
    if cref == 0 or iform == 0 or count <= 0 or count > 500 or cell == "" then return end

    local sess = session.getByCharId(char_id)
    if not sess or sess.cell ~= cell then return end

    store.recordItemTaken(cref, iform, count, cell)
    session.broadcastToCell(cell, json.encode({
        type             = "ITEM_SYNC",
        container_ref_id = cref,
        item_form_id     = iform,
        count            = count,
    }), char_id)
end

-- ── Equipment sync ────────────────────────────────────────────────────────────

function M.handleEquipUpdate(char_id, pkt)
    local json = require("cjson")
    if type(pkt.items) ~= "table" then return end

    local items = {}
    for _, v in ipairs(pkt.items) do
        local id = math.floor(tonumber(v) or 0)
        -- Vanilla Oblivion.esm formIDs only (mod index 00) — resolvable on every client
        if id > 0 and id < 0x01000000 then items[#items + 1] = id end
        if #items >= 20 then break end
    end

    -- Persist as a JSON array string ("[]" when everything was unequipped:
    -- cjson would encode an empty Lua table as an object)
    local blob = (#items > 0) and json.encode(items) or "[]"
    store.saveEquipment(char_id, blob)
    session.cacheEquipment(char_id, blob)

    local sess = session.getByCharId(char_id)
    if sess and sess.cell and sess.cell ~= "" then
        session.broadcastToCell(sess.cell, json.encode({
            type    = "EQUIP_SYNC",
            char_id = tostring(char_id),
            items   = items,
        }), char_id)
    end
end

-- ── NPC HP authority sync ─────────────────────────────────────────────────────

function M.handleNpcHp(char_id, pkt)
    local json = require("cjson")
    local cell = tostring(pkt.cell or "")
    if cell == "" or type(pkt.npcs) ~= "table" then return end

    local sess = session.getByCharId(char_id)
    if not sess or sess.cell ~= cell then return end
    if authorities[cell] ~= char_id then return end  -- only the authority may report

    local npcs = {}
    for _, e in ipairs(pkt.npcs) do
        if type(e) == "table" then
            local ref = math.floor(tonumber(e.ref) or 0)
            local hp  = math.floor(tonumber(e.hp) or -1)
            if ref > 0 and ref < 0xFF000000 and hp >= 0 then
                npcs[#npcs + 1] = { ref = ref, hp = hp }
            end
        end
        if #npcs >= 64 then break end
    end
    if #npcs == 0 then return end

    session.broadcastToCell(cell,
        json.encode({ type = "NPC_HP", cell = cell, npcs = npcs }), char_id)
end

function M.handleNpcDamage(char_id, pkt)
    local json   = require("cjson")
    local cell   = tostring(pkt.cell or "")
    local ref    = math.floor(tonumber(pkt.ref_id) or 0)
    local amount = math.floor(tonumber(pkt.amount) or 0)
    if cell == "" or ref <= 0 or amount <= 0 or amount > 1000 then return end

    local sess = session.getByCharId(char_id)
    if not sess or sess.cell ~= cell then return end

    -- Route to the cell authority; the authority applies its own damage locally
    local auth = authorities[cell]
    if not auth or auth == char_id then return end
    session.sendTo(auth, json.encode({
        type = "NPC_DAMAGE", cell = cell, ref_id = ref, amount = amount }))
end

-- ── Teleport to a fellow player (F9) ─────────────────────────────────────────

function M.handleTpRequest(char_id)
    local json = require("cjson")
    local me = session.getByCharId(char_id)
    if not me then return end

    -- Pick another online player we can actually teleport to: an exterior
    -- zone, or an interior with a known cell editor ID.
    local target = nil
    for cid, sess in pairs(session.getAll()) do
        if cid ~= char_id and sess.cell and sess.cell ~= "" then
            local teleportable = sess.cell:sub(1, 1) == "E"
                or (sess.cell_ed and sess.cell_ed ~= "")
            if teleportable then target = sess; break end
            target = target or sess  -- fallback (yields the "not yet" message)
        end
    end
    if not target then
        session.sendTo(char_id, json.encode({
            type = "MESSAGE", text = "No other player to teleport to." }))
        return
    end

    if target.cell:sub(1, 1) == "E" then
        -- Exterior zone key: E<worldspaceFormId>:<gx>:<gy> (zone = 3 cells)
        local ws, gx, gy = target.cell:match("^E(%d+):(%-?%d+):(%-?%d+)$")
        if ws then
            session.sendTo(char_id, json.encode({
                type = "TELEPORT", cow = 1, ws = tonumber(ws),
                cx = tonumber(gx) * 3 + 1, cy = tonumber(gy) * 3 + 1 }))
        end
    elseif target.cell_ed and target.cell_ed ~= "" then
        session.sendTo(char_id, json.encode({
            type = "TELEPORT", cell = target.cell_ed }))
    else
        session.sendTo(char_id, json.encode({
            type = "MESSAGE", text = target.name .. "'s location isn't teleportable yet." }))
    end
end

-- ── PvP hits ──────────────────────────────────────────────────────────────────

function M.handlePlayerHit(char_id, pkt)
    local json = require("cjson")
    if not (config and config.pvp == true) then return end

    local target = tonumber(pkt.target_char_id)
    local amount = math.floor(tonumber(pkt.amount) or 0)
    if not target or amount <= 0 then return end
    if amount > 100 then amount = 100 end

    local sess  = session.getByCharId(char_id)
    local tsess = session.getByCharId(target)
    if not sess or not tsess then return end
    if sess.cell == "" or sess.cell ~= tsess.cell then return end

    session.sendTo(target, json.encode({
        type = "DAMAGE_TAKEN", amount = amount, from = sess.name }))
end

return M
