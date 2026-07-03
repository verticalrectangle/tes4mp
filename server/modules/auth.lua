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

function M.handleHello(sock, info, pkt, config)
    local token = tostring(pkt.token or "")
    if #token < 16 then
        send(sock, { type = "KICK", reason = "bad token" })
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

    char = store.createCharByToken(token, name)
    if not char then
        send(sock, { type = "KICK", reason = "Failed to create character" })
        return
    end

    print(("[auth] HELLO → created char=%s"):format(name))
    finalize(sock, info, char, config, true)
end

return M
