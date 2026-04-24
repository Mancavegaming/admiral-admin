// Wires the config + session + RCON-spool bridge into our HTTP server.
// dllmain constructs one of these at mod load and destroys it on unload.
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "http_server.hpp"

namespace ap_app {

class App
{
public:
    App();
    ~App();

    bool start();
    void stop();

private:
    // Routes
    ap_http::Response handle_login_get(const ap_http::Request&);
    ap_http::Response handle_login_post(const ap_http::Request&);
    ap_http::Response handle_logout(const ap_http::Request&);
    ap_http::Response handle_index(const ap_http::Request&);
    ap_http::Response handle_static(const ap_http::Request&);
    ap_http::Response handle_rcon(const ap_http::Request&);
    ap_http::Response handle_health(const ap_http::Request&);
    ap_http::Response handle_status(const ap_http::Request&);

    // Session helpers
    bool is_authenticated(const ap_http::Request&) const;
    std::string new_session_token();
    static std::string extract_cookie(const ap_http::Request&, const std::string& name);

    // Spool helpers
    std::string spool_submit(const std::string& command,
                             const std::vector<std::string>& args,
                             int timeout_ms);

    ap_http::Server m_http;
    mutable std::mutex m_sess_mu;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_sessions;
};

} // namespace ap_app
