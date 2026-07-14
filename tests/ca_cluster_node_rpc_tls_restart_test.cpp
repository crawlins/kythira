// **Feature: ca-cluster-rpc-mtls, Property 5: A Restarted, Already-Cutover
// Node Needs No Bootstrap Credential**
// Cuts over a 3-node cluster (bootstrap credential only), deletes the
// bootstrap credential entirely, restarts one node with NEITHER
// --rpc-tls-cert nor --rpc-tls-key given at all, and confirms it still
// rejoins the cluster and the cluster keeps issuing certificates — proving
// the persisted peer certificate under --data-dir (Requirement 7.1) is
// sufficient on its own.
// **Validates: Requirement 7.1**

#define BOOST_TEST_MODULE ca_cluster_node_rpc_tls_restart_test

#include <boost/test/unit_test.hpp>

#include <raft/ca_bootstrap_client.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef CA_CLUSTER_NODE_PATH
#define CA_CLUSTER_NODE_PATH "ca_cluster_node"
#endif

extern char** environ;

using namespace raft::testing;

namespace {

auto find_free_port() -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

struct rpc_tls_node_process {
    pid_t pid{-1};
    std::uint64_t node_id;
    int http_port;
    int rpc_port;
    std::string data_dir;
    std::string unseal_key_file;
    std::string auth_token;
    std::string peers_arg;
    std::optional<std::string> bootstrap_cert_path;  // nullopt: no --rpc-tls-cert/key at all
    std::optional<std::string> bootstrap_key_path;
    bool bootstrap;

    rpc_tls_node_process(std::uint64_t id, int rpc_port_, int http_port_, std::string data_dir_,
                         std::string unseal_key_file_, std::string auth_token_,
                         std::string peers_arg_, std::optional<std::string> bootstrap_cert_path_,
                         std::optional<std::string> bootstrap_key_path_, bool bootstrap_)
        : node_id(id),
          http_port(http_port_),
          rpc_port(rpc_port_),
          data_dir(std::move(data_dir_)),
          unseal_key_file(std::move(unseal_key_file_)),
          auth_token(std::move(auth_token_)),
          peers_arg(std::move(peers_arg_)),
          bootstrap_cert_path(std::move(bootstrap_cert_path_)),
          bootstrap_key_path(std::move(bootstrap_key_path_)),
          bootstrap(bootstrap_) {
        std::filesystem::create_directories(data_dir);
        spawn();
    }

    ~rpc_tls_node_process() { stop(); }

    rpc_tls_node_process(const rpc_tls_node_process&) = delete;
    auto operator=(const rpc_tls_node_process&) -> rpc_tls_node_process& = delete;

    auto spawn() -> void {
        std::vector<std::string> argv_strs = {
            CA_CLUSTER_NODE_PATH,
            "--node-id",
            std::to_string(node_id),
            "--rpc-port",
            std::to_string(rpc_port),
            "--http-port",
            std::to_string(http_port),
            "--data-dir",
            data_dir,
            "--unseal-key-file",
            unseal_key_file,
            "--auth-token",
            auth_token,
            // See ca_cluster_node_rpc_tls_test.cpp's identical comment: RPC
            // TLS's per-call full handshake needs more timeout headroom
            // than the plain-TCP default under real host contention.
            // Every RPC call under RPC TLS pays a full TLS handshake
            // (asymmetric crypto, no session reuse across the per-call-
            // connect transport model — see tls_tcp_rpc.hpp's design
            // comment) on top of a new OS thread per connection on both
            // ends. Confirmed during this spec's implementation: even
            // 600/1200/200/2000ms (election-min/max/heartbeat/rpc) was
            // insufficient to avoid cascading re-elections under this
            // repository's own shared-host CI/dev contention once a
            // second concurrent RPC-heavy operation (record_rpc_tls_ready's
            // submit_command) overlaps with routine heartbeat traffic.
            // These much larger values trade test wall-clock time (still
            // comfortably inside this file's TIMEOUT) for headroom that
            // holds regardless of host load — this test asserts eventual
            // functional convergence, not latency.
            "--election-timeout-min-ms",
            "3000",
            "--election-timeout-max-ms",
            "5000",
            "--heartbeat-interval-ms",
            "500",
            "--rpc-timeout-ms",
            "5000",
        };
        if (!peers_arg.empty()) {
            argv_strs.emplace_back("--peers");
            argv_strs.push_back(peers_arg);
        }
        if (bootstrap) argv_strs.emplace_back("--bootstrap-ca");
        if (bootstrap_cert_path.has_value()) {
            argv_strs.emplace_back("--rpc-tls-cert");
            argv_strs.push_back(*bootstrap_cert_path);
            argv_strs.emplace_back("--rpc-tls-key");
            argv_strs.push_back(*bootstrap_key_path);
        }

        std::vector<char*> argv;
        argv.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) argv.push_back(s.data());
        argv.push_back(nullptr);

        int rc = posix_spawn(&pid, CA_CLUSTER_NODE_PATH, nullptr, nullptr, argv.data(), environ);
        BOOST_REQUIRE_MESSAGE(rc == 0,
                              "posix_spawn(ca_cluster_node) failed: " << std::strerror(rc));
    }

    auto stop() -> void {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    // Restarts against the SAME data_dir/ports — simulates a process
    // crash-and-restart. `new_bootstrap_cert_path`/`new_bootstrap_key_path`
    // let a restart change (or entirely drop) the --rpc-tls-cert/key flags,
    // the whole point of this test file.
    auto restart(std::optional<std::string> new_bootstrap_cert_path,
                 std::optional<std::string> new_bootstrap_key_path) -> void {
        BOOST_REQUIRE(pid <= 0);
        bootstrap = false;
        bootstrap_cert_path = std::move(new_bootstrap_cert_path);
        bootstrap_key_path = std::move(new_bootstrap_key_path);
        spawn();
    }

    [[nodiscard]] auto is_running() const -> bool { return pid > 0; }
};

auto wait_healthy(int http_port, std::chrono::seconds timeout) -> bool {
    httplib::Client c("127.0.0.1", http_port);
    c.set_connection_timeout(1, 0);
    c.set_read_timeout(10, 0);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto res = c.Get("/healthz");
        if (res && res->status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

auto find_leader_index(const std::vector<std::unique_ptr<rpc_tls_node_process>>& nodes,
                       const std::string& auth_token, std::chrono::seconds timeout)
    -> std::optional<std::size_t> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i]->is_running()) continue;
            httplib::Client c("127.0.0.1", nodes[i]->http_port);
            c.set_connection_timeout(1, 0);
            c.set_read_timeout(10, 0);
            auto res = c.Get("/v1/root-ca", {{"Authorization", "Bearer " + auth_token}});
            if (res && res->status == 200 && !res->body.empty()) return i;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return std::nullopt;
}

auto post_with_retry_on_not_ready(httplib::Client& client, const std::string& path,
                                  const httplib::Headers& headers, const std::string& body,
                                  std::chrono::seconds timeout) -> httplib::Result {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    httplib::Result res;
    do {
        res = client.Post(path, headers, body, "application/json");
        if (res && res->status != 503 && res->status != 502) return res;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } while (std::chrono::steady_clock::now() < deadline);
    return res;
}

auto try_issue_certificate(const std::vector<std::unique_ptr<rpc_tls_node_process>>& nodes,
                           const std::string& auth_token, std::chrono::seconds timeout) -> bool {
    auto leader_index = find_leader_index(nodes, auth_token, timeout);
    if (!leader_index.has_value()) return false;

    leaf_certificate_options opts;
    opts.subject.common_name = "rpc-tls-restart-test-client";
    opts.dns_names = {"rpc-tls-restart-test-client.example.com"};
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] =
        boost::json::array{boost::json::string("rpc-tls-restart-test-client.example.com")};

    httplib::Client client("127.0.0.1", nodes[*leader_index]->http_port);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(65, 0);
    auto res = post_with_retry_on_not_ready(client, "/v1/certificates",
                                            {{"Authorization", "Bearer " + auth_token}},
                                            boost::json::serialize(body), timeout);
    return res && res->status == 200;
}

constexpr const char* k_auth_token = "rpc-tls-restart-test-token";

}  // namespace

BOOST_AUTO_TEST_CASE(restarted_node_rejoins_without_bootstrap_credential,
                     *boost::unit_test::timeout(550)) {
    auto tmp_root = (std::filesystem::temp_directory_path() /
                     ("ca_cluster_node_rpc_tls_restart_test_" + std::to_string(::getpid())))
                        .string();
    std::filesystem::create_directories(tmp_root);
    std::string unseal_key_file = tmp_root + "/unseal.key";
    std::ofstream(unseal_key_file) << "rpc-tls-restart-test-unseal-passphrase\n";

    certificate_authority bootstrap_cred;
    std::string bootstrap_cert_path = tmp_root + "/bootstrap.crt";
    std::string bootstrap_key_path = tmp_root + "/bootstrap.key";
    std::ofstream(bootstrap_cert_path) << bootstrap_cred.root_certificate_pem();
    std::ofstream(bootstrap_key_path)
        << detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred);

    struct info {
        std::uint64_t id;
        int rpc_port;
        int http_port;
    };
    std::vector<info> infos = {{1, find_free_port(), find_free_port()},
                               {2, find_free_port(), find_free_port()},
                               {3, find_free_port(), find_free_port()}};

    std::ostringstream peers;
    for (std::size_t i = 0; i < infos.size(); ++i) {
        if (i > 0) peers << ",";
        peers << infos[i].id << ":127.0.0.1:" << infos[i].rpc_port
              << "@http://127.0.0.1:" << infos[i].http_port;
    }
    std::string peers_arg = peers.str();

    std::vector<std::unique_ptr<rpc_tls_node_process>> nodes;
    for (std::size_t i = 0; i < infos.size(); ++i) {
        nodes.push_back(std::make_unique<rpc_tls_node_process>(
            infos[i].id, infos[i].rpc_port, infos[i].http_port,
            tmp_root + "/node" + std::to_string(infos[i].id), unseal_key_file, k_auth_token,
            peers_arg, bootstrap_cert_path, bootstrap_key_path, /*bootstrap=*/i == 0));
        if (i == 0) {
            // Proportional to this file's much-larger-than-default
            // election timeout (3000-5000ms) — must comfortably exceed
            // it so the --bootstrap-ca node's own election timer always
            // fires first (Requirement 17.10's documented operational
            // practice), not a race against the other node's timer.
            std::this_thread::sleep_for(std::chrono::milliseconds(6000));
        }
    }
    BOOST_REQUIRE_MESSAGE(
        std::all_of(
            nodes.begin(), nodes.end(),
            [](const auto& n) { return wait_healthy(n->http_port, std::chrono::seconds(60)); }),
        "not every node became healthy");

    // Wait for the cluster to reach a stable, issuance-capable state
    // (bootstrap_ca committed, at least this far along toward cutover).
    bool ready = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (try_issue_certificate(nodes, k_auth_token, std::chrono::seconds(10))) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    BOOST_REQUIRE_MESSAGE(ready, "cluster never reached an issuance-capable state");

    // Pick a non-leader node to restart (restarting the leader would also
    // trigger a failover, an orthogonal concern already covered by
    // tests/ca_cluster_node_test.cpp's own failover property).
    auto leader_index = find_leader_index(nodes, k_auth_token, std::chrono::seconds(30));
    BOOST_REQUIRE(leader_index.has_value());
    std::size_t restart_index = (*leader_index + 1) % nodes.size();

    // Poll for the maintenance threads to reach full cutover on all three
    // nodes (each node's own persisted peer cert file under --data-dir is
    // what this test actually depends on existing) — a fixed sleep here
    // previously raced main.cpp's own k_identity_acquire_grace (a 3-second
    // minimum delay, added after ca-cluster-rpc-mtls's CI-only deadlock
    // fix, between a node first observing the CA root and that node
    // switching its presented identity): under CI contention this test's
    // own fixed wait could elapse before that grace period even finished,
    // let alone before real CSR generation/signing/persisting completed
    // afterward. Polling avoids needing to keep two independently-tuned
    // timing constants in sync by hand.
    auto peer_cert_path = std::filesystem::path(tmp_root) /
                          ("node" + std::to_string(nodes[restart_index]->node_id)) /
                          "rpc_peer_cert.pem";
    bool cutover_complete = false;
    auto cutover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < cutover_deadline) {
        if (std::filesystem::exists(peer_cert_path)) {
            cutover_complete = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // Confirm the persisted peer certificate actually exists before relying
    // on it — otherwise a false pass here would just mean "plain TCP-like
    // behavior happened to still work," not "Property 5 held."
    BOOST_REQUIRE_MESSAGE(cutover_complete,
                          "no persisted RPC peer certificate found before restart — "
                          "cutover likely hadn't happened yet");

    nodes[restart_index]->stop();

    // Delete the bootstrap credential entirely before restarting — the
    // restarted node must not need it.
    std::filesystem::remove(bootstrap_cert_path);
    std::filesystem::remove(bootstrap_key_path);

    nodes[restart_index]->restart(std::nullopt, std::nullopt);
    BOOST_REQUIRE_MESSAGE(wait_healthy(nodes[restart_index]->http_port, std::chrono::seconds(60)),
                          "restarted node (no --rpc-tls-cert/--rpc-tls-key) never became healthy");

    // The cluster as a whole must still be able to issue a certificate,
    // proving the restarted node rejoined the Raft cluster over RPC TLS
    // using only its persisted peer certificate.
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(nodes, k_auth_token, std::chrono::seconds(60)),
        "certificate issuance failed after restarting a node without the bootstrap credential");

    for (auto& n : nodes) n->stop();
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
}
