// HTTP node for the rfc2136_ldns_discovery Docker integration test.
//
// Registers an A record for this container under a shared DNS name via
// RFC 2136 dynamic update, then serves:
//   GET /health  → 200 "OK"
//   GET /peers   → JSON array [{id,address}, ...]
//
// Stopping the node with SIGTERM triggers clean deregistration (DELETE UPDATE).
//
// Environment variables:
//   NODE_ID        unique label (default: "node1")
//   HTTP_PORT      control-plane port (default: 9021)
//   RPC_PORT       not used for DNS A-record discovery; reserved (default: 7021)
//   DNS_SERVER     BIND9 IP (required)
//   DNS_ZONE       zone name (default: "example.local.")
//   DNS_SHARED_NAME shared A-record name (default: "cluster.example.local.")

// httplib must precede ldns headers: ldns redefines bool as _Bool in C++ TUs.
#include <httplib.h>

#include <raft/rfc2136_ldns_discovery.hpp>

#include <folly/init/Init.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#ifndef KYTHIRA_HAS_LDNS
#error "dns_discovery_node requires KYTHIRA_HAS_LDNS — build with libldns installed"
#endif

namespace {

httplib::Server* g_srv = nullptr;

void on_signal(int) {
    if (g_srv) g_srv->stop();
}

const char* env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : fallback;
}

// Resolves a hostname or IP literal to a dotted-decimal IPv4 string.
// Returns the input unchanged when already an IP literal or resolution fails.
std::string resolve_to_ip(const std::string& host) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        return host;
    }
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

// Resolve our own hostname to an IPv4 address string.
std::string self_ipv4() {
    char host_buf[256] = {};
    gethostname(host_buf, sizeof(host_buf) - 1);
    std::string ip = resolve_to_ip(host_buf);
    if (ip == host_buf) {
        throw std::runtime_error(std::string("dns_discovery_node: cannot resolve own hostname: ") +
                                 host_buf);
    }
    return ip;
}

}  // namespace

int main(int argc, char** argv) {
    folly::Init folly_init(&argc, &argv);

    const std::string node_id = env_or("NODE_ID", "node1");
    const int http_port = std::stoi(env_or("HTTP_PORT", "9021"));
    const std::string dns_server = env_or("DNS_SERVER", "");
    const std::string dns_zone = env_or("DNS_ZONE", "example.local.");
    const std::string shared_name = env_or("DNS_SHARED_NAME", "cluster.example.local.");

    if (dns_server.empty()) {
        std::cerr << "[dns_node] DNS_SERVER env var is required\n";
        return 1;
    }

    const std::string self_ip = self_ipv4();
    std::cout << "[dns_node] id=" << node_id << " ip=" << self_ip << " server=" << dns_server
              << " shared=" << shared_name << "\n";

    kythira::rfc2136_ldns_discovery::config cfg;
    cfg.query.server = resolve_to_ip(dns_server);
    cfg.query.port = 53;
    cfg.query.shared_name = shared_name;
    cfg.zone = dns_zone;
    cfg.ttl = 30;

    kythira::rfc2136_ldns_discovery discovery{cfg};

    try {
        discovery.register_node(node_id, self_ip).get();
        std::cout << "[dns_node] registered " << self_ip << " at " << shared_name << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "[dns_node] registration failed: " << ex.what() << "\n";
        return 1;
    }

    httplib::Server srv;
    g_srv = &srv;
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    srv.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    srv.Get("/peers", [&discovery](const httplib::Request& req, httplib::Response& res) {
        int timeout_ms = 2000;
        if (req.has_param("timeout_ms")) {
            try {
                timeout_ms = std::stoi(req.get_param_value("timeout_ms"));
            } catch (...) {
            }
        }
        auto peers = discovery.find_peers(std::chrono::milliseconds{timeout_ms}).get();
        std::string json = "[";
        bool first = true;
        for (const auto& p : peers) {
            if (!first) json += ',';
            json += R"({"id":")" + p.node_id + R"(","address":")" + p.address + R"("})";
            first = false;
        }
        json += ']';
        res.set_content(json, "application/json");
    });

    std::cout << "[dns_node] HTTP listening on :" << http_port << "\n";
    srv.listen("0.0.0.0", http_port);
    // Printed here (after listen() returns from on_signal()'s srv.stop(),
    // before `discovery` destructs below) so a real shutdown failure can be
    // distinguished from "the signal was never delivered/handled at all" —
    // the latter would mean this line, and everything after it including
    // discovery's deregistering destructor, never runs.
    std::cout << "[dns_node] shutting down, deregistering\n";
    return 0;
}
