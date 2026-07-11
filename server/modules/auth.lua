-- auth.lua — Token-only auth. No passwords, no accounts.
-- Client sends HELLO with its persistent machine token and the in-game player name.
-- Known token → CHAR_LOAD immediately.
-- Unknown token → character created from the supplied name → CHAR_LOAD.

local store   = require("server.db.store")
local session = require("server.modules.session")

local M = {}

local function send(sock, pkt)
    local json = require("cjson")
    sock:send(json.encode(pkt) .. "\n")
end

local function finalize(sock, info, char, config, is_new)
    store.updateCharacterSeen(tonumber(char.id))
    info.state   = "in_game"
    info.char_id = tonumber(char.id)
    session.addSession(sock, info, char, config, is_new)
end

-- Bump whenever the client/server protocol changes incompatibly. The July
-- 2026 full-sync round proved version skew is undebuggable in the field
-- (old follower code disabling new clients' ghosts, replica spawns failing
-- silently on one side) — mismatched clients get a clear KICK instead.
M.PROTOCOL_VERSION = 2

-- Stream the current client DLL: one DLL_UPDATE json line with the byte
-- count, then the raw file. LAN-sized (a few MB); sent blocking, one-time
-- per stale client. Returns false if the file isn't available.
function M.streamDllUpdate(sock, config)
    local path = (config and config.client_dll) or "dist/TES4MP.dll"
    local f = io.open(path, "rb")
    if not f then
        print(("[auth] client_dll not found at %s — cannot auto-update"):format(path))
        return false
    end
    local data = f:read("*a")
    f:close()
    if not data or #data < 64 * 1024 then return false end

    local json = require("cjson")
    sock:send(json.encode({ type = "DLL_UPDATE", size = #data }) .. "\n")
    sock:send(data)
    return true
end

function M.handleHello(sock, info, pkt, config)
    local token = tostring(pkt.token or "")
    if #token < 16 then
        send(sock, { type = "KICK", reason = "bad token" })
        return
    end

    local ver = tonumber(pkt.ver) or 0
    if ver ~= M.PROTOCOL_VERSION then
        -- Protocol 2+ clients know the auto-update stream: send the current
        -- DLL (raw bytes after a DLL_UPDATE size header); the client swaps
        -- it on disk and the player just restarts Oblivion. Older clients
        -- would choke on the binary — they get manual instructions.
        if ver >= 2 and M.streamDllUpdate(sock, config) then
            send(sock, { type = "KICK",
                reason = "TES4MP updated automatically - restart Oblivion to finish." })
            print(("[auth] streamed DLL update to %s (client protocol %d → %d)")
                :format(info.addr or "?", ver, M.PROTOCOL_VERSION))
        else
            send(sock, { type = "KICK",
                reason = ("TES4MP.dll is out of date (protocol %d, server needs %d). "
                       .. "Update Data\\OBSE\\Plugins\\TES4MP.dll from the server's "
                       .. "dist folder and reconnect."):format(ver, M.PROTOCOL_VERSION) })
            print(("[auth] rejected client protocol %d from %s (need %d)")
                :format(ver, info.addr or "?", M.PROTOCOL_VERSION))
        end
        return
    end
    -- Token prefix in the log: distinguishes "two machines" from "copied install"
    print(("[auth] HELLO from %s token=%s... name=%s")
        :format(info.addr or "?", token:sub(1, 6), tostring(pkt.name or "")))

    local char = store.getCharByToken(token)
    if char and session.getByCharId(tonumber(char.id)) then
        -- Same token, second connection: almost always a copied install.
        -- Reject loudly instead of letting two players fight over one session.
        send(sock, { type = "KICK",
            reason = "This character is already online. Each player needs their own "
                  .. "token - if you copied an install, delete "
                  .. "AppData\\Roaming\\TES4MP\\token.txt and reconnect." })
        print(("[auth] rejected duplicate login for char=%s"):format(char.name))
        return
    end
    if char then
        local name = tostring(pkt.name or ""):match("^%s*(.-)%s*$")
        if #name >= 2 and #name <= 24 and name ~= char.name
                and not store.getCharacterByName(name) then  -- names are UNIQUE
            store.updateCharacterName(tonumber(char.id), name)
            char.name = name
            print(("[auth] HELLO → renamed char to %s"):format(name))
        else
            print(("[auth] HELLO → returning char=%s"):format(char.name))
        end
        finalize(sock, info, char, config, false)
        return
    end

    -- New player — use the name they set in Oblivion's character creation screen.
    local name = tostring(pkt.name or ""):match("^%s*(.-)%s*$")
    if #name < 2 then name = "Adventurer" end
    if #name > 24 then name = name:sub(1, 24) end

    -- Names are UNIQUE. Two clients can legitimately collide (e.g. both
    -- connect mid-chargen with the engine's default name) — suffix with the
    -- token prefix instead of blowing up the INSERT. CHAR_SAVE renames later.
    if store.getCharacterByName(name) then
        name = name:sub(1, 24 - 5) .. "-" .. token:sub(1, 4)
        print(("[auth] name taken, using %s"):format(name))
    end

    char = store.createCharByToken(token, name)
    if not char then
        send(sock, { type = "KICK", reason = "Failed to create character" })
        return
    end

    print(("[auth] HELLO → created char=%s"):format(name))
    finalize(sock, info, char, config, true)
end

return M
