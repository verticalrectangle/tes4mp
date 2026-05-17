local M = {}

local ROLES = { player = 1, moderator = 2, admin = 3 }

local PERMISSIONS = {
    -- General
    ["cmd.help"]        = "player",
    ["cmd.players"]     = "player",
    ["cmd.who"]         = "player",
    ["cmd.tell"]        = "player",
    ["cmd.party"]       = "player",
    ["cmd.setpw"]       = "player",
    -- Moderation
    ["cmd.kick"]        = "moderator",
    ["cmd.setstage"]    = "moderator",
    ["cmd.announce"]    = "moderator",
    -- Admin
    ["cmd.ban"]         = "admin",
    ["cmd.unban"]       = "admin",
    ["cmd.setrank"]     = "admin",
    ["cmd.syncquests"]  = "admin",
    ["cmd.give"]        = "admin",
    ["cmd.take"]        = "admin",
    ["cmd.teleport"]    = "admin",
    ["cmd.tphere"]      = "admin",
    ["cmd.setlevel"]    = "admin",
    ["cmd.setskill"]    = "admin",
    ["cmd.setattr"]     = "admin",
    ["cmd.addgold"]     = "admin",
    ["cmd.setbounty"]   = "admin",
    ["cmd.audit"]       = "admin",
    ["cmd.clearflag"]   = "admin",
    ["cmd.motd"]        = "admin",
    ["cmd.reloadconfig"]= "admin",
}

function M.rankOf(role)    return ROLES[role] or 0 end
function M.isValid(role)   return ROLES[role] ~= nil end
function M.roles()         return { "player", "moderator", "admin" } end

function M.can(role, permission)
    local required = PERMISSIONS[permission]
    if not required then return false end
    return (ROLES[role] or 0) >= (ROLES[required] or 99)
end

return M
