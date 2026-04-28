#include "http_server.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace ap_http {

namespace {

struct WsaInit
{
    WsaInit()
    {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WsaInit()
    {
        WSACleanup();
    }
};

WsaInit& wsa()
{
    static WsaInit inst;
    return inst;
}

std::string to_lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool read_until(SOCKET sock, const std::string& delim, std::string& buf, size_t max_bytes)
{
    char ch;
    while (buf.size() < max_bytes) {
        int n = recv(sock, &ch, 1, 0);
        if (n <= 0) return false;
        buf.push_back(ch);
        if (buf.size() >= delim.size() &&
            buf.compare(buf.size() - delim.size(), delim.size(), delim) == 0) {
            return true;
        }
    }
    return false;
}

bool read_exact(SOCKET sock, size_t n, std::string& out)
{
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        int r = recv(sock, out.data() + got, static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool send_all(SOCKET sock, const char* data, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        int r = send(sock, data + sent, static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

std::string mime_type(const std::string& path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = to_lower(path.substr(dot + 1));
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "txt")  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

const char* status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Status";
    }
}

void write_response(SOCKET sock, const Response& r)
{
    std::ostringstream os;
    os << "HTTP/1.1 " << r.status << " " << status_text(r.status) << "\r\n";
    os << "Content-Type: " << r.content_type << "\r\n";
    os << "Content-Length: " << r.body.size() << "\r\n";
    os << "Connection: close\r\n";
    os << "Cache-Control: no-store\r\n";
    for (const auto& h : r.extra_headers) {
        os << h.first << ": " << h.second << "\r\n";
    }
    os << "\r\n";
    std::string head = os.str();
    send_all(sock, head.data(), head.size());
    if (!r.body.empty()) send_all(sock, r.body.data(), r.body.size());
}

bool parse_request_line(const std::string& line, Request& req)
{
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    req.method = line.substr(0, sp1);
    std::string url = line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qm = url.find('?');
    if (qm != std::string::npos) {
        req.path  = url.substr(0, qm);
        req.query = url.substr(qm + 1);
    } else {
        req.path  = url;
        req.query = "";
    }
    return true;
}

void parse_headers(const std::string& block, Request& req)
{
    size_t i = 0;
    while (i < block.size()) {
        size_t eol = block.find("\r\n", i);
        std::string line;
        if (eol == std::string::npos) {
            line = block.substr(i);
            i = block.size();
        } else {
            line = block.substr(i, eol - i);
            i = eol + 2;
        }
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name  = to_lower(line.substr(0, colon));
        size_t vstart = colon + 1;
        while (vstart < line.size() && (line[vstart] == ' ' || line[vstart] == '\t')) ++vstart;
        std::string value = line.substr(vstart);
        req.headers[name] = value;
    }
}

bool path_safe(const std::string& rel)
{
    // No .. segments, no absolute paths, no backslashes.
    if (rel.find("..") != std::string::npos) return false;
    if (rel.find('\\') != std::string::npos) return false;
    if (!rel.empty() && rel[0] != '/') return false;
    return true;
}

Response serve_static(const std::string& root, std::string path)
{
    if (path == "/" || path.empty()) path = "/index.html";
    if (!path_safe(path)) return text_response(403, "forbidden");

    std::filesystem::path full(root);
    full /= path.substr(1); // strip leading '/'
    std::error_code ec;
    if (!std::filesystem::exists(full, ec) || std::filesystem::is_directory(full, ec)) {
        return text_response(404, "not found: " + path);
    }

    std::ifstream f(full, std::ios::binary);
    if (!f) return text_response(500, "read failed");
    std::ostringstream o;
    o << f.rdbuf();
    Response r;
    r.status       = 200;
    r.content_type = mime_type(path);
    r.body         = o.str();
    return r;
}

} // namespace

Response json_response(int status, const std::string& json)
{
    Response r;
    r.status       = status;
    r.content_type = "application/json; charset=utf-8";
    r.body         = json;
    return r;
}

Response text_response(int status, const std::string& text)
{
    Response r;
    r.status       = status;
    r.content_type = "text/plain; charset=utf-8";
    r.body         = text;
    return r;
}

Response html_response(int status, const std::string& html)
{
    Response r;
    r.status       = status;
    r.content_type = "text/html; charset=utf-8";
    r.body         = html;
    return r;
}

Response redirect_response(const std::string& location)
{
    Response r;
    r.status       = 302;
    r.content_type = "text/plain; charset=utf-8";
    r.body         = "redirecting to " + location;
    r.extra_headers.push_back({"Location", location});
    return r;
}

std::string url_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') { out.push_back(' '); continue; }
        if (c == '%' && i + 2 < in.size()) {
            auto h = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                return -1;
            };
            int hi = h(in[i + 1]);
            int lo = h(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::unordered_map<std::string, std::string> parse_form_urlencoded(const std::string& body)
{
    std::unordered_map<std::string, std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        size_t amp = body.find('&', i);
        std::string pair = body.substr(i, (amp == std::string::npos) ? std::string::npos : amp - i);
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            if (!pair.empty()) out[url_decode(pair)] = "";
        } else {
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return out;
}

Server::Server() { wsa(); }

Server::~Server() { stop(); }

void Server::route(std::string path, Handler h)
{
    m_routes[std::move(path)] = std::move(h);
}

void Server::static_root(std::string dir)
{
    m_static_root = std::move(dir);
}

bool Server::start(uint16_t port)
{
    if (m_running.load()) return false;

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return false;
    }
    if (::listen(s, 8) != 0) {
        closesocket(s);
        return false;
    }

    m_listen_socket = static_cast<uintptr_t>(s);
    m_port          = port;
    m_stop_flag     = false;
    m_running       = true;
    m_accept_thread = std::thread([this] { accept_loop(); });
    return true;
}

void Server::stop()
{
    if (!m_running.load()) return;
    m_stop_flag = true;
    SOCKET s = static_cast<SOCKET>(m_listen_socket);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
        m_listen_socket = ~uintptr_t{0};
    }
    if (m_accept_thread.joinable()) m_accept_thread.join();
    m_running = false;
}

Handler Server::resolve_handler(const std::string& path) const
{
    auto it = m_routes.find(path);
    if (it != m_routes.end()) return it->second;
    return nullptr;
}

void Server::accept_loop()
{
    SOCKET listen_s = static_cast<SOCKET>(m_listen_socket);
    while (!m_stop_flag.load()) {
        sockaddr_in cli{};
        int sz = sizeof(cli);
        SOCKET c = accept(listen_s, reinterpret_cast<sockaddr*>(&cli), &sz);
        if (c == INVALID_SOCKET) {
            if (m_stop_flag.load()) break;
            continue;
        }
        DWORD tmo = 10000; // 10s recv timeout per connection
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        std::thread(
            [this, cs = static_cast<uintptr_t>(c)] { handle_connection(cs); }
        ).detach();
    }
}

void Server::handle_connection(uintptr_t client_socket)
{
    SOCKET sock = static_cast<SOCKET>(client_socket);

    // Resolve the peer IP for localhost-trust.
    Request req;
    {
        sockaddr_in peer{};
        int pn = sizeof(peer);
        if (getpeername(sock, reinterpret_cast<sockaddr*>(&peer), &pn) == 0) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            req.client_ip = ip;
        }
    }

    // Read request line.
    std::string line;
    if (!read_until(sock, "\r\n", line, 4096)) { closesocket(sock); return; }
    line.resize(line.size() - 2);

    if (!parse_request_line(line, req)) {
        write_response(sock, text_response(400, "bad request line"));
        closesocket(sock);
        return;
    }

    // Read headers.
    std::string header_block;
    if (!read_until(sock, "\r\n\r\n", header_block, 32768)) {
        write_response(sock, text_response(400, "bad headers"));
        closesocket(sock);
        return;
    }
    header_block.resize(header_block.size() - 4);
    parse_headers(header_block, req);

    // Read body if Content-Length present.
    auto cl_it = req.headers.find("content-length");
    if (cl_it != req.headers.end()) {
        size_t len = std::strtoul(cl_it->second.c_str(), nullptr, 10);
        if (len > 0) {
            if (len > 8 * 1024 * 1024) {
                write_response(sock, text_response(413, "body too large"));
                closesocket(sock);
                return;
            }
            if (!read_exact(sock, len, req.body)) {
                write_response(sock, text_response(400, "short body"));
                closesocket(sock);
                return;
            }
        }
    }

    Response resp;
    Handler  h = resolve_handler(req.path);
    if (h) {
        try {
            resp = h(req);
        } catch (...) {
            resp = text_response(500, "handler exception");
        }
    } else if (!m_static_root.empty()) {
        resp = serve_static(m_static_root, req.path);
    } else {
        resp = text_response(404, "no route");
    }

    write_response(sock, resp);
    closesocket(sock);
}

} // namespace ap_http
