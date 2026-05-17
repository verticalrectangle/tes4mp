#pragma once
#include <string>
#include <vector>

struct Config {
    std::string host     = "127.0.0.1";
    int         port     = 7777;
    std::string charName;   // empty = use Windows username
};

Config LoadConfig();   // reads TES4MP.ini from OBSE Plugins dir
