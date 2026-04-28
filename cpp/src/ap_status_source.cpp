// Lightweight, thread-safe readers used by /api/status. Each reader has a
// small in-process cache so the handler is cheap even when the web panel
// polls every couple of seconds.

#include <Unreal/AActor.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

using namespace RC;
using namespace RC::Unreal;

#include "ap_config.hpp"

namespace ap_status {

namespace {

std::chrono::steady_clock::time_point g_boot_time = std::chrono::steady_clock::now();

// Player-controller count. Cache for 1 second to amortize the UObject scan
// across the 2-second panel-poll interval.
std::mutex                                      g_pc_mu;
int                                             g_pc_last  = 0;
std::chrono::steady_clock::time_point           g_pc_read_at{};

int raw_count_player_controllers()
{
    int n = 0;
    UObjectGlobals::ForEachUObject([&](UObject* obj, int32, int32) {
        if (!obj) return LoopAction::Continue;
        auto* cls = obj->GetClassPrivate();
        if (!cls) return LoopAction::Continue;
        auto name = cls->GetName();
        if (std::wstring_view(name).find(STR("PlayerController")) == std::wstring_view::npos)
            return LoopAction::Continue;
        auto full = obj->GetFullName();
        if (full.find(STR("Default__")) != std::wstring::npos) return LoopAction::Continue;
        // PlayerState must be non-null (rules out in-transition / partially-
        // initialised controllers that show up during login).
        auto* ps_prop = obj->GetPropertyByNameInChain(STR("PlayerState"));
        if (!ps_prop) return LoopAction::Continue;
        auto** ps_ptr = ps_prop->ContainerPtrToValuePtr<UObject*>(obj);
        if (!ps_ptr || !*ps_ptr) return LoopAction::Continue;
        ++n;
        return LoopAction::Continue;
    });
    return n;
}

// Tiny JSON field extractor — same pattern as ap_config.cpp but local copy
// to keep this TU independent.
bool find_string_field(const std::string& j, const std::string& key, std::string& out)
{
    std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p;
    out.clear();
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) { out += j[p + 1]; p += 2; }
        else                                  { out += j[p++]; }
    }
    return true;
}

bool find_int_field(const std::string& j, const std::string& key, long& out)
{
    std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) ++p;
    char* endp = nullptr;
    out = std::strtol(j.c_str() + p, &endp, 10);
    return endp != j.c_str() + p;
}

std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

} // namespace

int count_player_controllers()
{
    std::lock_guard<std::mutex> lk(g_pc_mu);
    auto now = std::chrono::steady_clock::now();
    if (now - g_pc_read_at > std::chrono::milliseconds(1000)) {
        g_pc_last    = raw_count_player_controllers();
        g_pc_read_at = now;
    }
    return g_pc_last;
}

namespace {

// Cache the parsed ServerDescription.json briefly so each /api/status hit
// doesn't reread + reparse the file. 5s is plenty — Windrose only rewrites
// it on config changes.
std::mutex                              g_sd_mu;
std::chrono::steady_clock::time_point   g_sd_read_at{};
std::string                             g_sd_text;

const std::string& server_description_text()
{
    std::lock_guard<std::mutex> lk(g_sd_mu);
    auto now = std::chrono::steady_clock::now();
    if (now - g_sd_read_at < std::chrono::seconds(5) && !g_sd_text.empty()) {
        return g_sd_text;
    }
    const auto& cfg = ap_cfg::get();
    std::filesystem::path p(cfg.game_dir);
    p /= "R5/ServerDescription.json";
    g_sd_text    = slurp(p.string());
    g_sd_read_at = now;
    return g_sd_text;
}

} // namespace

// Server name + invite code + max players + deployment id all come from
// R5/ServerDescription.json, written by Windrose itself. The keys we want
// live under "ServerDescription_Persistent" but find_string_field only
// needs key uniqueness which holds for these names.
std::string read_config_server_name()
{
    const std::string& sd = server_description_text();
    if (!sd.empty()) {
        std::string v;
        if (find_string_field(sd, "ServerName", v) && !v.empty()) return v;
    }
    // Optional fallback for users who still use WindrosePlus.
    const auto& cfg = ap_cfg::get();
    std::string wpt = slurp(cfg.game_dir + "\\windrose_plus.json");
    if (!wpt.empty()) {
        std::string v;
        if (find_string_field(wpt, "server_name", v) && !v.empty()) return v;
    }
    return "Windrose Dedicated Server";
}

// Game version: prefer the DeploymentId from ServerDescription.json (e.g.
// "0.10.0.3.104-256f9653" — the actual Windrose patch version) over the
// exe's PE FileVersion (which is the underlying UE engine version).
std::string read_exe_file_version()
{
    const std::string& sd = server_description_text();
    if (!sd.empty()) {
        std::string v;
        if (find_string_field(sd, "DeploymentId", v) && !v.empty()) return v;
    }
    // Fallback: PE file version from the shipping exe.
    const auto& cfg = ap_cfg::get();
    std::string exe = cfg.game_dir + "\\R5\\Binaries\\Win64\\WindroseServer-Win64-Shipping.exe";
    DWORD handle = 0;
    DWORD size   = GetFileVersionInfoSizeA(exe.c_str(), &handle);
    if (size == 0) return "unknown";
    std::string buf(size, '\0');
    if (!GetFileVersionInfoA(exe.c_str(), 0, size, buf.data())) return "unknown";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(buf.data(), "\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !ffi) return "unknown";
    char out[64];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u",
                  HIWORD(ffi->dwFileVersionMS),
                  LOWORD(ffi->dwFileVersionMS),
                  HIWORD(ffi->dwFileVersionLS),
                  LOWORD(ffi->dwFileVersionLS));
    return out;
}

std::string read_invite_code()
{
    const std::string& sd = server_description_text();
    if (sd.empty()) return {};
    std::string v;
    if (find_string_field(sd, "InviteCode", v)) return v;
    return {};
}

int read_max_players()
{
    const std::string& sd = server_description_text();
    if (!sd.empty()) {
        long n = 0;
        if (find_int_field(sd, "MaxPlayerCount", n) && n > 0) return static_cast<int>(n);
    }
    return 8;
}

uint64_t process_uptime_seconds()
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - g_boot_time).count();
}

} // namespace ap_status
