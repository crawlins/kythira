// HTTP node for the rfc2136_dns_sd_discovery Docker integration test.
//
// Registers PTR + SRV + TXT (freshness) records under cluster.example.local
// via RFC 2136 dynamic update, then serves:
//   GET /health  → 200 "OK"
//   GET /peers   → JSON array [{id,address}, ...]
//
// Killing this node with SIGKILL prevents clean deregistration; surviving
// nodes detect the dead node via the TXT fresh_until expiry.
//
// Environment variables:
//   NODE_ID             service instance name (default: "node1")
//   HTTP_PORT           control-plane port    (default: 9031)
//   RPC_PORT            advertised in SRV     (default: 7031)
//   DNS_SERVER          BIND9 IP (required)
//   DNS_ZONE            zone name             (default: "example.local.")
//   SERVICE_DOMAIN      service base domain   (default: "cluster.example.local.")
//   SERVICE_TYPE        DNS-SD type           (default: "_kythira-test._tcp")
//   FRESHNESS_INTERVAL  seconds               (default: 20)

// httplib must precede ldns headers: ldns redefines bool as _Bool in C++ TUs.
#include <httplib.h>

#include <raft/rfc2136_dns_sd_discovery.hpp>

#include <folly/init/Init.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#ifndef KYTHIRA_HAS_LDNS
#error "dns_sd_discovery_node requires KYTHIRA_HAS_LDNS — build with libldns installed"
#endif

namespace {

httplib::Server* g_srv = nullptr;

void on_signal(int) {
    if (g_srv) g_srv->stop();
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

const char* env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    folly::Init folly_init(&argc, &argv);

    const std::string node_id = env_or("NODE_ID", "node1");
    const int http_port = std::stoi(env_or("HTTP_PORT", "9031"));
    const int rpc_port = std::stoi(env_or("RPC_PORT", "7031"));
    const std::string dns_server = env_or("DNS_SERVER", "");
    const std::string dns_zone = env_or("DNS_ZONE", "example.local.");
    const std::string svc_domain = env_or("SERVICE_DOMAIN", "cluster.example.local.");
    const std::string svc_type = env_or("SERVICE_TYPE", "_kythira-test._tcp");
    const int freshness = std::stoi(env_or("FRESHNESS_INTERVAL", "20"));

    if (dns_server.empty()) {
        std::cerr << "[dns_sd_node] DNS_SERVER env var is required\n";
        return 1;
    }

    char host_buf[256] = {};
    gethostname(host_buf, sizeof(host_buf) - 1);
    const std::string self_addr = std::string(host_buf) + ":" + std::to_string(rpc_port);

    std::cout << "[dns_sd_node] id=" << node_id << " addr=" << self_addr << " server=" << dns_server
              << " domain=" << svc_domain << " type=" << svc_type << " freshness=" << freshness
              << "s\n";

    kythira::rfc2136_dns_sd_discovery::config cfg;
    cfg.server = resolve_to_ip(dns_server);
    cfg.port = 53;
    cfg.zone = dns_zone;
    cfg.service_domain = svc_domain;
    cfg.service_type = svc_type;
    cfg.ttl = 30;
    cfg.freshness_interval = std::chrono::seconds{freshness};

    kythira::rfc2136_dns_sd_discovery discovery{std::move(cfg)};

    try {
        discovery.register_node(node_id, self_addr).get();
        std::cout << "[dns_sd_node] registered\n";
    } catch (const std::exception& ex) {
        std::cerr << "[dns_sd_node] registration failed: " << ex.what() << "\n";
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

    std::cout << "[dns_sd_node] HTTP listening on :" << http_port << "\n";
    srv.listen("0.0.0.0", http_port);
    return 0;
}
