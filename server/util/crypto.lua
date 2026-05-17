-- PBKDF2-SHA256 password hashing via Python3.
-- Uses temp files to pass credentials — no shell injection possible.
-- Format stored in DB:  pbkdf2sha256$<iters>$<salt_hex>$<hash_hex>

local M = {}

local ITERS = 100000  -- NIST-recommended minimum for PBKDF2-SHA256

-- 16 cryptographic random bytes → hex string
local function genSalt()
    local f = assert(io.open("/dev/urandom", "rb"), "cannot open /dev/urandom")
    local bytes = f:read(16)
    f:close()
    return (bytes:gsub(".", function(c) return ("%02x"):format(c:byte()) end))
end

-- 32 cryptographic random bytes → 64-char hex (session tokens)
function M.genToken()
    local f = assert(io.open("/dev/urandom", "rb"))
    local bytes = f:read(32)
    f:close()
    return (bytes:gsub(".", function(c) return ("%02x"):format(c:byte()) end))
end

-- Run PBKDF2-SHA256 via Python3. salt_hex is a hex string.
-- Returns the derived key as a hex string, or "" on error.
local function pbkdf2(password, salt_hex, iters)
    local ptmp  = os.tmpname()
    local pyscr = os.tmpname()

    do
        local f = assert(io.open(ptmp, "wb"))
        f:write(password)
        f:close()
    end
    do
        local f = assert(io.open(pyscr, "w"))
        f:write(string.format([[
import hashlib, binascii, sys
pw = open(sys.argv[1], "rb").read()
s  = bytes.fromhex(sys.argv[2])
dk = hashlib.pbkdf2_hmac("sha256", pw, s, %d)
print(binascii.hexlify(dk).decode())
]], iters))
        f:close()
    end

    local cmd  = string.format("python3 '%s' '%s' '%s' 2>/dev/null",
                               pyscr, ptmp, salt_hex)
    local pipe = io.popen(cmd)
    local hash = (pipe:read("*l") or ""):match("^%s*(.-)%s*$")
    pipe:close()
    os.remove(ptmp)
    os.remove(pyscr)
    return hash
end

-- Hash a plaintext password. Returns a storable string.
function M.hashPassword(password)
    local salt = genSalt()
    local hash = pbkdf2(password, salt, ITERS)
    assert(hash and #hash == 64, "crypto.hashPassword: pbkdf2 failed (python3 available?)")
    return string.format("pbkdf2sha256$%d$%s$%s", ITERS, salt, hash)
end

-- Verify a plaintext password against a stored hash string.
function M.verifyPassword(password, stored)
    if type(stored) ~= "string" then return false end
    local iters, salt, expected = stored:match("^pbkdf2sha256%$(%d+)%$(%x+)%$(%x+)$")
    if not iters then return false end
    local actual = pbkdf2(password, salt, tonumber(iters))
    return #actual > 0 and actual == expected
end

return M
