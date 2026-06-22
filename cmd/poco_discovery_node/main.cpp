// HTTP node for the poco_peer_discovery Docker integration test.
//
// Reads config from environment variables, registers with poco_peer_discovery,
// and serves a minimal HTTP control plane:
//   GET /health   → 200 "OK"
//   GET /peers    → JSON array [{id,address}, ...]  (queries DNS-SD each call)
//
// Environment variables:
//   NODE_ID            unique service name (default: "node1")
//   HTTP_PORT          control-plane port   (default: 9011)
//   RPC_PORT           port advertised in DNS-SD (default: 7011)
//   FRESHNESS_INTERVAL freshness in seconds (default: 600)
//   SERVICE_TYPE       DNS-SD type string   (default: "_raft._tcp")

#include <raft/poco_peer_discovery.hpp>

#include <httplib.h>
#include <folly/init/Init.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

#ifndef KYTHIRA_HAS_POCO_DNSSD
#error "poco_discovery_node requires KYTHIRA_HAS_POCO_DNSSD — build with Poco DNSSD enabled"
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

}  // namespace

int main(int argc, char** argv) {
    folly::Init folly_init(&argc, &argv);

    const std::string node_id = env_or("NODE_ID", "node1");
    const int http_port = std::stoi(env_or("HTTP_PORT", "9011"));
    const int rpc_port = std::stoi(env_or("RPC_PORT", "7011"));
    const int freshness = std::stoi(env_or("FRESHNESS_INTERVAL", "600"));
    const std::string svc_type = env_or("SERVICE_TYPE", "_raft._tcp");

    char host_buf[256] = {};
    gethostname(host_buf, sizeof(host_buf) - 1);
    const std::string self_addr = std::string(host_buf) + ":" + std::to_string(rpc_port);

    std::cout << "[discovery_node] id=" << node_id << " addr=" << self_addr
              << " service=" << svc_type << " freshness=" << freshness << "s\n";

    kythira::poco_peer_discovery::config cfg;
    cfg.service_type = svc_type;
    cfg.freshness_interval = std::chrono::seconds{freshness};

    kythira::poco_peer_discovery discovery{std::move(cfg)};

    try {
        discovery.register_node(node_id, self_addr).get();
        std::cout << "[discovery_node] registered\n";
    } catch (const std::exception& ex) {
        std::cerr << "[discovery_node] registration failed: " << ex.what() << "\n";
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

    std::cout << "[discovery_node] HTTP listening on :" << http_port << "\n";
    srv.listen("0.0.0.0", http_port);
    return 0;
}
