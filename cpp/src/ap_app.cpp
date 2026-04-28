#include "ap_app.hpp"
#include "ap_config.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

// Exposed by ap_status_source.cpp — cheap, safe, off-spool gatherers that
// read values directly from the running game state. Lives in the global
// ::ap_status namespace (not nested inside ap_app).
namespace ap_status {
    int         count_player_controllers();
    std::string read_config_server_name();
    std::string read_exe_file_version();
    std::string read_invite_code();
    int         read_max_players();
    uint64_t    process_uptime_seconds();
}

namespace ap_app {

namespace {

constexpr std::chrono::minutes kSessionLifetime{60 * 24 * 30}; // 30 days

std::string gen_hex_token(size_t bytes = 16)
{
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64    rng{rd()};
    std::uniform_int_distribution<uint32_t> dist(0, 255);
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        uint32_t b = dist(rng);
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

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

std::string iso8601_ms_now()
{
    using namespace std::chrono;
    auto t  = system_clock::now();
    auto ms = duration_cast<milliseconds>(t.time_since_epoch()).count() % 1000;
    std::time_t tt = system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms);
    return buf;
}

const char* kLoginHtml = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<title>AdmiralsPanel — Login</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { margin:0; font:14px/1.4 system-ui, -apple-system, Segoe UI, sans-serif;
         background:#1a1510; color:#f1e7d4; display:flex; align-items:center;
         justify-content:center; min-height:100vh; }
  .card { background:#2a2118; padding:28px 32px; border:1px solid #4a3826;
          border-radius:6px; width:320px; box-shadow:0 4px 20px rgba(0,0,0,.4); }
  h1 { margin:0 0 4px; font-size:18px; color:#f4d580; }
  p { margin:4px 0 18px; color:#b6a58a; font-size:13px; }
  label { display:block; font-size:12px; color:#c6b192; margin-bottom:4px; }
  input[type=password] { width:100%; padding:8px 10px; background:#1a1510;
                         border:1px solid #5c4628; color:#f1e7d4; border-radius:4px;
                         font:14px system-ui; box-sizing:border-box; }
  button { margin-top:14px; width:100%; padding:10px; background:#8a6a2c;
           color:#fff; border:0; border-radius:4px; font:600 14px system-ui;
           cursor:pointer; }
  button:hover { background:#a27d33; }
  .err { color:#e47; font-size:13px; margin-top:10px; display:__ERRDISP__; }
</style>
</head><body>
  <form class="card" method="POST" action="/login">
    <h1>AdmiralsPanel</h1>
    <p>Enter password to continue.</p>
    <label for="pw">Password</label>
    <input id="pw" name="password" type="password" autofocus required autocomplete="current-password">
    <button type="submit">Sign in</button>
    <div class="err">Invalid password.</div>
  </form>
</body></html>
)HTML";

// Extract the raw `"multipliers": {...}` JSON object substring from
// admiralspanel.json on each /api/status hit. Re-reading the file each call
// is cheap (<1KB) and avoids stale cache vs the Lua mod's writes.
// Returns "{}" on any failure or absence.
std::string read_multipliers_json(const std::string& cfg_path)
{
    std::ifstream f(cfg_path, std::ios::binary);
    if (!f) return "{}";
    std::ostringstream o;
    o << f.rdbuf();
    std::string j = o.str();

    size_t k = j.find("\"multipliers\"");
    if (k == std::string::npos) return "{}";
    size_t colon = j.find(':', k);
    if (colon == std::string::npos) return "{}";
    size_t open = j.find('{', colon);
    if (open == std::string::npos) return "{}";

    int depth = 0;
    for (size_t p = open; p < j.size(); ++p) {
        if (j[p] == '{') ++depth;
        else if (j[p] == '}') {
            --depth;
            if (depth == 0) return j.substr(open, p - open + 1);
        }
    }
    return "{}";
}

const char* kIndexFallback = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<title>AdmiralsPanel</title>
<style>body{font:14px/1.5 system-ui;margin:40px;max-width:640px;color:#222}</style>
</head><body>
<h1>AdmiralsPanel — signed in</h1>
<p>The web panel files are not yet installed in this deployment. The HTTP server is running and you are authenticated.</p>
<p>Expected location: <code>admiralspanel_data/web/index.html</code>. Run <code>install.ps1</code> from the repo to deploy them.</p>
<p><a href="/logout">Log out</a></p>
</body></html>
)HTML";

} // namespace

App::App() = default;

App::~App() { stop(); }

bool App::start()
{
    const auto& cfg = ap_cfg::get();

    // Auth-required routes first.
    m_http.route("/api/health", [this](const ap_http::Request& r) {
        return handle_health(r);
    });
    m_http.route("/api/status", [this](const ap_http::Request& r) {
        return handle_status(r);
    });
    m_http.route("/api/rcon", [this](const ap_http::Request& r) {
        return handle_rcon(r);
    });

    m_http.route("/login", [this](const ap_http::Request& r) {
        if (r.method == "POST") return handle_login_post(r);
        return handle_login_get(r);
    });
    m_http.route("/logout", [this](const ap_http::Request& r) {
        return handle_logout(r);
    });
    m_http.route("/healthcheck", [](const ap_http::Request&) {
        return ap_http::json_response(200,
            R"({"status":"ok","app":"AdmiralsPanel","version":"0.7.0"})");
    });

    // Root + static files.
    m_http.route("/", [this](const ap_http::Request& r) { return handle_index(r); });
    m_http.static_root(cfg.web_dir);

    return m_http.start(cfg.http_port);
}

void App::stop()
{
    m_http.stop();
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

std::string App::new_session_token()
{
    std::string tok = gen_hex_token();
    std::lock_guard<std::mutex> lk(m_sess_mu);
    m_sessions[tok] = std::chrono::steady_clock::now();
    return tok;
}

std::string App::extract_cookie(const ap_http::Request& r, const std::string& name)
{
    auto it = r.headers.find("cookie");
    if (it == r.headers.end()) return {};
    const std::string& c = it->second;
    // Cookies are "a=b; c=d; ..."
    size_t i = 0;
    while (i < c.size()) {
        size_t semi = c.find(';', i);
        std::string pair = c.substr(i, (semi == std::string::npos) ? std::string::npos : semi - i);
        // Trim leading spaces
        size_t p = 0;
        while (p < pair.size() && (pair[p] == ' ' || pair[p] == '\t')) ++p;
        pair = pair.substr(p);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name) {
            return pair.substr(eq + 1);
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return {};
}

bool App::is_authenticated(const ap_http::Request& r) const
{
    // Localhost-trust: requests from 127.0.0.1 / ::1 bypass auth. The HTTP
    // server binds all interfaces, but external access is gated by the
    // host + cloud firewalls. On the machine itself, we assume the admin
    // and don't force login. Remove this block to revert to strict auth.
    if (r.client_ip == "127.0.0.1" || r.client_ip == "::1") return true;

    auto cookie = extract_cookie(r, "ap_session");
    if (cookie.empty()) return false;
    std::lock_guard<std::mutex> lk(m_sess_mu);
    auto it = m_sessions.find(cookie);
    if (it == m_sessions.end()) return false;
    auto age = std::chrono::steady_clock::now() - it->second;
    return age < kSessionLifetime;
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

ap_http::Response App::handle_login_get(const ap_http::Request&)
{
    std::string html(kLoginHtml);
    // Hide the error banner on GET.
    size_t p = html.find("__ERRDISP__");
    if (p != std::string::npos) html.replace(p, 11, "none");
    return ap_http::html_response(200, html);
}

ap_http::Response App::handle_login_post(const ap_http::Request& r)
{
    auto form = ap_http::parse_form_urlencoded(r.body);
    std::string pw  = form.count("password") ? form["password"] : std::string{};
    const auto& cfg = ap_cfg::get();

    if (pw.empty() || cfg.password.empty() || pw != cfg.password) {
        std::string html(kLoginHtml);
        size_t p = html.find("__ERRDISP__");
        if (p != std::string::npos) html.replace(p, 11, "block");
        return ap_http::html_response(200, html);
    }

    std::string tok = new_session_token();
    auto resp = ap_http::redirect_response("/");
    resp.extra_headers.push_back({
        "Set-Cookie",
        "ap_session=" + tok + "; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax"
    });
    return resp;
}

ap_http::Response App::handle_logout(const ap_http::Request& r)
{
    auto cookie = extract_cookie(r, "ap_session");
    if (!cookie.empty()) {
        std::lock_guard<std::mutex> lk(m_sess_mu);
        m_sessions.erase(cookie);
    }
    auto resp = ap_http::redirect_response("/login");
    resp.extra_headers.push_back({
        "Set-Cookie",
        "ap_session=deleted; Path=/; Max-Age=0; HttpOnly; SameSite=Lax"
    });
    return resp;
}

ap_http::Response App::handle_index(const ap_http::Request& r)
{
    if (!is_authenticated(r)) return ap_http::redirect_response("/login");

    // Prefer the installed index.html, fall back to a placeholder page.
    const auto& cfg = ap_cfg::get();
    std::filesystem::path idx = std::filesystem::path(cfg.web_dir) / "index.html";
    std::error_code ec;
    if (std::filesystem::exists(idx, ec)) {
        std::ifstream f(idx, std::ios::binary);
        std::ostringstream o;
        o << f.rdbuf();
        return ap_http::html_response(200, o.str());
    }
    return ap_http::html_response(200, kIndexFallback);
}

ap_http::Response App::handle_static(const ap_http::Request&)
{
    // Handled via HttpServer's static_root.
    return ap_http::text_response(404, "unreachable");
}

ap_http::Response App::handle_health(const ap_http::Request& r)
{
    if (!is_authenticated(r)) return ap_http::json_response(401, R"({"error":"auth"})");
    return ap_http::json_response(200,
        R"({"status":"ok","version":"0.7.0","ts":")" + iso8601_ms_now() + "\"}");
}

ap_http::Response App::handle_status(const ap_http::Request& r)
{
    if (!is_authenticated(r)) return ap_http::json_response(401, R"({"error":"auth"})");
    const auto& cfg = ap_cfg::get();

    int      players      = ap_status::count_player_controllers();
    int      max_players  = ap_status::read_max_players();
    std::string server_name = ap_status::read_config_server_name();
    std::string game_ver    = ap_status::read_exe_file_version();
    std::string invite      = ap_status::read_invite_code();
    uint64_t uptime         = ap_status::process_uptime_seconds();

    std::filesystem::path cfg_path =
        std::filesystem::path(cfg.game_dir) / "admiralspanel.json";
    std::string mults = read_multipliers_json(cfg_path.string());

    std::ostringstream o;
    o << "{";
    o << "\"server\":{";
    o <<   "\"name\":\""            << json_escape(server_name) << "\",";
    o <<   "\"version\":\""         << json_escape(game_ver)    << "\",";
    o <<   "\"admiralspanel\":\"0.7.0\",";
    o <<   "\"windrose_plus\":null,"; // kept for panel backwards-compat
    o <<   "\"invite_code\":"       << (invite.empty() ? std::string("null") :
                                         ("\"" + json_escape(invite) + "\"")) << ",";
    o <<   "\"player_count\":" << players << ",";
    o <<   "\"max_players\":"  << max_players << ",";
    o <<   "\"uptime_seconds\":" << uptime;
    o << "},";
    o << "\"multipliers\":" << mults << ",";
    o << "\"meta\":{";
    o <<   "\"http_port\":" << cfg.http_port << ",";
    o <<   "\"game_dir\":\""  << json_escape(cfg.game_dir)  << "\",";
    o <<   "\"data_dir\":\""  << json_escape(cfg.data_dir)  << "\",";
    o <<   "\"web_dir\":\""   << json_escape(cfg.web_dir)   << "\",";
    o <<   "\"spool_dir\":\"" << json_escape(cfg.spool_dir) << "\"";
    o << "}";
    o << "}";
    return ap_http::json_response(200, o.str());
}

// ---------------------------------------------------------------------------
// RCON spool bridge
// ---------------------------------------------------------------------------

ap_http::Response App::handle_rcon(const ap_http::Request& r)
{
    if (!is_authenticated(r)) return ap_http::json_response(401, R"({"error":"auth"})");
    if (r.method != "POST")   return ap_http::json_response(405, R"({"error":"POST required"})");

    // Hand-roll JSON parsing of {"command":"...", "args":[...]}
    std::string cmd;
    std::vector<std::string> args;

    auto extract_string = [&](const std::string& key, std::string& out) -> bool {
        std::string needle = "\"" + key + "\"";
        size_t p = r.body.find(needle);
        if (p == std::string::npos) return false;
        p = r.body.find(':', p + needle.size());
        if (p == std::string::npos) return false;
        ++p;
        while (p < r.body.size() && std::isspace(static_cast<unsigned char>(r.body[p]))) ++p;
        if (p >= r.body.size() || r.body[p] != '"') return false;
        ++p;
        out.clear();
        while (p < r.body.size() && r.body[p] != '"') {
            if (r.body[p] == '\\' && p + 1 < r.body.size()) {
                char nx = r.body[p + 1];
                switch (nx) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    default:   out += nx;   break;
                }
                p += 2;
            } else {
                out += r.body[p++];
            }
        }
        return true;
    };

    if (!extract_string("command", cmd) || cmd.empty()) {
        return ap_http::json_response(400, R"({"error":"missing command"})");
    }

    // Parse args array — naive: find `"args"` then take strings up to ]
    {
        size_t p = r.body.find("\"args\"");
        if (p != std::string::npos) {
            p = r.body.find('[', p);
            if (p != std::string::npos) {
                size_t q = r.body.find(']', p);
                if (q != std::string::npos) {
                    std::string arr = r.body.substr(p + 1, q - p - 1);
                    size_t i = 0;
                    while (i < arr.size()) {
                        while (i < arr.size() && arr[i] != '"') ++i;
                        if (i >= arr.size()) break;
                        ++i;
                        std::string s;
                        while (i < arr.size() && arr[i] != '"') {
                            if (arr[i] == '\\' && i + 1 < arr.size()) {
                                s += arr[i + 1];
                                i += 2;
                            } else {
                                s += arr[i++];
                            }
                        }
                        if (i < arr.size()) ++i;
                        args.push_back(std::move(s));
                    }
                }
            }
        }
    }

    std::string result = spool_submit(cmd, args, 25000);
    return ap_http::json_response(200, result);
}

std::string App::spool_submit(const std::string& command,
                              const std::vector<std::string>& args,
                              int timeout_ms)
{
    const auto& cfg = ap_cfg::get();
    std::filesystem::path spool(cfg.spool_dir);

    // Unique request ID.
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string id = "ap_" + std::to_string(now_ms) + "_" + gen_hex_token(4);

    // Write cmd file.
    std::filesystem::path cmd_path = spool / ("cmd_" + id + ".json");
    std::filesystem::path res_path = spool / ("res_" + id + ".json");
    std::ostringstream body;
    body << "{";
    body << "\"id\":\""      << id << "\",";
    body << "\"command\":\"" << json_escape(command) << "\",";
    body << "\"args\":[";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) body << ",";
        body << "\"" << json_escape(args[i]) << "\"";
    }
    body << "],";
    body << "\"ts\":" << now_ms;
    body << "}";
    {
        std::ofstream f(cmd_path, std::ios::binary);
        f << body.str();
    }

    // Append to the pending-index file (Lua reads this).
    std::filesystem::path idx = spool / "pending_commands.txt";
    {
        std::ofstream f(idx, std::ios::app | std::ios::binary);
        f << "cmd_" << id << ".json\r\n";
    }

    // Poll for response.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (std::filesystem::exists(res_path)) {
            std::ifstream f(res_path, std::ios::binary);
            std::ostringstream o;
            o << f.rdbuf();
            std::error_code ec;
            std::filesystem::remove(res_path, ec);
            return o.str();
        }
    }

    // Timed out — clean up the cmd file, return error.
    std::error_code ec;
    std::filesystem::remove(cmd_path, ec);
    std::ostringstream o;
    o << "{\"id\":\"" << id << "\",\"status\":\"error\",\"message\":\"Command timed out ("
      << (timeout_ms / 1000) << "s)\"}";
    return o.str();
}

} // namespace ap_app
