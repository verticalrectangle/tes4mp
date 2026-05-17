-- world.lua — Dungeon clears, faction ranks, bounties, position tracking.

local store   = require("server.db.store")
local session = require("server.modules.session")

local M = {}

local config = nil

function M.init(cfg)
    config = cfg
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
        local fid = tonumber(pkt.cell) or 0
        return fid ~= 0 and tostring(fid) or ""
    else
        -- Exterior: bucket into zone grid so adjacent cells share a key.
        local gx = math.floor(x / EXTERIOR_ZONE)
        local gy = math.floor(y / EXTERIOR_ZONE)
        return "E" .. ws .. ":" .. gx .. ":" .. gy
    end
end

function M.handlePositionUpdate(char_id, pkt)
    local json = require("cjson")
    local socket = require("socket")

    local x    = tonumber(pkt.x)   or 0
    local y    = tonumber(pkt.y)   or 0
    local z    = tonumber(pkt.z)   or 0
    local rot  = tonumber(pkt.rot) or 0
    local anim = tonumber(pkt.anim) or 0
    local now  = socket.gettime()
    local cell = cellKey(pkt, x, y)

    -- Speed validation (horizontal distance only — ignore Z for jumps/ledges)
    local last = session.getPosCache(char_id)
    if last and last.t > 0 then
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

        if cell ~= "" then
            -- Tell new cell members this ghost is appearing (always, appearance optional)
            local appear    = session.getAppearanceCached(char_id)
            local appearpkt = { type = "GHOST_APPEAR", char_id = tostring(char_id),
                                 char_name = sess.name, x = x, y = y, z = z, rot = rot }
            if appear then
                local ok, decoded = pcall(json.decode, appear)
                if ok then appearpkt.appearance = decoded end
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

function M.handleCharSave(char_id, pkt)
    local sess = session.getByCharId(char_id)
    if not sess then return end

    -- Sync name if it changed
    local newName = tostring(pkt.name or ""):match("^%s*(.-)%s*$")
    if #newName >= 2 and #newName <= 24 and newName ~= sess.name then
        store.updateCharacterName(char_id, newName)
        sess.name = newName
        print(("[world] char %d renamed to %s"):format(char_id, newName))
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

-- ── NPC kill sync ─────────────────────────────────────────────────────────────

function M.handleNpcKilled(char_id, pkt)
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

return M
