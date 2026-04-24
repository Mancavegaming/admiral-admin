#include "ap_config.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace ap_cfg {

namespace {

Config       g_config;
std::mutex   g_mutex;
std::string  g_config_path;

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Very small JSON string+number extractor. Parses `"key"` and the following
// string or integer. Good enough for our 2-field config.
bool find_string_field(const std::string& j, const std::string& key, std::string& out)
{
    std::string needle = "\"" + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p;
    std::string val;
    while (p < j.size() && j[p] != '"') {
        if (j[p] == '\\' && p + 1 < j.size()) {
            char nx = j[p + 1];
            switch (nx) {
                case '"':  val += '"';  break;
                case '\\': val += '\\'; break;
                case 'n':  val += '\n'; break;
                case 'r':  val += '\r'; break;
                case 't':  val += '\t'; break;
                default:   val += nx;   break;
            }
            p += 2;
        } else {
            val += j[p++];
        }
    }
    out = std::move(val);
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
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) ++p;
    char* endp = nullptr;
    out = std::strtol(j.c_str() + p, &endp, 10);
    return endp != j.c_str() + p;
}

std::string read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

bool write_file(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

std::string generate_password(size_t length = 24)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789";
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<size_t> pick(0, sizeof(alphabet) - 2);
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) out.push_back(alphabet[pick(rng)]);
    return out;
}

std::string serialize(const Config& c)
{
    std::ostringstream o;
    o << "{\n";
    o << "  \"password\": \"" << json_escape(c.password) << "\",\n";
    o << "  \"http_port\": " << c.http_port << "\n";
    o << "}\n";
    return o.str();
}

} // namespace

std::string discover_game_dir()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path p(exe);
    // WindroseServer.exe is typically at <gamedir>\R5\Binaries\Win64\...
    // Walk up three levels.
    for (int i = 0; i < 4 && p.has_parent_path(); ++i) p = p.parent_path();
    return p.string();
}

Config& load(const std::string& game_dir)
{
    std::lock_guard<std::mutex> lk(g_mutex);

    g_config.game_dir = game_dir;
    std::filesystem::path gd(game_dir);
    g_config_path = (gd / "admiralspanel.json").string();
    g_config.data_dir = (gd / "admiralspanel_data").string();
    g_config.spool_dir = (gd / "admiralspanel_data" / "rcon").string();
    g_config.web_dir = (gd / "admiralspanel_data" / "web").string();

    std::error_code ec;
    std::filesystem::create_directories(g_config.data_dir, ec);
    std::filesystem::create_directories(g_config.spool_dir, ec);
    std::filesystem::create_directories(g_config.web_dir, ec);

    std::string text = read_file(g_config_path);
    if (text.empty()) {
        g_config.password  = generate_password();
        g_config.http_port = 8790;
        write_file(g_config_path, serialize(g_config));
    } else {
        std::string pw;
        long port = 8790;
        if (find_string_field(text, "password", pw)) g_config.password = pw;
        if (find_int_field(text, "http_port", port) && port > 0 && port < 65536) {
            g_config.http_port = static_cast<uint16_t>(port);
        }
        if (g_config.password.empty()) {
            g_config.password = generate_password();
            write_file(g_config_path, serialize(g_config));
        }
    }
    return g_config;
}

const Config& get()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_config;
}

bool save()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    return write_file(g_config_path, serialize(g_config));
}

} // namespace ap_cfg
