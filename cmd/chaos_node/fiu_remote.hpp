#pragma once

// Minimal TCP server that speaks the libfiu remote-control line protocol.
// Replaces fiu-rc.h / fiu_rc_tcp() (not present in the Ubuntu libfiu-dev package).
//
// Protocol: one command per line (\n terminated), reply is a decimal integer
// followed by \n.  Commands (key=value format):
//   enable name=<name> [failnum=<n>] [one]
//   enable_random name=<name> [failnum=<n>] [probability=<p>]
//   disable name=<name>
//   disable_all
//
// Note: fiu_rc_string() in Ubuntu libfiu-dev 1.2 silently ignores enable_random
// and does not honour the "one" (FIU_ONETIME) flag.  We parse commands ourselves
// and call the C API directly.  disable_all is also missing from the API; we
// track enabled fault names and call fiu_disable() for each.

#ifdef KYTHIRA_FAULT_INJECTION

#include <fiu-control.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace chaos_node {

class fiu_tcp_rc {
public:
    explicit fiu_tcp_rc(std::uint16_t port) : _port(port) {}

    ~fiu_tcp_rc() { stop(); }

    fiu_tcp_rc(const fiu_tcp_rc&) = delete;
    fiu_tcp_rc& operator=(const fiu_tcp_rc&) = delete;

    void start() {
        if (_running.exchange(true)) return;

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            _running = false;
            throw std::runtime_error("fiu_tcp_rc: socket()");
        }

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(_port);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            _running = false;
            throw std::runtime_error("fiu_tcp_rc: bind() on port " + std::to_string(_port));
        }
        ::listen(fd, 32);
        _listen_fd = fd;
        _thread = std::thread([this] { accept_loop(); });
    }

    void stop() {
        if (!_running.exchange(false)) return;
        int fd = _listen_fd.exchange(-1);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (_thread.joinable()) _thread.join();
    }

private:
    void accept_loop() {
        while (_running) {
            int client = ::accept(_listen_fd.load(), nullptr, nullptr);
            if (client < 0) break;
            std::thread([this, client] {
                handle_client(client);
                ::close(client);
            }).detach();
        }
    }

    void handle_client(int fd) {
        std::string buf;
        char c;
        while (::recv(fd, &c, 1, 0) == 1) {
            if (c == '\n') {
                if (!buf.empty()) {
                    process(fd, buf);
                    buf.clear();
                }
            } else {
                buf += c;
            }
        }
    }

    // Parse "cmd key=val key=val [bare_flag]" into {key->val, bare_flag->""}.
    // The command word itself is stored under the empty key "".
    static auto parse_tokens(const std::string& line) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> kv;
        std::istringstream iss(line);
        std::string tok;
        bool first = true;
        while (iss >> tok) {
            if (first) {
                kv[""] = tok;
                first = false;
                continue;
            }
            auto eq = tok.find('=');
            if (eq != std::string::npos)
                kv.emplace(tok.substr(0, eq), tok.substr(eq + 1));
            else
                kv.emplace(tok, "");
        }
        return kv;
    }

    void process(int fd, const std::string& line) {
        auto kv = parse_tokens(line);
        auto cmd = kv[""];

        int rc = -1;

        if (cmd == "disable_all") {
            std::lock_guard lk(_names_mu);
            for (const auto& n : _enabled_names) ::fiu_disable(n.c_str());
            _enabled_names.clear();
            rc = 0;

        } else if (cmd == "disable") {
            auto it = kv.find("name");
            if (it != kv.end() && !it->second.empty()) {
                rc = ::fiu_disable(it->second.c_str());
                if (rc == 0) {
                    std::lock_guard lk(_names_mu);
                    _enabled_names.erase(it->second);
                }
            }

        } else if (cmd == "enable" || cmd == "enable_random") {
            auto name_it = kv.find("name");
            if (name_it != kv.end() && !name_it->second.empty()) {
                const std::string& name = name_it->second;
                int failnum = 1;
                if (auto it = kv.find("failnum"); it != kv.end()) failnum = std::stoi(it->second);

                if (cmd == "enable") {
                    unsigned flags = kv.count("one") ? FIU_ONETIME : 0;
                    rc = ::fiu_enable(name.c_str(), failnum, nullptr, flags);
                } else {
                    float prob = 1.0f;
                    if (auto it = kv.find("probability"); it != kv.end())
                        prob = std::stof(it->second);
                    rc = ::fiu_enable_random(name.c_str(), failnum, nullptr, 0, prob);
                }

                if (rc == 0) {
                    std::lock_guard lk(_names_mu);
                    _enabled_names.insert(name);
                }
            }
        }

        std::string reply = std::to_string(rc) + "\n";
        ::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
    }

    std::uint16_t _port;
    std::atomic<int> _listen_fd{-1};
    std::atomic<bool> _running{false};
    std::thread _thread;
    std::mutex _names_mu;
    std::set<std::string> _enabled_names;
};

}  // namespace chaos_node

#endif  // KYTHIRA_FAULT_INJECTION
