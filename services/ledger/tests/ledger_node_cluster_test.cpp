// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger_node daemon end to end (.kiro/specs/verifiable-ledger/ task
// 7c): three processes on loopback, Raft over mutual TLS, the HTTPS API
// with client certificates, RocksDB under each. A ledger is created and
// written through the C++ client, `ledger_ctl verify --api` checks it,
// writes go on with a member killed, and everything survives the whole
// cluster restarting.

#define BOOST_TEST_MODULE ledger_node_cluster_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_client.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/private_file.hpp>

#include "test_ports.hpp"

#include <httplib.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration)

using namespace kythira::ledger;

namespace {

constexpr std::string_view admin_id = "spiffe://kythira/ledger/admin";
constexpr int k_members = 3;

auto slurp(const std::filesystem::path& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The cluster's certificates: one CA, a node certificate for localhost
/// that serves and dials (API and Raft), and an admin client.
struct pki {
    std::filesystem::path dir;
    std::string ca;
    std::string node_cert;
    std::string node_key;
    std::string admin_cert;
    std::string admin_key;

    pki() {
        dir = raft::make_private_temp_directory(std::filesystem::temp_directory_path(),
                                                "ledger-node-cluster-");
        raft::testing::certificate_authority authority;
        auto write = [&](const std::string& name, const std::string& pem) {
            const auto p = (dir / name).string();
            raft::write_private_file(p, pem);
            return p;
        };
        ca = write("ca.pem", authority.root_certificate_pem());
        raft::testing::leaf_certificate_options node;
        node.subject.common_name = "localhost";
        node.dns_names = {"localhost"};
        node.ip_addresses = {"127.0.0.1"};
        node.server_auth = true;
        node.client_auth = true;
        const auto n = authority.issue(node);
        node_cert = write("node.pem", n.chain_pem);
        node_key = write("node.key", n.private_key_pem);
        raft::testing::leaf_certificate_options admin;
        admin.subject.common_name = "admin";
        admin.uri_names = {std::string(admin_id)};
        admin.server_auth = false;
        admin.client_auth = true;
        const auto a = authority.issue(admin);
        admin_cert = write("admin.pem", a.chain_pem);
        admin_key = write("admin.key", a.private_key_pem);
    }
    ~pki() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    pki(const pki&) = delete;
    auto operator=(const pki&) -> pki& = delete;
    pki(pki&&) = delete;
    auto operator=(pki&&) -> pki& = delete;
};

/// One ledger_node process. fork + PR_SET_PDEATHSIG + execve, as
/// tests/ca_cluster_node_harness.hpp does, so a dying test never leaves a
/// node holding ctest's pipes.
class node_process {
public:
    node_process(int id, std::vector<std::string> env, std::filesystem::path log)
        : id_(id), env_(std::move(env)), log_(std::move(log)) {}
    ~node_process() { stop(); }
    node_process(const node_process&) = delete;
    auto operator=(const node_process&) -> node_process& = delete;
    node_process(node_process&&) = delete;
    auto operator=(node_process&&) -> node_process& = delete;

    auto start() -> void {
        BOOST_REQUIRE(pid_ <= 0);
        std::vector<std::string> env_strs;
        for (char** e = environ; *e != nullptr; ++e) {
            if (std::strncmp(*e, "KYTHIRA_", 8) != 0) {
                env_strs.emplace_back(*e);
            }
        }
        env_strs.insert(env_strs.end(), env_.begin(), env_.end());
        std::vector<char*> envp;
        for (auto& s : env_strs) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);
        std::string path = KYTHIRA_LEDGER_NODE;
        std::vector<char*> argv{path.data(), nullptr};
        const auto log = log_.string();
        const pid_t parent = getpid();
        pid_ = fork();
        BOOST_REQUIRE_MESSAGE(pid_ >= 0, "fork failed: " << std::strerror(errno));
        if (pid_ == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) {
                _exit(1);
            }
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (fd >= 0) {
                ::dup2(fd, STDOUT_FILENO);
                ::dup2(fd, STDERR_FILENO);
                ::close(fd);
            }
            ::execve(path.c_str(), argv.data(), envp.data());
            _exit(127);
        }
    }

    /// SIGTERM, then SIGKILL if it has not exited within 30 s.
    auto stop() -> void {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        if (!wait_exit(std::chrono::seconds{30})) {
            BOOST_ERROR("node " << id_ << " did not exit within 30 s of SIGTERM");
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
        } else {
            BOOST_TEST(exit_status_ == 0, "node " << id_ << " exit status " << exit_status_ << ":\n"
                                                  << log_text());
        }
        pid_ = -1;
    }

    auto kill_hard() -> void {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    /// The node printed its ready line since `from` bytes of its log.
    [[nodiscard]] auto ready(std::size_t from = 0) const -> bool {
        const auto text = slurp(log_);
        return text.size() > from && text.find(" ready: ", from) != std::string::npos;
    }
    [[nodiscard]] auto running() -> bool {
        if (pid_ <= 0) {
            return false;
        }
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return false;
        }
        return true;
    }
    [[nodiscard]] auto log_size() const -> std::size_t { return slurp(log_).size(); }
    [[nodiscard]] auto log_text() const -> std::string { return slurp(log_); }

private:
    auto wait_exit(std::chrono::seconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                exit_status_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    int id_;
    std::vector<std::string> env_;
    std::filesystem::path log_;
    pid_t pid_ = -1;
    int exit_status_ = 0;
};

class cluster {
public:
    cluster() {
        std::vector<std::uint16_t> raft_ports;
        for (int i = 0; i < k_members; ++i) {
            raft_ports.push_back(kythira::testing::free_port());
            api_ports_.push_back(kythira::testing::free_port());
        }
        metrics_port_ = kythira::testing::free_port();
        std::string peers;
        std::string api_urls;
        for (int i = 0; i < k_members; ++i) {
            const auto id = std::to_string(i + 1);
            peers +=
                (i == 0 ? "" : ",") + id + "=https://localhost:" + std::to_string(raft_ports[i]);
            api_urls += (i == 0 ? "" : ",") + id + "=" + url(i);
        }
        const auto roles = pki_.dir / "roles.txt";
        raft::write_private_file(roles.string(),
                                 "# test roles\n" + std::string(admin_id) + " * admin\n");
        for (int i = 0; i < k_members; ++i) {
            const auto data = pki_.dir / ("node-" + std::to_string(i + 1));
            std::vector<std::string> env{
                "KYTHIRA_NODE_ID=" + std::to_string(i + 1),
                "KYTHIRA_PEERS=" + peers,
                "KYTHIRA_RAFT_BIND=127.0.0.1",
                "KYTHIRA_RAFT_PORT=" + std::to_string(raft_ports[i]),
                "KYTHIRA_LEDGER_API_LISTEN=127.0.0.1:" + std::to_string(api_ports_[i]),
                "KYTHIRA_LEDGER_API_URLS=" + api_urls,
                "KYTHIRA_LEDGER_DATA_DIR=" + data.string(),
                "KYTHIRA_LEDGER_TLS_CERT=" + pki_.node_cert,
                "KYTHIRA_LEDGER_TLS_KEY=" + pki_.node_key,
                "KYTHIRA_LEDGER_CLIENT_CA=" + pki_.ca,
                "KYTHIRA_LEDGER_ROLES_FILE=" + roles.string(),
                "KYTHIRA_LEDGER_SIGNING_KEY=" + pki_.node_key,
                "KYTHIRA_LEDGER_SIGNING_CHAIN=" + pki_.node_cert,
                "KYTHIRA_LEDGER_MAINTENANCE_INTERVAL_MS=100",
                "KYTHIRA_LOG_LEVEL=warning",
            };
            if (i == 0) {
                env.push_back("KYTHIRA_LEDGER_METRICS_LISTEN=127.0.0.1:" +
                              std::to_string(metrics_port_));
                env.emplace_back("KYTHIRA_LEDGER_METRICS_INTERVAL_MS=200");
            }
            nodes_.push_back(std::make_unique<node_process>(
                i + 1, std::move(env), pki_.dir / ("node-" + std::to_string(i + 1) + ".log")));
        }
    }
    ~cluster() {
        for (auto& n : nodes_) {
            n->stop();
        }
    }
    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    [[nodiscard]] auto url(int i) const -> std::string {
        return "https://localhost:" + std::to_string(api_ports_[static_cast<std::size_t>(i)]);
    }

    auto start(int i) -> void {
        auto& n = *nodes_[static_cast<std::size_t>(i)];
        const auto from = n.log_size();
        n.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        while (!n.ready(from)) {
            BOOST_REQUIRE_MESSAGE(n.running(), "node " << i + 1 << " exited:\n" << n.log_text());
            BOOST_REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline,
                                  "node " << i + 1 << " not ready:\n"
                                          << n.log_text());
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
    auto start_all() -> void {
        for (int i = 0; i < k_members; ++i) {
            start(i);
        }
    }
    auto stop_all() -> void {
        for (auto& n : nodes_) {
            n->stop();
        }
    }
    [[nodiscard]] auto data_dir(int i) const -> std::filesystem::path {
        return pki_.dir / ("node-" + std::to_string(i + 1));
    }
    [[nodiscard]] auto node(int i) -> node_process& { return *nodes_[static_cast<std::size_t>(i)]; }
    [[nodiscard]] auto certs() const -> const pki& { return pki_; }

    [[nodiscard]] auto client(std::vector<int> members = {0, 1, 2}) const -> ledger_client {
        https_transport_options t;
        t.ca_path = pki_.ca;
        t.cert_path = pki_.admin_cert;
        t.key_path = pki_.admin_key;
        t.timeout = std::chrono::seconds{10};
        ledger_client_options o;
        for (const int m : members) {
            o.endpoints.push_back(url(m));
        }
        o.max_redirects = 8;
        return {o, https_transport(t)};
    }

    /// Node 1's Prometheus endpoint, or "" if it does not answer.
    [[nodiscard]] auto scrape() const -> std::string {
        httplib::Client client("127.0.0.1", metrics_port_);
        client.set_connection_timeout(std::chrono::seconds{2});
        const auto res = client.Get("/metrics");
        return res && res->status == 200 ? res->body : std::string();
    }

private:
    pki pki_;
    std::uint16_t metrics_port_ = 0;
    std::vector<std::uint16_t> api_ports_;
    std::vector<std::unique_ptr<node_process>> nodes_;
};

/// Runs `fn` until it returns without a retryable error: no leader yet, a
/// member down, or a commit that could not be confirmed (the callers
/// write idempotently, so resending is safe).
template<typename Fn>
auto eventually(Fn&& fn, std::chrono::seconds budget = std::chrono::seconds{60}) -> decltype(fn()) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        try {
            return fn();
        } catch (const ledger_client_error& e) {
            const bool retry = e.code() == "not_leader" || e.code() == "commit_unconfirmed" ||
                               e.code() == "ledger_not_found" || e.retryable();
            if (!retry || std::chrono::steady_clock::now() > deadline) {
                throw;
            }
        } catch (const ledger_transport_error&) {
            if (std::chrono::steady_clock::now() > deadline) {
                throw;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
}

auto account(std::string_view owner, std::int64_t balance) -> ion_value {
    ion_value v = ion_value::make_struct();
    v.set("owner", ion_value::string(owner)).set("balance", ion_value::integer(balance));
    return v;
}

/// A document id (22 base62 characters) for a short name, zero-padded on
/// the left so "a1" and "a10" stay distinct.
auto doc_id(std::string_view name) -> std::string {
    return std::string(id_length - name.size(), '0') + std::string(name);
}

/// Insert a document, accepting that an earlier unconfirmed attempt wrote it.
auto insert(ledger_client& c, const std::string& id, std::int64_t balance) -> void {
    eventually([&] {
        try {
            c.execute("bank", {statements::insert("acct", doc_id(id), account(id, balance))});
        } catch (const ledger_client_error& e) {
            if (e.code() != "document_exists") {
                throw;
            }
        }
        return 0;
    });
}

auto balance_of(ledger_client& c, const std::string& id) -> std::optional<std::int64_t> {
    return eventually([&]() -> std::optional<std::int64_t> {
        const auto doc = c.get_document("bank", "acct", doc_id(id));
        if (!doc) {
            return std::nullopt;
        }
        const ion_value* data = doc->find("data");
        const ion_value* b = data != nullptr ? data->find("balance") : nullptr;
        return b != nullptr ? b->as_int64() : std::nullopt;
    });
}

/// N from ledger_ctl's "verified N blocks of ledger bank", or -1.
auto verified_blocks(const std::string& out) -> int {
    constexpr std::string_view prefix = "verified ";
    const auto at = out.find(prefix);
    if (at == std::string::npos || out.find(" blocks of ledger bank", at) == std::string::npos) {
        return -1;
    }
    try {
        return std::stoi(out.substr(at + prefix.size()));
    } catch (const std::exception&) {
        return -1;
    }
}

/// ledger_ctl verify --data-dir on member i's data directory.
auto ledger_ctl_verify_dir(const cluster& c, int i, const std::filesystem::path& log) -> int {
    const std::string cmd = std::string(KYTHIRA_LEDGER_CTL) + " verify --data-dir " +
                            c.data_dir(i).string() + " --ledger bank >" + log.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

auto ledger_ctl_verify(const cluster& c, const std::filesystem::path& log) -> int {
    const auto& p = c.certs();
    const std::string cmd = std::string(KYTHIRA_LEDGER_CTL) + " verify --api " + c.url(0) + "," +
                            c.url(1) + "," + c.url(2) + " --ledger bank --tls-ca " + p.ca +
                            " --cert " + p.admin_cert + " --key " + p.admin_key + " --ca " + p.ca +
                            " >" + log.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_three_node_cluster_serves_verifies_and_survives_restarts) {
    cluster c;
    c.start_all();
    auto client = c.client();

    // Create the ledger and its table; placement and the group's first
    // election take a moment.
    eventually([&] {
        try {
            client.create_ledger("bank", false);
        } catch (const ledger_client_error& e) {
            if (e.code() != "ledger_exists") {
                throw;
            }
        }
        return 0;
    });
    eventually([&] {
        try {
            client.execute("bank", {statements::create_table("acct")});
        } catch (const ledger_client_error& e) {
            if (e.code() != "table_exists") {
                throw;
            }
        }
        return 0;
    });
    for (int i = 0; i < 10; ++i) {
        insert(client, "a" + std::to_string(i), i * 10);
    }
    BOOST_TEST(balance_of(client, "a7").value_or(-1) == 70);

    // The journal re-hashes to the cluster's signed digest.
    const auto log = c.certs().dir / "ledger_ctl.log";
    BOOST_TEST(ledger_ctl_verify(c, log) == 0, slurp(log));
    // The table and ten inserts; no write has been retried yet.
    BOOST_TEST(verified_blocks(slurp(log)) == 11, slurp(log));
    // ...and so does a running member's data directory, read beside it.
    BOOST_TEST(ledger_ctl_verify_dir(c, 1, log) == 0, slurp(log));
    BOOST_TEST(verified_blocks(slurp(log)) == 11, slurp(log));

    // Node 1 exports the ledger's gauges: the journal, and the bytes not
    // yet sealed into a segment.
    const auto has_metric = [&](const std::string& line) {
        return c.scrape().find(line) != std::string::npos;
    };
    for (int i = 0; i < 100 && !has_metric(R"(kythira_ledger_blocks{ledger="bank"} 11)"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    const auto scraped = c.scrape();
    BOOST_TEST(scraped.find(R"(kythira_ledger_blocks{ledger="bank"} 11)") != std::string::npos,
               scraped);
    BOOST_TEST(scraped.find(R"(kythira_ledger_journal_bytes{ledger="bank",where="unsealed"})") !=
                   std::string::npos,
               scraped);
    BOOST_TEST(scraped.find(R"(kythira_ledger_leader{ledger="bank"})") != std::string::npos,
               scraped);

    // Writes go on with any one member killed, and it catches up after.
    c.node(2).kill_hard();
    auto two = c.client({0, 1});
    for (int i = 10; i < 15; ++i) {
        insert(two, "a" + std::to_string(i), i * 10);
    }
    BOOST_TEST(balance_of(two, "a14").value_or(-1) == 140);
    c.start(2);
    c.node(0).kill_hard();
    auto others = c.client({1, 2});
    insert(others, "b0", 5);
    BOOST_TEST(balance_of(others, "a12").value_or(-1) == 120);
    c.start(0);

    // Everything survives the whole cluster stopping and starting again:
    // the catalog, the ledger's documents and its journal.
    c.stop_all();
    c.start_all();
    auto again = c.client();
    BOOST_TEST(balance_of(again, "a3").value_or(-1) == 30);
    BOOST_TEST(balance_of(again, "a14").value_or(-1) == 140);
    BOOST_TEST(balance_of(again, "b0").value_or(-1) == 5);
    insert(again, "c0", 1);
    BOOST_TEST(ledger_ctl_verify(c, log) == 0, slurp(log));
    // Seven more inserts. A resent insert that finds its document already
    // there is rejected, and a rejection makes no block.
    BOOST_TEST(verified_blocks(slurp(log)) == 18, slurp(log));

    // A stopped member's data directory verifies offline, including the
    // blocks it caught up on after being killed.
    c.stop_all();
    BOOST_TEST(ledger_ctl_verify_dir(c, 2, log) == 0, slurp(log));
    BOOST_TEST(verified_blocks(slurp(log)) == 18, slurp(log));
}
