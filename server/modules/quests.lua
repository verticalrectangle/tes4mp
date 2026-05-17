local store   = require("server.db.store")
local session = require("server.modules.session")

local M = {}

local questConfig = nil

function M.init(cfg)
    questConfig = cfg
end

local function isGlobal(questId)
    if not questConfig then return false end
    for _, qid in ipairs(questConfig.global_quests or {}) do
        if qid == questId then return true end
    end
    for _, prefix in ipairs(questConfig.global_prefixes or {}) do
        if questId:sub(1, #prefix) == prefix then return true end
    end
    return false
end

function M.handleQuestStage(char_id, questId, stage)
    local json    = require("cjson")
    local sender  = session.getByCharId(char_id)
    if not sender then return end

    stage = tonumber(stage)
    if not stage then return end

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
