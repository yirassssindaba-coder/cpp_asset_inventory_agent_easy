#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <iostream>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  // IMPORTANT: winsock2.h must be included before windows.h
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #if defined(_MSC_VER)
    #pragma comment(lib, "ws2_32.lib")
  #endif
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <errno.h>
  #include <sys/stat.h>
  #if defined(__linux__)
    #include <sys/sysinfo.h>
  #endif
  #if defined(__APPLE__)
    #include <sys/sysctl.h>
  #endif
#endif

// ------------------------------
// logging helpers
// ------------------------------
static void log_info(const std::string& msg) { std::cout << "[INFO] " << msg << "\n"; }
static void log_warn(const std::string& msg) { std::cout << "[WARN] " << msg << "\n"; }
static void log_err (const std::string& msg) { std::cerr << "[ERROR] " << msg << "\n"; }

// ------------------------------
// system info (best-effort)
// ------------------------------
static std::string now_iso8601_local() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return std::string(buf);
}

static std::string get_hostname() {
    char buf[256] = {0};
#if defined(_WIN32)
    DWORD sz = (DWORD)sizeof(buf);
    if (GetComputerNameA(buf, &sz)) return std::string(buf);
    return "unknown";
#else
    if (gethostname(buf, sizeof(buf)-1) == 0) return std::string(buf);
    return "unknown";
#endif
}

static std::string get_os_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "UnknownOS";
#endif
}

static long long get_total_ram_mb() {
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) {
        return (long long)(st.ullTotalPhys / (1024ULL * 1024ULL));
    }
    return -1;
#elif defined(__linux__)
    struct sysinfo info{};
    if (sysinfo(&info) == 0) {
        unsigned long long total = (unsigned long long)info.totalram * (unsigned long long)info.mem_unit;
        return (long long)(total / (1024ULL * 1024ULL));
    }
    return -1;
#elif defined(__APPLE__)
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t size = 0;
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, nullptr, 0) == 0) {
        return (long long)(size / (1024ULL * 1024ULL));
    }
    return -1;
#else
    return -1;
#endif
}

static unsigned get_cpu_cores() {
    unsigned c = std::thread::hardware_concurrency();
    return c ? c : 0;
}

// ------------------------------
// tiny JSON build + schema check
// ------------------------------
static std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (char ch : s) {
        switch (ch) {
            case '\\': o << "\\\\"; break;
            case '"':  o << "\\\""; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: o << ch;
        }
    }
    return o.str();
}

static bool json_has_required_keys(const std::string& body, std::string* why) {
    auto has = [&](const char* k) {
        std::string kk = "\"";
        kk += k;
        kk += "\"";
        return body.find(kk) != std::string::npos;
    };
    if (body.size() < 2 || body.front() != '{' || body.back() != '}') {
        if (why) *why = "Body is not a JSON object.";
        return false;
    }
    const char* req[] = {"hostname", "os", "timestamp"};
    for (auto k : req) {
        if (!has(k)) {
            if (why) { *why = "Missing required key: "; *why += k; }
            return false;
        }
    }
    return true;
}

static std::string build_asset_json(const std::string& id, const std::string& ip_guess) {
    const std::string hostname = get_hostname();
    const std::string os = get_os_name();
    const unsigned cores = get_cpu_cores();
    const long long ram_mb = get_total_ram_mb();
    const std::string ts = now_iso8601_local();

    std::ostringstream out;
    out << "{";
    out << "\"id\":\"" << json_escape(id) << "\",";
    out << "\"hostname\":\"" << json_escape(hostname) << "\",";
    out << "\"os\":\"" << json_escape(os) << "\",";
    out << "\"cpu_cores\":" << cores << ",";
    if (ram_mb >= 0) out << "\"ram_mb\":" << ram_mb << ",";
    else out << "\"ram_mb\":\"N/A\",";
    out << "\"ip\":\"" << json_escape(ip_guess.empty() ? "N/A" : ip_guess) << "\",";
    out << "\"timestamp\":\"" << json_escape(ts) << "\"";
    out << "}";
    return out.str();
}

// ------------------------------
// sockets (client/server) - portable
// ------------------------------
#if defined(_WIN32)
static bool winsock_init() {
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2,2), &wsa) == 0;
}
static void winsock_cleanup() { WSACleanup(); }
static int last_sock_err() { return WSAGetLastError(); }
static int close_socket(int fd) { return closesocket((SOCKET)fd); }
#else
static int last_sock_err() { return errno; }
static int close_socket(int fd) { return close(fd); }
#endif

static std::string err_text(int e) {
#if defined(_WIN32)
    char *msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, (DWORD)e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&msg, 0, NULL);
    std::string out = msg ? msg : "unknown";
    if (msg) LocalFree(msg);
    return out;
#else
    return std::strerror(e);
#endif
}

static int set_nonblocking(int fd, bool nb) {
#if defined(_WIN32)
    u_long mode = nb ? 1UL : 0UL;
    return ioctlsocket((SOCKET)fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (nb) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#endif
}

static int connect_timeout(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);

    int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
    if (gai != 0 || !res) {
#if defined(_WIN32)
        log_err("getaddrinfo failed.");
#else
        log_err(std::string("getaddrinfo failed: ") + gai_strerror(gai));
#endif
        return -1;
    }

    int sockfd = -1;
    for (auto p = res; p; p = p->ai_next) {
        int s = (int)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
#if defined(_WIN32)
        if (s == (int)INVALID_SOCKET) continue;
#else
        if (s < 0) continue;
#endif
        set_nonblocking(s, true);

        int rc = connect(s, p->ai_addr, (int)p->ai_addrlen);
        if (rc == 0) {
            set_nonblocking(s, false);
            sockfd = s;
            break;
        }

        int e = last_sock_err();
#if defined(_WIN32)
        if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS) { close_socket(s); continue; }
#else
        if (e != EINPROGRESS) { close_socket(s); continue; }
#endif

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET((SOCKET)s, &wfds);

        struct timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int sel = select(s + 1, nullptr, &wfds, nullptr, &tv);
        if (sel > 0 && FD_ISSET((SOCKET)s, &wfds)) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt((SOCKET)s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
            if (so_error == 0) {
                set_nonblocking(s, false);
                sockfd = s;
                break;
            }
        }
        close_socket(s);
    }

    freeaddrinfo(res);
    return sockfd;
}

static bool send_all(int sockfd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)send((SOCKET)sockfd, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static std::string recv_all(int sockfd) {
    std::string out;
    char buf[4096];
    for (;;) {
        int n = (int)recv((SOCKET)sockfd, buf, (int)sizeof(buf), 0);
        if (n > 0) out.append(buf, buf + n);
        else break;
    }
    return out;
}

static int http_post_json(const std::string& host, int port, const std::string& path, const std::string& json_body, int timeout_ms) {
    int s = connect_timeout(host, port, timeout_ms);
    if (s < 0) return -1;

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Content-Type: application/json\r\n";
    req << "Content-Length: " << json_body.size() << "\r\n";
    req << "Connection: close\r\n\r\n";
    req << json_body;

    if (!send_all(s, req.str())) {
        close_socket(s);
        return -2;
    }

    std::string resp = recv_all(s);
    close_socket(s);

    int code = 0;
    if (resp.rfind("HTTP/", 0) == 0) {
        size_t sp = resp.find(' ');
        if (sp != std::string::npos && sp + 4 < resp.size()) {
            code = std::atoi(resp.c_str() + sp + 1);
        }
    }
    return code;
}

static std::string http_response(int code, const std::string& content_type, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " ";
    out << (code == 200 ? "OK" : code == 201 ? "Created" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error");
    out << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << body;
    return out.str();
}

static bool read_until(int sockfd, std::string& data, const std::string& delim, int max_bytes = 1<<20) {
    char buf[4096];
    while ((int)data.size() < max_bytes) {
        if (data.find(delim) != std::string::npos) return true;
        int n = (int)recv((SOCKET)sockfd, buf, (int)sizeof(buf), 0);
        if (n <= 0) return false;
        data.append(buf, buf + n);
    }
    return false;
}

static void server_loop(int port, const std::string& db_path) {
#if defined(_WIN32)
    if (!winsock_init()) {
        log_err("WSAStartup failed.");
        return;
    }
#endif

    int listen_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32)
    if (listen_fd == (int)INVALID_SOCKET) { log_err("socket() failed."); winsock_cleanup(); return; }
#else
    if (listen_fd < 0) { log_err("socket() failed."); return; }
#endif

    int opt = 1;
    setsockopt((SOCKET)listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind((SOCKET)listen_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        int e = last_sock_err();
        log_err("bind() failed: " + err_text(e));
        close_socket(listen_fd);
#if defined(_WIN32)
        winsock_cleanup();
#endif
        return;
    }

    if (listen((SOCKET)listen_fd, 16) != 0) {
        int e = last_sock_err();
        log_err("listen() failed: " + err_text(e));
        close_socket(listen_fd);
#if defined(_WIN32)
        winsock_cleanup();
#endif
        return;
    }

    log_info("Server listening on http://127.0.0.1:" + std::to_string(port) + "/");
    log_info("DB file: " + db_path);

    const std::string html = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Asset Inventory Dashboard</title>
  <style>
    body{font-family:system-ui,Segoe UI,Arial;margin:24px}
    h1{margin:0 0 12px}
    table{border-collapse:collapse;width:100%}
    th,td{border:1px solid #ddd;padding:10px;font-size:14px}
    th{background:#f3f4f6;text-align:left}
    .muted{color:#6b7280}
    .row{display:flex;gap:12px;align-items:center;margin:12px 0}
    a.btn{padding:8px 12px;border:1px solid #ddd;border-radius:10px;text-decoration:none}
  </style>
</head>
<body>
  <h1>Asset Inventory Dashboard</h1>
  <div class="row">
    <span class="muted">Refresh: otomatis setiap 3 detik</span>
    <a class="btn" href="/api/export.csv">Export CSV</a>
  </div>
  <table>
    <thead>
      <tr><th>ID</th><th>Hostname</th><th>OS</th><th>Cores</th><th>RAM (MB)</th><th>IP</th><th>Timestamp</th></tr>
    </thead>
    <tbody id="tb"></tbody>
  </table>

<script>
async function load(){
  const r = await fetch('/api/assets');
  const data = await r.json();
  const tb = document.getElementById('tb');
  tb.innerHTML='';
  for (const it of data){
    const tr=document.createElement('tr');
    tr.innerHTML = `<td>${it.id||''}</td><td>${it.hostname||''}</td><td>${it.os||''}</td><td>${it.cpu_cores||''}</td><td>${it.ram_mb||''}</td><td>${it.ip||''}</td><td>${it.timestamp||''}</td>`;
    tb.appendChild(tr);
  }
}
load();
setInterval(load, 3000);
</script>
</body>
</html>
)HTML";

    for (;;) {
        sockaddr_in client{};
#if defined(_WIN32)
        int clen = sizeof(client);
#else
        socklen_t clen = sizeof(client);
#endif
        int cfd = (int)accept((SOCKET)listen_fd, (sockaddr*)&client, &clen);
#if defined(_WIN32)
        if (cfd == (int)INVALID_SOCKET) continue;
#else
        if (cfd < 0) continue;
#endif

        std::string data;
        if (!read_until(cfd, data, "\r\n\r\n")) { close_socket(cfd); continue; }

        size_t line_end = data.find("\r\n");
        std::string req_line = line_end == std::string::npos ? data : data.substr(0, line_end);
        std::istringstream rl(req_line);
        std::string method, path, ver;
        rl >> method >> path >> ver;

        size_t hdr_end = data.find("\r\n\r\n");
        std::string headers = data.substr(0, hdr_end);
        std::string body = data.substr(hdr_end + 4);

        long long content_len = 0;
        {
            std::istringstream hs(headers);
            std::string hline;
            while (std::getline(hs, hline)) {
                if (!hline.empty() && hline.back() == '\r') hline.pop_back();
                std::string lower = hline;
                for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
                if (lower.rfind("content-length:", 0) == 0) {
                    content_len = std::atoll(hline.c_str() + std::string("Content-Length:").size());
                }
            }
        }
        while ((long long)body.size() < content_len) {
            char buf[4096];
            int n = (int)recv((SOCKET)cfd, buf, (int)sizeof(buf), 0);
            if (n <= 0) break;
            body.append(buf, buf + n);
        }

        if (method == "GET" && (path == "/" || path == "/index.html")) {
            std::string resp = http_response(200, "text/html; charset=utf-8", html);
            send_all(cfd, resp);
            close_socket(cfd);
            continue;
        }

        if (method == "GET" && path == "/api/assets") {
            std::ifstream in(db_path, std::ios::in);
            std::ostringstream out;
            out << "[";
            bool first = true;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                if (!first) out << ",";
                out << line;
                first = false;
            }
            out << "]";
            std::string resp = http_response(200, "application/json; charset=utf-8", out.str());
            send_all(cfd, resp);
            close_socket(cfd);
            continue;
        }

        if (method == "GET" && path == "/api/export.csv") {
            auto pick = [](const std::string& json, const char* key)->std::string{
                std::string k = "\""; k += key; k += "\":";
                auto pos = json.find(k);
                if (pos == std::string::npos) return "";
                pos += k.size();
                while (pos < json.size() && (json[pos] == ' ')) pos++;
                if (pos < json.size() && json[pos] == '"') {
                    pos++;
                    auto end = json.find('"', pos);
                    if (end == std::string::npos) return "";
                    return json.substr(pos, end - pos);
                } else {
                    auto end = json.find_first_of(",}", pos);
                    if (end == std::string::npos) end = json.size();
                    return json.substr(pos, end - pos);
                }
            };

            std::ifstream in(db_path);
            std::ostringstream csv;
            csv << "id,hostname,os,cpu_cores,ram_mb,ip,timestamp\n";
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                csv << pick(line, "id") << ","
                    << pick(line, "hostname") << ","
                    << pick(line, "os") << ","
                    << pick(line, "cpu_cores") << ","
                    << pick(line, "ram_mb") << ","
                    << pick(line, "ip") << ","
                    << pick(line, "timestamp") << "\n";
            }

            std::string resp = http_response(200, "text/csv; charset=utf-8", csv.str());
            send_all(cfd, resp);
            close_socket(cfd);
            continue;
        }

        if (method == "POST" && path == "/api/assets") {
            std::string why;
            if (!json_has_required_keys(body, &why)) {
                std::string resp = http_response(400, "application/json; charset=utf-8", std::string("{\"error\":\"") + json_escape(why) + "\"}");
                send_all(cfd, resp);
                close_socket(cfd);
                continue;
            }

            { std::ofstream out(db_path, std::ios::app); out << body << "\n"; }

            std::string resp = http_response(201, "application/json; charset=utf-8", "{\"ok\":true}");
            send_all(cfd, resp);
            close_socket(cfd);
            continue;
        }

        std::string resp = http_response(404, "application/json; charset=utf-8", "{\"error\":\"not found\"}");
        send_all(cfd, resp);
        close_socket(cfd);
    }
}

static void print_help() {
    std::cout <<
R"HELP(
Asset Inventory (C++17) - single binary

USAGE:
  asset_inventory server --port 8080 [--db data/assets.jsonl]
  asset_inventory agent  --host 127.0.0.1 --port 8080 --path /api/assets [--retries 3] [--timeout 2000] [--id <id>] [--ip <ip>]

EXAMPLES:
  ./bin/asset_inventory server --port 8080
  ./bin/asset_inventory agent --host 127.0.0.1 --port 8080 --path /api/assets --retries 3 --timeout 2000
)HELP";
}

static std::string arg_value(const std::vector<std::string>& args, const std::string& key, const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); i++) if (args[i] == key) return args[i+1];
    return def;
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    winsock_init();
#endif

    if (argc < 2) {
        print_help();
        return 1;
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);
    const std::string mode = args[0];

    if (mode == "--help" || mode == "-h" || mode == "help") {
        print_help();
        return 0;
    }

    if (mode == "server") {
        int port = std::atoi(arg_value(args, "--port", "8080").c_str());
        std::string db = arg_value(args, "--db", "data/assets.jsonl");

        // Create db dir best-effort
        auto slash = db.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::string dir = db.substr(0, slash);
#if defined(_WIN32)
            CreateDirectoryA(dir.c_str(), NULL);
#else
            ::mkdir(dir.c_str(), 0755);
#endif
        }

        server_loop(port, db);
        return 0;
    }

    if (mode == "agent") {
        const std::string host = arg_value(args, "--host", "127.0.0.1");
        const int port = std::atoi(arg_value(args, "--port", "8080").c_str());
        const std::string path = arg_value(args, "--path", "/api/assets");
        const int retries = std::atoi(arg_value(args, "--retries", "3").c_str());
        const int timeout_ms = std::atoi(arg_value(args, "--timeout", "2000").c_str());
        std::string id = arg_value(args, "--id", "");

        if (id.empty()) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
            id = get_hostname() + "-" + std::to_string((long long)ms);
        }
        const std::string ip = arg_value(args, "--ip", "");

        const std::string payload = build_asset_json(id, ip);

        std::string why;
        if (!json_has_required_keys(payload, &why)) {
            log_err("Internal payload schema invalid: " + why);
            return 2;
        }

        log_info("Sending inventory JSON to http://" + host + ":" + std::to_string(port) + path);
        log_info("Payload: " + payload);

        int attempt = 0;
        while (attempt <= retries) {
            int code = http_post_json(host, port, path, payload, timeout_ms);
            if (code == 201 || code == 200) {
                log_info("Send OK (HTTP " + std::to_string(code) + ")");
                return 0;
            }

            if (code > 0) log_warn("Server response HTTP " + std::to_string(code));
            else log_warn("Network/connection failed (code " + std::to_string(code) + ")");

            attempt++;
            if (attempt > retries) break;

            int backoff_ms = 500 * attempt;
            log_info("Retry in " + std::to_string(backoff_ms) + "ms (" + std::to_string(attempt) + "/" + std::to_string(retries) + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        }

        log_err("Failed to send after retries.");
        return 1;
    }

    log_err("Unknown mode: " + mode);
    print_help();
    return 1;
}
