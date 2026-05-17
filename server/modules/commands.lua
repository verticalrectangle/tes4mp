local rbac    = require("server.modules.rbac")
local session = require("server.modules.session")
local quests  = require("server.modules.quests")
local store   = require("server.db.store")
local crypto  = require("server.util.crypto")

local M = {}

local config   = {}   -- set by M.init
local handlers = {}

function M.init(cfg)
    config = cfg
end

local function reply(char_id, text)
    local json = require("cjson")
    session.sendTo(char_id, json.encode({ type = "COMMAND_RESULT", text = text }))
end

local function announce(text)
    local json = require("cjson")
    session.broadcast(json.encode({ type = "SERVER_ANNOUNCEMENT", text = text }))
end

local function define(name, perm, fn)
    handlers[name] = { perm = perm, fn = fn }
end

-- ── General ───────────────────────────────────────────────────────────────────

define("help", "cmd.help", function(cid, sess, args)
    local lines = { "Commands available to you:" }
    local function add(line) lines[#lines + 1] = line end
    add("/help                           — this list")
    add("/players                        — list online players")
    add("/who                            — show your character info")
    add("/tell <name> <message>          — private message")
    add("/party invite|leave|members     — party management")
    add("/setpw <old> <new>              — change your password")
    if rbac.can(sess.role, "cmd.kick")      then add("/kick <name>                     — kick a player") end
    if rbac.can(sess.role, "cmd.setstage")  then add("/setstage <questId> <stage>      — force quest stage") end
    if rbac.can(sess.role, "cmd.announce")  then add("/announce <message>              — server-wide announcement") end
    if rbac.can(sess.role, "cmd.ban")       then add("/ban <name> [reason]             — ban account") end
    if rbac.can(sess.role, "cmd.unban")     then add("/unban <name>                    — unban account") end
    if rbac.can(sess.role, "cmd.setrank")   then add("/setrank <name> <role>           — set role") end
    if rbac.can(sess.role, "cmd.give")      then add("/give <name> <itemId> [count]    — give item") end
    if rbac.can(sess.role, "cmd.take")      then add("/take <name> <itemId> [count]    — remove item") end
    if rbac.can(sess.role, "cmd.teleport")  then add("/tp <name> <cell>               — teleport player") end
    if rbac.can(sess.role, "cmd.tphere")    then add("/tphere <name>                  — teleport player to you") end
    if rbac.can(sess.role, "cmd.setlevel")  then add("/setlevel <name> <level>         — set level") end
    if rbac.can(sess.role, "cmd.setskill")  then add("/setskill <name> <skill> <val>   — set skill") end
    if rbac.can(sess.role, "cmd.setattr")   then add("/setattr <name> <attr> <val>     — set attribute") end
    if rbac.can(sess.role, "cmd.addgold")   then add("/addgold <name> <amount>         — add/remove gold") end
    if rbac.can(sess.role, "cmd.setbounty") then add("/setbounty <name> <hold> <amt>   — set bounty") end
    if rbac.can(sess.role, "cmd.syncquests")then add("/syncquests                      — push quest sync to all") end
    if rbac.can(sess.role, "cmd.audit")     then add("/audit <name> [n]               — audit log for player") end
    if rbac.can(sess.role, "cmd.clearflag") then add("/clearflag <name>               — clear suspicious audit flags") end
    if rbac.can(sess.role, "cmd.motd")      then add("/motd set <message>             — update server MOTD") end
    reply(cid, table.concat(lines, "\n"))
end)

define("players", "cmd.players", function(cid, sess, args)
    local lines = { "Online players:" }
    for _, s in pairs(session.getAll()) do
        lines[#lines + 1] = ("  %s [%s] lv.%d"):format(s.name, s.role, tonumber(s.char.level) or 1)
    end
    reply(cid, table.concat(lines, "\n"))
end)

define("who", "cmd.who", function(cid, sess, args)
    local char = sess.char
    reply(cid, ("Character: %s | Race: %s | Class: %s | Level: %d | Role: %s")
        :format(char.name, char.race, char.class, tonumber(char.level) or 1, sess.role))
end)

-- ── /tell <name> <message> ────────────────────────────────────────────────────

define("tell", "cmd.tell", function(cid, sess, args)
    local name = args[1]
    if not name or not args[2] then reply(cid, "Usage: /tell <name> <message>") return end
    local msg = table.concat(args, " ", 2)
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({
        type = "PRIVATE_MSG", from = sess.name, text = msg,
    }))
    reply(cid, "→ " .. target.name .. ": " .. msg)
end)

-- ── /setpw <old> <new> ────────────────────────────────────────────────────────

define("setpw", "cmd.setpw", function(cid, sess, args)
    local old_pw = args[1]
    local new_pw = args[2]
    if not old_pw or not new_pw then reply(cid, "Usage: /setpw <oldpw> <newpw>") return end
    if #new_pw < 6 then reply(cid, "New password must be at least 6 characters") return end
    local account = store.getAccountById(sess.account_id)
    if not crypto.verifyPassword(old_pw, account.password_hash) then
        reply(cid, "Incorrect current password")
        return
    end
    store.setPasswordHash(sess.account_id, crypto.hashPassword(new_pw))
    reply(cid, "Password changed successfully")
end)

-- ── /party ────────────────────────────────────────────────────────────────────

define("party", "cmd.party", function(cid, sess, args)
    local sub = (args[1] or ""):lower()
    local json = require("cjson")

    if sub == "invite" then
        local name = args[2]
        if not name then reply(cid, "Usage: /party invite <name>") return end
        local target = session.getByName(name)
        if not target then reply(cid, "Player not online: " .. name) return end
        session.sendTo(target.char_id, json.encode({
            type = "PARTY_INVITE", from = sess.name,
        }))
        reply(cid, "Party invite sent to " .. target.name)

    elseif sub == "accept" then
        -- The invite sender's name is stored in args[2] by the client
        local from_name = args[2]
        if not from_name then reply(cid, "Usage: /party accept <inviter>") return end
        local leader = session.getByName(from_name)
        if not leader then reply(cid, "Inviter not online") return end
        local party_id = store.getPartyByChar(leader.char_id)
        if not party_id then
            party_id = store.createParty(leader.char_id)
        end
        store.addPartyMember(party_id, cid)
        local members = store.getPartyMembers(party_id)
        for _, mid in ipairs(members) do
            session.sendTo(mid, json.encode({ type = "PARTY_UPDATE", members = members }))
        end
        reply(cid, "Joined " .. from_name .. "'s party")

    elseif sub == "leave" then
        store.removePartyMember(cid)
        reply(cid, "Left party")

    elseif sub == "members" then
        local party_id = store.getPartyByChar(cid)
        if not party_id then reply(cid, "You are not in a party") return end
        local members = store.getPartyMembers(party_id)
        local names = {}
        for _, mid in ipairs(members) do
            local s = session.getByCharId(mid)
            names[#names + 1] = s and s.name or ("char#" .. mid)
        end
        reply(cid, "Party: " .. table.concat(names, ", "))
    else
        reply(cid, "Usage: /party invite|accept|leave|members")
    end
end)

-- ── Moderation ────────────────────────────────────────────────────────────────

define("kick", "cmd.kick", function(cid, sess, args)
    local name = args[1]
    if not name then reply(cid, "Usage: /kick <name>") return end
    if session.kickByName(name, "Kicked by " .. sess.name) then
        reply(cid, "Kicked " .. name)
    else
        reply(cid, "Player not online: " .. name)
    end
end)

define("announce", "cmd.announce", function(cid, sess, args)
    if not args[1] then reply(cid, "Usage: /announce <message>") return end
    local msg = "[" .. sess.name .. "] " .. table.concat(args, " ")
    announce(msg)
    reply(cid, "Announced: " .. msg)
end)

define("setstage", "cmd.setstage", function(cid, sess, args)
    local questId = args[1]
    local stage   = tonumber(args[2])
    if not questId or not stage then
        reply(cid, "Usage: /setstage <questId> <stage>") return
    end
    quests.forceQuestStage(questId, stage, sess.name)
    reply(cid, ("Set %s → stage %d"):format(questId, stage))
end)

define("syncquests", "cmd.syncquests", function(cid, sess, args)
    quests.syncAll()
    reply(cid, "Full quest sync pushed to all players")
end)

-- ── Admin: account/role ───────────────────────────────────────────────────────

define("ban", "cmd.ban", function(cid, sess, args)
    local name   = args[1]
    if not name then reply(cid, "Usage: /ban <name> [reason]") return end
    local reason = #args > 1 and table.concat(args, " ", 2) or ("banned by " .. sess.name)
    local target = session.getByName(name)
    local account_id
    if target then
        account_id = target.account_id
        session.kick(target.char_id, "You have been banned: " .. reason)
    else
        local char = store.getCharacterByName(name)
        if not char then reply(cid, "No character found: " .. name) return end
        account_id = tonumber(char.account_id)
    end
    store.lockAccount(account_id, 9999999999)
    store.logAudit(account_id, "BAN", reason)
    reply(cid, "Banned " .. name .. ": " .. reason)
end)

define("unban", "cmd.unban", function(cid, sess, args)
    local name = args[1]
    if not name then reply(cid, "Usage: /unban <name>") return end
    local char = store.getCharacterByName(name)
    if not char then reply(cid, "No character found: " .. name) return end
    store.lockAccount(tonumber(char.account_id), 0)
    reply(cid, "Unbanned " .. name)
end)

define("setrank", "cmd.setrank", function(cid, sess, args)
    local name = args[1]; local role = args[2]
    if not name or not role then
        reply(cid, "Usage: /setrank <name> <player|moderator|admin>") return
    end
    if not rbac.isValid(role) then
        reply(cid, "Invalid role. Valid: player, moderator, admin") return
    end
    if rbac.rankOf(role) > rbac.rankOf(sess.role) then
        reply(cid, "Cannot assign a role higher than your own") return
    end
    local char = store.getCharacterByName(name)
    if not char then reply(cid, "No character found: " .. name) return end
    store.updateAccountRole(tonumber(char.account_id), role)
    local live = session.getByName(name)
    if live then live.role = role end
    reply(cid, ("Set %s account role → %s"):format(name, role))
end)

-- ── Admin: give / take / gold ─────────────────────────────────────────────────

define("give", "cmd.give", function(cid, sess, args)
    local name   = args[1]; local item = args[2]; local count = tonumber(args[3]) or 1
    if not name or not item then reply(cid, "Usage: /give <name> <itemEditorId> [count]") return end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({
        type = "GIVE_ITEM", item_id = item, count = count,
    }))
    reply(cid, ("Gave %dx %s to %s"):format(count, item, name))
end)

define("take", "cmd.take", function(cid, sess, args)
    local name   = args[1]; local item = args[2]; local count = tonumber(args[3]) or 1
    if not name or not item then reply(cid, "Usage: /take <name> <itemEditorId> [count]") return end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({
        type = "TAKE_ITEM", item_id = item, count = count,
    }))
    reply(cid, ("Took %dx %s from %s"):format(count, item, name))
end)

define("addgold", "cmd.addgold", function(cid, sess, args)
    local name   = args[1]; local amount = tonumber(args[2])
    if not name or not amount then reply(cid, "Usage: /addgold <name> <amount>") return end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    store.addGold(target.char_id, amount)
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({ type = "ADD_GOLD", amount = amount }))
    reply(cid, ("Added %d gold to %s"):format(amount, name))
end)

-- ── Admin: teleport ───────────────────────────────────────────────────────────

define("tp", "cmd.teleport", function(cid, sess, args)
    local name = args[1]
    local cell = args[2] and table.concat(args, " ", 2) or nil
    if not name or not cell then reply(cid, "Usage: /tp <name> <cell name>") return end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({ type = "TELEPORT", cell = cell }))
    store.updateCharacterPosition(target.char_id, cell, 0, 0, 0)
    reply(cid, ("Teleported %s to %s"):format(name, cell))
end)

define("tphere", "cmd.tphere", function(cid, sess, args)
    local name = args[1]
    if not name then reply(cid, "Usage: /tphere <name>") return end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    -- Use caller's stored position
    local my_char = store.getCharacterById(cid)
    if not my_char then return end
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({
        type = "TELEPORT",
        cell = my_char.last_cell,
        x    = tonumber(my_char.pos_x),
        y    = tonumber(my_char.pos_y),
        z    = tonumber(my_char.pos_z),
    }))
    store.updateCharacterPosition(target.char_id, my_char.last_cell,
        tonumber(my_char.pos_x), tonumber(my_char.pos_y), tonumber(my_char.pos_z))
    reply(cid, ("Teleported %s to your location"):format(name))
end)

-- ── Admin: character stats ────────────────────────────────────────────────────

define("setlevel", "cmd.setlevel", function(cid, sess, args)
    local name  = args[1]; local level = tonumber(args[2])
    if not name or not level then reply(cid, "Usage: /setlevel <name> <level>") return end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    store.updateCharacterLevel(target.char_id, level)
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({ type = "SET_LEVEL", level = level }))
    reply(cid, ("Set %s level → %d"):format(name, level))
end)

define("setskill", "cmd.setskill", function(cid, sess, args)
    local name  = args[1]; local skill = args[2]; local val = tonumber(args[3])
    if not name or not skill or not val then
        reply(cid, "Usage: /setskill <name> <skill> <value>") return
    end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    store.setCharacterSkill(target.char_id, skill, val)
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({ type = "SET_SKILL", skill = skill, value = val }))
    reply(cid, ("Set %s %s → %d"):format(name, skill, val))
end)

define("setattr", "cmd.setattr", function(cid, sess, args)
    local name  = args[1]; local attr = args[2]; local val = tonumber(args[3])
    if not name or not attr or not val then
        reply(cid, "Usage: /setattr <name> <attribute> <value>") return
    end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    store.setCharacterAttribute(target.char_id, attr, val)
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({ type = "SET_ATTR", attr = attr, value = val }))
    reply(cid, ("Set %s %s → %d"):format(name, attr, val))
end)

define("setbounty", "cmd.setbounty", function(cid, sess, args)
    local name   = args[1]; local hold = args[2]; local amount = tonumber(args[3])
    if not name or not hold or not amount then
        reply(cid, "Usage: /setbounty <name> <hold> <amount>") return
    end
    local target = session.getByName(name)
    if not target then reply(cid, "Player not online: " .. name) return end
    store.setBounty(target.char_id, hold, amount)
    local json = require("cjson")
    session.sendTo(target.char_id, json.encode({ type = "SET_BOUNTY", hold = hold, amount = amount }))
    reply(cid, ("Set %s bounty in %s → %d"):format(name, hold, amount))
end)

-- ── Admin: motd / clearflag ───────────────────────────────────────────────────

define("motd", "cmd.motd", function(cid, sess, args)
    local sub = (args[1] or ""):lower()
    if sub ~= "set" or not args[2] then
        reply(cid, "Current MOTD: " .. (config.motd or ""))
        reply(cid, "Usage: /motd set <message>")
        return
    end
    local msg = table.concat(args, " ", 2)
    config.motd = msg
    reply(cid, "MOTD updated: " .. msg)
    print(("[commands] MOTD set by %s: %s"):format(sess.name, msg))
end)

define("clearflag", "cmd.clearflag", function(cid, sess, args)
    local name = args[1]
    if not name then reply(cid, "Usage: /clearflag <name>") return end
    local char = store.getCharacterByName(name)
    if not char then reply(cid, "No character found: " .. name) return end
    store.clearAuditFlags(tonumber(char.id))
    reply(cid, "Cleared suspicious flags for " .. name)
    print(("[commands] audit flags cleared for %s by %s"):format(name, sess.name))
end)

-- ── Admin: audit ──────────────────────────────────────────────────────────────

define("audit", "cmd.audit", function(cid, sess, args)
    local name  = args[1]; local limit = tonumber(args[2]) or 10
    if not name then reply(cid, "Usage: /audit <name> [count]") return end
    local char = store.getCharacterByName(name)
    if not char then reply(cid, "No character found: " .. name) return end
    local entries = store.getAuditLog(tonumber(char.id), limit)
    if #entries == 0 then reply(cid, "No audit entries for " .. name) return end
    local lines = { "Audit log for " .. name .. ":" }
    for _, e in ipairs(entries) do
        lines[#lines + 1] = os.date("  %Y-%m-%d %H:%M", tonumber(e.timestamp)) ..
            "  [" .. e.event_type .. "] " .. (e.detail or "")
    end
    reply(cid, table.concat(lines, "\n"))
end)

-- ── Entry point ───────────────────────────────────────────────────────────────

function M.handle(char_id, text)
    local sess = session.getByCharId(char_id)
    if not sess then return end

    text = text:match("^%s*(.-)%s*$")
    if text:sub(1, 1) ~= "/" then
        reply(char_id, "Commands must start with /")
        return
    end

    local parts = {}
    for w in text:sub(2):gmatch("%S+") do parts[#parts + 1] = w end
    local cmd  = (parts[1] or ""):lower()
    local args = {}
    for i = 2, #parts do args[#args + 1] = parts[i] end

    local h = handlers[cmd]
    if not h then
        reply(char_id, "Unknown command: /" .. cmd .. "  (type /help)")
        return
    end
    if not rbac.can(sess.role, h.perm) then
        reply(char_id, "Permission denied")
        return
    end
    h.fn(char_id, sess, args)
end

return M
