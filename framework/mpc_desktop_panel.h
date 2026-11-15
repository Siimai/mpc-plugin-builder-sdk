// mpc_desktop_panel.h - DEVELOPMENT ONLY: a browser control panel for the desktop (Mac/Linux) VST2 build.
//
// Compiled in only by `tools/build.py <project> --target desktop` (it defines MPC_DESKTOP_PANEL).
// The MPC builds never contain this code.
//
// What it does: when the host creates the plugin, a tiny HTTP server starts on 127.0.0.1:8765 (or the next free
// port up to 8780, one per plugin instance) in a background thread. Open http://127.0.0.1:8765 in a browser and you
// get the plugin's touch-screen skin (Plugin Skins/TUI.json) drawn as a live, clickable page, or a plain slider list
// when the plugin has no skin. The page reads and writes the plugin's parameters through the same Plugin object the
// host uses. Files served: <plugin folder>/panel.html (copied there by build.py) and <plugin folder>/Plugin Skins/*.
//
// Not realtime-safe on purpose (it is a debugging aid): it runs on its own thread, reads/writes parameters through
// std::atomic values and does not tell the host about the change, so Reaper will not record panel moves as
// automation. Loopback only. POSIX only (macOS, Linux); on Windows the panel is simply not compiled.
#ifndef MPC_DESKTOP_PANEL_H
#define MPC_DESKTOP_PANEL_H

#if defined(MPC_DESKTOP_PANEL) && !defined(_WIN32)

// (system headers are included at the top of mpc_plugin.h; this file is included inside namespace mpc)

namespace detail {

class DesktopPanel
{
public:
    explicit DesktopPanel(Plugin* plugin) : plugin_(plugin)
    {
        for (int port = 8765; port <= 8780 && listenFd_ < 0; ++port) tryListen(port);
        if (listenFd_ < 0) { std::fprintf(stderr, "[panel] no free port in 8765..8780\n"); return; }
        std::fprintf(stderr, "[panel] %s: http://127.0.0.1:%d\n", plugin_->info().name, port_);
        thread_ = std::thread([this] { loop(); });
    }

    ~DesktopPanel()
    {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (listenFd_ >= 0) ::close(listenFd_);
    }

private:
    Plugin* plugin_;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;

    void tryListen(int port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) { ::close(fd); return; }
        listenFd_ = fd;
        port_ = port;
    }

    void loop()
    {
        while (!stop_.load()) {
            fd_set rf; FD_ZERO(&rf); FD_SET(listenFd_, &rf);
            timeval tv{0, 200000};
            if (::select(listenFd_ + 1, &rf, nullptr, nullptr, &tv) <= 0) continue;
            int c = ::accept(listenFd_, nullptr, nullptr);
            if (c < 0) continue;
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            timeval rt{2, 0};
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
            handle(c);
            ::close(c);
        }
    }

    // ---- tiny HTTP ---------------------------------------------------------
    static void sendAll(int fd, const char* d, size_t n)
    {
        while (n > 0) {
            ssize_t w = ::send(fd, d, n, 0);
            if (w <= 0) return;
            d += w; n -= (size_t)w;
        }
    }

    static void reply(int fd, int code, const char* type, const std::string& body)
    {
        char head[256];
        std::snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n",
                      code, code == 200 ? "OK" : "Not Found", type, body.size());
        sendAll(fd, head, std::strlen(head));
        sendAll(fd, body.data(), body.size());
    }

    static std::string urlDecode(const std::string& s)
    {
        std::string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
                o += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else if (s[i] == '+') o += ' ';
            else o += s[i];
        }
        return o;
    }

    static std::string query(const std::string& q, const char* key)
    {
        std::string k = std::string(key) + "=";
        size_t p = 0;
        while (p < q.size()) {
            size_t e = q.find('&', p);
            if (e == std::string::npos) e = q.size();
            if (q.compare(p, k.size(), k) == 0) return q.substr(p + k.size(), e - p - k.size());
            p = e + 1;
        }
        return "";
    }

    static std::string jsonStr(const std::string& s)
    {
        std::string o = "\"";
        for (unsigned char ch : s) {
            if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
            else if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
            else o += (char)ch;
        }
        return o + "\"";
    }

    static const char* mimeFor(const std::string& path)
    {
        auto ends = [&](const char* e) { size_t n = std::strlen(e); return path.size() >= n && path.compare(path.size() - n, n, e) == 0; };
        if (ends(".png")) return "image/png";
        if (ends(".json")) return "application/json";
        if (ends(".html")) return "text/html; charset=utf-8";
        if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
        if (ends(".svg")) return "image/svg+xml";
        if (ends(".ttf")) return "font/ttf";
        if (ends(".otf")) return "font/otf";
        return "application/octet-stream";
    }

    void handle(int fd)
    {
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0) return;
        buf[n] = 0;
        std::string req(buf);
        if (req.compare(0, 4, "GET ") != 0) { reply(fd, 404, "text/plain", "GET only"); return; }
        size_t sp = req.find(' ', 4);
        if (sp == std::string::npos) return;
        std::string target = req.substr(4, sp - 4);
        std::string path = target, q;
        size_t qm = target.find('?');
        if (qm != std::string::npos) { path = target.substr(0, qm); q = target.substr(qm + 1); }
        path = urlDecode(path);

        if (path == "/api/params") { reply(fd, 200, "application/json", paramsJson()); return; }
        if (path == "/api/state") { reply(fd, 200, "application/json", stateJson()); return; }
        if (path == "/api/set") {
            int i = std::atoi(query(q, "i").c_str());
            float v = (float)std::atof(query(q, "v").c_str());
            plugin_->setNormalised(i, v);
            reply(fd, 200, "application/json", "{}");
            return;
        }
        if (path == "/api/program") {
            plugin_->setProgram(std::atoi(query(q, "n").c_str()));
            reply(fd, 200, "application/json", "{}");
            return;
        }

        std::string file;
        const std::string& folder = pluginFolder();
        if (path == "/" || path == "/index.html") file = folder + "/panel.html";
        else if (path.compare(0, 6, "/skin/") == 0) file = folder + "/Plugin Skins/" + path.substr(6);
        else { reply(fd, 404, "text/plain", "not found"); return; }
        if (file.find("..") != std::string::npos) { reply(fd, 404, "text/plain", "not found"); return; }
        std::vector<uint8_t> bytes;
        if (!readFile(file, bytes)) { reply(fd, 404, "text/plain", "not found"); return; }
        reply(fd, 200, mimeFor(file), std::string(bytes.begin(), bytes.end()));
    }

    // ---- JSON --------------------------------------------------------------
    std::string paramsJson()
    {
        const PluginInfo& info = plugin_->info();
        std::string o = "{\"name\":" + jsonStr(info.name) + ",\"vendor\":" + jsonStr(info.vendor) + ",\"programs\":[";
        for (int i = 0; i < plugin_->numPrograms(); ++i) { if (i) o += ','; o += jsonStr(plugin_->programName(i)); }
        o += "],\"params\":[";
        for (int i = 0; i < plugin_->numParams(); ++i) {
            const Param& p = plugin_->param(i);
            if (i) o += ',';
            char b[64];
            std::snprintf(b, sizeof b, "%.6f", p.toNormalised(p.def));
            o += "{\"name\":" + jsonStr(p.name) + ",\"unit\":" + jsonStr(p.unit) + ",\"def\":" + b + ",\"choices\":[";
            for (size_t k = 0; k < p.choices.size(); ++k) { if (k) o += ','; o += jsonStr(p.choices[k]); }
            o += "]}";
        }
        return o + "]}";
    }

    std::string stateJson()
    {
        std::string o = "{\"program\":" + std::to_string(plugin_->currentProgram()) + ",\"v\":[";
        for (int i = 0; i < plugin_->numParams(); ++i) {
            char b[32];
            std::snprintf(b, sizeof b, i ? ",%.5f" : "%.5f", plugin_->getNormalised(i));
            o += b;
        }
        o += "],\"d\":[";
        for (int i = 0; i < plugin_->numParams(); ++i) { if (i) o += ','; o += jsonStr(plugin_->displayText(i)); }
        return o + "]}";
    }
};

inline void panelStart(Plugin* p, void*& slot) { slot = new DesktopPanel(p); }
inline void panelStop(void*& slot) { delete static_cast<DesktopPanel*>(slot); slot = nullptr; }

} // namespace detail

#endif // MPC_DESKTOP_PANEL && !_WIN32
#endif // MPC_DESKTOP_PANEL_H
