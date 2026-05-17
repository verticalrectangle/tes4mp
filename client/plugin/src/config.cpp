#include "config.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

// Reads Key=Value from a section header [Section] in an INI file.
// Simple parser: no multiline, no comments mid-line.
static std::string IniGet(const std::string& path,
                           const std::string& section,
                           const std::string& key,
                           const std::string& def = "") {
    std::ifstream f(path);
    if (!f.good()) return def;

    std::string curSection, line;
    while (std::getline(f, line)) {
        // trim
        auto s = line.find_first_not_of(" \t\r\n");
        if (s == std::string::npos || line[s] == ';') continue;
        line = line.substr(s);

        if (line.front() == '[') {
            auto e = line.find(']');
            curSection = (e != std::string::npos) ? line.substr(1, e - 1) : "";
        } else if (curSection == section) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto k = line.substr(0, eq);
            // trim key
            auto ke = k.find_last_not_of(" \t");
            if (ke != std::string::npos) k = k.substr(0, ke + 1);
            if (k == key) {
                auto v = line.substr(eq + 1);
                auto vs = v.find_first_not_of(" \t");
                auto ve = v.find_last_not_of(" \t\r\n");
                if (vs == std::string::npos) return def;
                return v.substr(vs, ve - vs + 1);
            }
        }
    }
    return def;
}

Config LoadConfig() {
    // INI lives alongside TES4MP.dll: Data\OBSE\Plugins\TES4MP.ini
    char oblivionDir[MAX_PATH] = {};
    // GetModuleFileName for our own DLL gives us our path
    HMODULE hmod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadConfig, &hmod);
    GetModuleFileNameA(hmod, oblivionDir, MAX_PATH);

    // Strip filename to get directory
    std::string iniPath = oblivionDir;
    auto sep = iniPath.find_last_of("\\/");
    if (sep != std::string::npos) iniPath = iniPath.substr(0, sep);
    iniPath += "\\TES4MP.ini";

    Config cfg;
    cfg.host     = IniGet(iniPath, "Server", "Host", "127.0.0.1");
    cfg.port     = std::stoi(IniGet(iniPath, "Server", "Port", "7777"));
    cfg.charName = IniGet(iniPath, "Player", "CharName", "");

    if (cfg.charName.empty()) {
        char username[256] = {};
        DWORD len = sizeof(username);
        GetUserNameA(username, &len);
        cfg.charName = username;
    }

    return cfg;
}
