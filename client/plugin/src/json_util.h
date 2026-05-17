#pragma once
// Minimal JSON builder and field extractor for TES4MP packets.
// Handles only the specific packet shapes we send/receive — no general purpose parser.

#include <string>
#include <vector>
#include <sstream>
#include <cstring>

namespace json {

// ---- Builder ----

inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

inline std::string str(const std::string& k, const std::string& v) {
    return "\"" + k + "\":\"" + escape(v) + "\"";
}

inline std::string num(const std::string& k, int v) {
    return "\"" + k + "\":" + std::to_string(v);
}

inline std::string obj(std::initializer_list<std::string> fields) {
    std::string out = "{";
    bool first = true;
    for (auto& f : fields) {
        if (!first) out += ",";
        out += f;
        first = false;
    }
    out += "}";
    return out;
}

// ---- Extractor ----
// Extracts a field value from a flat JSON string like:
//   {"type":"QUEST_STAGE","questId":"MQ01","stage":10}
// Works on our specific packet shapes — not a general JSON parser.

inline std::string getStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '"')  { val += '"';  pos += 2; continue; }
            if (next == '\\') { val += '\\'; pos += 2; continue; }
            if (next == 'n')  { val += '\n'; pos += 2; continue; }
        }
        val += json[pos++];
    }
    return val;
}

inline bool getBool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return json.substr(pos, 4) == "true";
}

inline int getInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ')) ++pos;
    if (pos >= json.size()) return 0;
    bool neg = (json[pos] == '-');
    if (neg) ++pos;
    int val = 0;
    while (pos < json.size() && std::isdigit((unsigned char)json[pos]))
        val = val * 10 + (json[pos++] - '0');
    return neg ? -val : val;
}

// Parse the "quests" array from a QUEST_SYNC packet.
// Returns vector of {questId, stage} pairs.
struct QuestEntry { std::string questId; int stage; };

inline std::vector<QuestEntry> getQuestArray(const std::string& j) {
    std::vector<QuestEntry> out;
    auto arr = j.find("\"quests\":[");
    if (arr == std::string::npos) return out;
    arr += 9; // skip to '['
    auto end = j.find(']', arr);
    if (end == std::string::npos) return out;
    std::string slice = j.substr(arr, end - arr);
    // Walk through each {...} object in the array
    size_t i = 0;
    while (i < slice.size()) {
        auto ob = slice.find('{', i);
        auto cb = slice.find('}', ob == std::string::npos ? 0 : ob);
        if (ob == std::string::npos || cb == std::string::npos) break;
        std::string entry = slice.substr(ob, cb - ob + 1);
        QuestEntry qe;
        qe.questId = getStr(entry, "questId");
        qe.stage   = getInt(entry, "stage");
        if (!qe.questId.empty())
            out.push_back(qe);
        i = cb + 1;
    }
    return out;
}

} // namespace json
