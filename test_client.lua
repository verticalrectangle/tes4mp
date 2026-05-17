-- TES4MP test client
-- Usage:
--   lua test_client.lua register <host> <port> <user> <pass> <charname>
--   lua test_client.lua login    <host> <port> <user> <pass> [slot]
--   lua test_client.lua token    <host> <port> <token>

local socket = require("socket")
local json   = require("cjson")

local R    = "\27[0m"
local BOLD = "\27[1m"
local RED  = "\27[31m";  local GREEN  = "\27[32m"
local YELLOW = "\27[33m"; local CYAN  = "\27[36m"

local function log(color, label, msg)
    print(color .. BOLD .. string.format("[%-8s]", label) .. R .. " " .. msg)
end
local function sent(m) log(CYAN,   "SENT",    m) end
local function recv(m) log(GREEN,  "RECV",    m) end
local function info(m) log(YELLOW, "INFO",    m) end
local function fail(m) log(RED,    "ERROR",   m) end
local function ok(m)   log(GREEN,  "OK",      m) end

local mode     = arg[1] or "login"
local HOST     = arg[2] or "127.0.0.1"
local PORT     = tonumber(arg[3]) or 7777
local USERNAME = arg[4] or "testuser"
local PASSWORD = arg[5] or "testpass"
local CHARNAME = arg[6] or "TestHero"   -- register only
local SLOT     = tonumber(arg[6]) or 1  -- login: which slot to pick
local TOKEN    = arg[4] or ""           -- token mode

-- ── TCP plumbing ──────────────────────────────────────────────────────────────

info(("Connecting to %s:%d (mode=%s)..."):format(HOST, PORT, mode))
local sock = assert(socket.tcp())
sock:settimeout(5)
local ok_, cerr = sock:connect(HOST, PORT)
if not ok_ then fail("Connection failed: " .. tostring(cerr)); os.exit(1) end
sock:settimeout(0)
info("Connected.")

local recvBuf = ""

local function sendPkt(pkt)
    local line = json.encode(pkt)
    sent(line)
    sock:send(line .. "\n")
end

-- Read one line with up to `timeout` seconds total wait.
local function recvLine(timeout)
    local deadline = socket.gettime() + (timeout or 5)
    while socket.gettime() < deadline do
        local data, serr, partial = sock:receive(4096)
        local chunk = data or partial
        if chunk and #chunk > 0 then
            recvBuf = recvBuf .. chunk
        end
        local nl = recvBuf:find("\n")
        if nl then
            local line = recvBuf:sub(1, nl - 1)
            recvBuf = recvBuf:sub(nl + 1)
            return line
        end
        if serr == "closed" then return nil, "closed" end
        socket.sleep(0.02)
    end
    return nil, "timeout"
end

-- Drain all pending packets for `timeout` seconds, returning list of decoded tables.
local function drainPkts(timeout)
    local deadline = socket.gettime() + (timeout or 0.5)
    local pkts = {}
    while socket.gettime() < deadline do
        local data, serr, partial = sock:receive(4096)
        local chunk = data or partial
        if chunk and #chunk > 0 then
            recvBuf = recvBuf .. chunk
            deadline = socket.gettime() + 0.3
        end
        while true do
            local nl = recvBuf:find("\n")
            if not nl then break end
            local line = recvBuf:sub(1, nl - 1)
            recvBuf = recvBuf:sub(nl + 1)
            if #line > 0 then
                recv(line)
                local ok2, p = pcall(json.decode, line)
                if ok2 then pkts[#pkts + 1] = p end
                deadline = socket.gettime() + 0.3
            end
        end
        if serr == "closed" then break end
        socket.sleep(0.02)
    end
    return pkts
end

local function expectType(timeout, ...)
    local wanted = {...}
    local deadline = socket.gettime() + (timeout or 5)
    while socket.gettime() < deadline do
        local line, err = recvLine(0.1)
        if line then
            recv(line)
            local ok2, p = pcall(json.decode, line)
            if ok2 then
                for _, w in ipairs(wanted) do
                    if p.type == w then return p end
                end
                -- not what we wanted, keep going
            end
        end
    end
    fail("Timed out waiting for " .. table.concat(wanted, "/"))
    return nil
end

-- ── Auth flow ─────────────────────────────────────────────────────────────────

-- 1. Wait for SERVER_HELLO
local hello = expectType(5, "SERVER_HELLO")
if not hello then os.exit(1) end
ok(("SERVER_HELLO  name=%q motd=%q"):format(hello.name or "?", hello.motd or ""))

-- 2. Send auth packet according to mode
if mode == "register" then
    sendPkt({
        type     = "REGISTER",
        username = USERNAME,
        password = PASSWORD,
        charname = CHARNAME,
    })
elseif mode == "token" then
    sendPkt({ type = "TOKEN_AUTH", token = TOKEN })
else
    -- login (default)
    sendPkt({
        type     = "LOGIN",
        username = USERNAME,
        password = PASSWORD,
    })
end

-- 3. Wait for CHARACTER_LIST or AUTH_FAIL
local charlist = expectType(5, "CHARACTER_LIST", "AUTH_FAIL", "AUTH_OK")
if not charlist then os.exit(1) end

if charlist.type == "AUTH_FAIL" then
    fail("AUTH_FAIL: " .. (charlist.reason or "?"))
    os.exit(1)
end

-- Token auth or register may go straight to AUTH_OK+CHAR_LOAD;
-- LOGIN always gets CHARACTER_LIST first.
local session_token = nil

if charlist.type == "CHARACTER_LIST" then
    ok("CHARACTER_LIST received:")
    local chars = charlist.characters or {}
    for _, c in ipairs(chars) do
        local desc = c.empty and "[empty]"
            or ("%s Lv.%d"):format(c.name or "?", c.level or 1)
        ok(("  slot %d: %s"):format(c.slot, desc))
    end

    local pick = (mode == "register") and 1 or SLOT
    info(("Selecting slot %d..."):format(pick))
    sendPkt({ type = "CHAR_SELECT", slot = pick })

    -- Wait for AUTH_OK after slot selection
    local authok = expectType(5, "AUTH_OK", "AUTH_FAIL")
    if not authok then os.exit(1) end
    if authok.type == "AUTH_FAIL" then
        fail("AUTH_FAIL after CHAR_SELECT: " .. (authok.reason or "?"))
        os.exit(1)
    end
    session_token = authok.token
    ok(("AUTH_OK  role=%s token=%s"):format(authok.role or "?", authok.token or "?"))
else
    -- Already AUTH_OK (token auth path)
    session_token = charlist.token
    ok(("AUTH_OK  role=%s token=%s"):format(charlist.role or "?", charlist.token or "?"))
end

-- 5. Drain CHAR_LOAD + QUEST_SYNC + PLAYER_LIST that follow AUTH_OK
local init_pkts = drainPkts(2.0)
for _, p in ipairs(init_pkts) do
    if p.type == "CHAR_LOAD" then
        ok("CHAR_LOAD received — character data applied")
    elseif p.type == "QUEST_SYNC" then
        local qs = p.quests or {}
        ok(("QUEST_SYNC   %d quest(s)"):format(#qs))
    elseif p.type == "PLAYER_LIST" then
        local ps = p.players or {}
        ok(("PLAYER_LIST  %d online"):format(#ps))
        for _, pl in ipairs(ps) do
            ok(("             %s [%s]"):format(pl.name, pl.role))
        end
    end
end

-- ── In-game test suite ────────────────────────────────────────────────────────

local function test(label, fn)
    io.write(YELLOW .. BOLD .. "\n── " .. label .. " ──\n" .. R)
    fn()
    drainPkts(1.0)
end

test("/players", function()
    sendPkt({ type = "COMMAND", text = "/players" })
end)

test("/who", function()
    sendPkt({ type = "COMMAND", text = "/who" })
end)

test("/help", function()
    sendPkt({ type = "COMMAND", text = "/help" })
end)

test("QUEST_STAGE global (MQ01 -> 10)", function()
    sendPkt({ type = "QUEST_STAGE", questId = "MQ01", stage = 10 })
end)

test("QUEST_STAGE personal (FG01 -> 5)", function()
    sendPkt({ type = "QUEST_STAGE", questId = "FG01", stage = 5 })
end)

test("QUEST_STAGE same stage again (no-op)", function()
    sendPkt({ type = "QUEST_STAGE", questId = "MQ01", stage = 10 })
end)

test("QUEST_STAGE lower stage (must not go backwards)", function()
    sendPkt({ type = "QUEST_STAGE", questId = "MQ01", stage = 3 })
end)

test("/setstage MQ02 20 (admin command)", function()
    sendPkt({ type = "COMMAND", text = "/setstage MQ02 20" })
end)

test("/syncquests", function()
    sendPkt({ type = "COMMAND", text = "/syncquests" })
end)

-- Admin commands target by character name, not username.
-- After AUTH_OK + CHAR_LOAD we know our own character name from the CHAR_LOAD packet,
-- but for simplicity we use a known name. In register mode that's CHARNAME.
local admin_target = (mode == "register") and CHARNAME or "Test Hero"

test("/give <charname> gold001 100 (admin)", function()
    sendPkt({ type = "COMMAND", text = ("/give %s gold001 100"):format(admin_target) })
end)

test("/setlevel (admin)", function()
    sendPkt({ type = "COMMAND", text = ("/setlevel %s 5"):format(admin_target) })
end)

test("/setskill blade (admin)", function()
    sendPkt({ type = "COMMAND", text = ("/setskill %s blade 50"):format(admin_target) })
end)

test("/setattr strength (admin)", function()
    sendPkt({ type = "COMMAND", text = ("/setattr %s strength 60"):format(admin_target) })
end)

test("PING", function()
    sendPkt({ type = "PING" })
end)

test("Bad JSON (server must not crash)", function()
    sent("not json at all")
    sock:send("not json at all\n")
end)

test("Unknown command", function()
    sendPkt({ type = "COMMAND", text = "/doesnotexist" })
end)

print(YELLOW .. BOLD .. "\n── Test suite complete ──" .. R)
if session_token then
    info("Session token: " .. session_token)
    info("Re-auth: lua test_client.lua token " .. HOST .. " " .. PORT .. " " .. session_token)
end

print("\nInteractive mode:")
print("  /cmd <text>          → COMMAND packet")
print("  /quest <id> <stage>  → QUEST_STAGE packet")
print("  /ping                → PING")
print("  {raw json}           → sent verbatim")
print("  Ctrl+C               → quit\n")

while true do
    drainPkts(0)
    io.write(CYAN .. "> " .. R)
    io.flush()
    local line = io.read("*l")
    if not line then break end
    line = line:match("^%s*(.-)%s*$")
    if line == "" then
        -- no-op
    elseif line:sub(1, 5) == "/cmd " then
        sendPkt({ type = "COMMAND", text = line:sub(6) })
        drainPkts(1.0)
    elseif line:sub(1, 7) == "/quest " then
        local qid, stage = line:sub(8):match("(%S+)%s+(%d+)")
        if qid and stage then
            sendPkt({ type = "QUEST_STAGE", questId = qid, stage = tonumber(stage) })
        else
            info("Usage: /quest <questId> <stage>")
        end
        drainPkts(1.0)
    elseif line == "/ping" then
        sendPkt({ type = "PING" })
        drainPkts(1.0)
    elseif line:sub(1, 1) == "{" then
        sent(line)
        sock:send(line .. "\n")
        drainPkts(1.0)
    else
        info("Try: /cmd, /quest, /ping, or raw {json}")
    end
end
