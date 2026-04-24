// AdmiralsPanel standalone HTTP server (Winsock2).
//
// Serves the admin web panel + a JSON API, replacing our dependency on
// WindrosePlus's dashboard. Runs in its own thread; each incoming connection
// is handled on a short-lived thread.
//
// Route handlers receive the request and return a Response. HTTP/1.1, no
// persistent connections (Connection: close on every response), plain text
// parsing — good enough for a single-operator admin panel on localhost or
// LAN.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ap_http {

struct Request
{
    std::string method;                                  // "GET" / "POST" / ...
    std::string path;                                    // "/api/rcon" (no query)
    std::string query;                                   // raw query after '?', may be empty
    std::unordered_map<std::string, std::string> headers;// lowercased keys
    std::string body;
    std::string client_ip;
};

struct Response
{
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra_headers; // e.g. Set-Cookie
};

using Handler = std::function<Response(const Request&)>;

class Server
{
public:
    Server();
    ~Server();

    // Register a handler for an exact path. Most-specific match wins;
    // unmatched paths fall through to the static-file root (if set) or 404.
    void route(std::string path, Handler h);

    // Serve static files from this directory for any path not matched by a
    // route. Pass empty string to disable (404 for unmatched).
    void static_root(std::string dir);

    // Start listening on the given port. Returns false on bind failure.
    bool start(uint16_t port);

    // Stop accepting + join the accept thread.
    void stop();

    uint16_t listen_port() const { return m_port; }
    bool is_running() const { return m_running.load(); }

private:
    void accept_loop();
    void handle_connection(uintptr_t client_socket);

    Handler resolve_handler(const std::string& path) const;

    std::unordered_map<std::string, Handler> m_routes;
    std::string                              m_static_root;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop_flag{false};
    uintptr_t         m_listen_socket = ~uintptr_t{0}; // INVALID_SOCKET
    uint16_t          m_port = 0;
    std::thread       m_accept_thread;
};

// Helpers for handlers:
Response json_response(int status, const std::string& json);
Response text_response(int status, const std::string& text);
Response html_response(int status, const std::string& html);
Response redirect_response(const std::string& location);
std::string url_decode(const std::string& in);
std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string& body);

} // namespace ap_http
