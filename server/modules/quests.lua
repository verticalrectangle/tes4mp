local store   = require("server.db.store")
local session = require("server.modules.session")

local M = {}

local questConfig = nil

function M.init(cfg)
    questConfig = cfg
end

-- Quest editor IDs the client should watch and report stage changes for.
function M.getMonitored()
    return (questConfig and questConfig.monitored) or {}
end

local function matches(questId, list, prefixes)
    for _, qid in ipairs(list or {}) do
        if qid == questId then return true end
    end
    for _, prefix in ipairs(prefixes or {}) do
        if questId:sub(1, #prefix) == prefix then return true end
    end
    return false
end

-- WP14 scope rules (clients report ALL quests now):
--   ignored_prefixes/ignored_quests  → dropped (engine-noise controller quests)
--   global_quests/global_prefixes    → global
--   personal_quests/personal_prefixes→ party-scoped (guild lines)
--   everything else                  → config "default" ("global" here)
local function isGlobal(questId)
    if not questConfig then return false end
    if matches(questId, questConfig.global_quests, questConfig.global_prefixes) then
        return true
    end
    if matches(questId, questConfig.personal_quests, questConfig.personal_prefixes) then
        return false
    end
    return (questConfig.default or "global") ~= "personal"
end

local function isIgnored(questId)
    if not questConfig then return false end
    return matches(questId, questConfig.ignored_quests, questConfig.ignored_prefixes)
end

-- Burst guard: a corrupted client-side quest walk could spray hundreds of
-- QUEST_STAGE packets; the honest client caps itself at 8 per 5s poll.
local QS_WINDOW_S, QS_MAX = 10, 40

function M.handleQuestStage(char_id, questId, stage)
    local json    = require("cjson")
    local sender  = session.getByCharId(char_id)
    if not sender then return end

    stage = tonumber(stage)
    if not stage then return end

    -- Editor IDs are alnum/underscore/dash; anything else is not a quest ID
    -- (it would be echoed into peers' setstage console commands — reject).
    if not questId:match("^[%w_%-]+$") or #questId > 64 then return end
    if isIgnored(questId) then return end

    local now = os.time()
    if not sender.qs_win or now - sender.qs_win >= QS_WINDOW_S then
        sender.qs_win, sender.qs_n = now, 0
    end
    sender.qs_n = sender.qs_n + 1
    if sender.qs_n > QS_MAX then
        if sender.qs_n == QS_MAX + 1 then
            local store = require("server.db.store")
            store.logAudit(char_id, "QUEST_SPAM",
                (">%d QUEST_STAGE in %ds, dropping"):format(QS_MAX, QS_WINDOW_S))
        end
        return
    end

    if isGlobal(questId) then
        local current = store.getGlobalQuestStage(questId)
        if stage <= current then return end

        store.upsertGlobalQuest(questId, stage)
        local pkt = json.encode({
            type    = "QUEST_STAGE",
            questId = questId,
            stage   = stage,
            scope   = "global",
            src     = sender.name,
        })
        session.broadcast(pkt)
        print(("[quests] global %s → %d (by %s)"):format(questId, stage, sender.name))
    else
        local personal = store.getCharacterQuests(char_id)
        local current  = personal[questId] or 0
        if stage <= current then return end

        store.upsertCharacterQuest(char_id, questId, stage)
        local pkt = json.encode({
            type    = "QUEST_STAGE",
            questId = questId,
            stage   = stage,
            scope   = "personal",
            src     = sender.name,
        })
        -- Send to party members only (or just the sender if no party)
        session.broadcastParty(char_id, pkt)
        print(("[quests] personal %s → %d (by %s)"):format(questId, stage, sender.name))
    end
end

function M.forceQuestStage(questId, stage, actorName)
    local json = require("cjson")
    stage = tonumber(stage)
    store.upsertGlobalQuest(questId, stage)
    session.broadcast(json.encode({
        type    = "QUEST_STAGE",
        questId = questId,
        stage   = stage,
        scope   = "global",
        src     = actorName or "server",
        forced  = true,
    }))
    print(("[quests] forced %s → %d by %s"):format(questId, stage, actorName or "server"))
end

function M.syncToChar(char_id)
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
    session.sendTo(char_id, json.encode({ type = "QUEST_SYNC", quests = quests }))
end

function M.syncAll()
    for char_id in pairs(session.getAll()) do
        M.syncToChar(char_id)
    end
end

return M
