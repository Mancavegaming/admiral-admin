// AdmiralsPanel standalone config — lightweight JSON read/write without a
// heavy dep. Loads admiralspanel.json from the game directory; creates with
// sensible defaults if missing.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace ap_cfg {

struct Config
{
    std::string password;         // dashboard login
    uint16_t    http_port = 8790; // HTTP server port
    std::string game_dir;         // root dir with admiralspanel.json
    std::string data_dir;         // admiralspanel_data/ under game_dir
    std::string web_dir;          // where we serve static files from
    std::string spool_dir;        // command-spool subdir under data_dir
};

// Locate the game directory (WindroseServer.exe's root). Uses GetModuleFileName
// on the server exe and walks up a few directories.
std::string discover_game_dir();

// Load config from <game_dir>/admiralspanel.json. Creates a default file if
// missing (generating a random password on first run). Returns reference to
// the process-wide singleton.
Config& load(const std::string& game_dir);

// Thread-safe accessor.
const Config& get();

// Rewrite the config file after an in-process change (e.g. password rotate).
bool save();

} // namespace ap_cfg
