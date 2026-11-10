// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger under kills and partitions, in containers
// (.kiro/specs/verifiable-ledger/ task 11, Requirement 12.5).
// services/ledger/docker/ledger-chaos-compose.yml starts three ledger_node
// members addressed by service name, with Raft over mutual TLS and the
// HTTPS API published on 1652<n>.
//
// Writers insert documents and replace a counter document each through the
// C++ client while the test SIGKILLs members and starts them again (one at
// a time, two at once so the ledger loses its quorum, then all three), and
// cuts each member off the network in turn. Afterwards:
//
//   * every acknowledged insert is there with its value, and every counter
//     holds its last acknowledged value or a later attempted one;
//   * `ledger_ctl verify --api` re-hashes the whole journal through the
//     API to the cluster's signed digest;
//   * on every member, `ledger_ctl verify --data-dir` re-hashes that
//     member's own copy of the journal (blocks, RocksDB state and sealed
//     segment files) to the same signed digest, and all three hold the
//     same number of blocks. So the replicas' journals are identical, not
//     just individually consistent.

#define BOOST_TEST_MODULE ledger_chaos_test
#include <boost/test/results_collector.hpp>
#include <boost/test/tree/observer.hpp>
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_client.hpp>

#include <raft/certificate_authority.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace std::chrono_literals;
using namespace kythira::ledger;
namespace os = docker_chaos::os;

namespace {

constexpr std::array<std::uint16_t, 3> k_api_ports{16521, 16522, 16523};
constexpr std::array<const char*, 3> k_containers{"ledger-chaos-1", "ledger-chaos-2",
                                                  "ledger-chaos-3"};
constexpr std::array<const char*, 3> k_services{"ledger-1", "ledger-2", "ledger-3"};
constexpr const char* k_network = "kythira_ledger_chaos_net";
constexpr std::string_view k_admin_id = "spiffe://kythira/ledger/admin";
constexpr std::string_view k_ledger = "bank";
constexpr std::size_t k_writers = 4;

std::string compose_file() {
    const char* env = std::getenv("KYTHIRA_LEDGER_CHAOS_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "services/ledger/docker/ledger-chaos-compose.yml";
}

std::vector<std::string> compose_cmd(std::initializer_list<std::string> args) {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", compose_file()});
    cmd.insert(cmd.end(), args);
    return cmd;
}

std::string url(std::size_t i) {
    return "https://localhost:" + std::to_string(k_api_ports[i]);
}

/// A world-readable file: the containers read the PKI as uid 10001, which
/// is neither the file's owner under Docker nor under rootless Podman
/// (where the host user is the container's root). Throwaway test keys.
void write_public_file(const std::filesystem::path& p, const std::string& text) {
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            throw std::runtime_error("cannot write " + p.string());
        }
    }
    std::filesystem::permissions(
        p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
               std::filesystem::perms::group_read | std::filesystem::perms::others_read);
}

/// The cluster's throwaway PKI, mounted into every member at /pki: one
/// CA, one node certificate for every service name (and for localhost,
/// which the test dials through the published ports) that serves and
/// dials, an admin client, and the roles file granting it everything.
struct pki {
    std::filesystem::path dir;
    std::string ca;
    std::string admin_cert;
    std::string admin_key;

    pki() {
        dir = std::filesystem::temp_directory_path() /
              ("kythira-ledger-chaos-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        std::filesystem::permissions(
            dir, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                     std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                     std::filesystem::perms::others_exec);
        raft::testing::certificate_authority authority;
        write_public_file(dir / "ca.pem", authority.root_certificate_pem());
        raft::testing::leaf_certificate_options node;
        node.subject.common_name = "ledger";
        node.dns_names = {"localhost"};
        for (const auto* s : k_services) {
            node.dns_names.emplace_back(s);
        }
        node.ip_addresses = {"127.0.0.1"};
        node.server_auth = true;
        node.client_auth = true;
        const auto n = authority.issue(node);
        write_public_file(dir / "node.pem", n.chain_pem);
        write_public_file(dir / "node.key", n.private_key_pem);
        raft::testing::leaf_certificate_options admin;
        admin.subject.common_name = "admin";
        admin.uri_names = {std::string(k_admin_id)};
        admin.server_auth = false;
        admin.client_auth = true;
        const auto a = authority.issue(admin);
        write_public_file(dir / "admin.pem", a.chain_pem);
        write_public_file(dir / "admin.key", a.private_key_pem);
        write_public_file(dir / "roles.txt",
                          "# ledger chaos scenario\n" + std::string(k_admin_id) + " * admin\n");
        ca = (dir / "ca.pem").string();
        admin_cert = (dir / "admin.pem").string();
        admin_key = (dir / "admin.key").string();
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

pki& certs() {
    static pki p;
    return p;
}

ledger_client make_client(std::chrono::seconds timeout = 10s) {
    https_transport_options t;
    t.ca_path = certs().ca;
    t.cert_path = certs().admin_cert;
    t.key_path = certs().admin_key;
    t.timeout = timeout;
    ledger_client_options o;
    for (std::size_t i = 0; i < k_api_ports.size(); ++i) {
        o.endpoints.push_back(url(i));
    }
    o.max_redirects = 8;
    return {o, https_transport(t)};
}

/// Runs `fn` until it returns without a retryable error (no leader, a
/// member down, a commit that could not be confirmed) or the budget runs
/// out, when the last error is rethrown.
template<typename Fn> auto eventually(Fn&& fn, std::chrono::seconds budget) -> decltype(fn()) {
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
        std::this_thread::sleep_for(250ms);
    }
}

/// A document id (22 base62 characters) for a short name.
std::string doc_id(std::string_view name) {
    return std::string(id_length - name.size(), '0') + std::string(name);
}

ion_value account(std::string_view owner, std::int64_t balance) {
    ion_value v = ion_value::make_struct();
    v.set("owner", ion_value::string(owner)).set("balance", ion_value::integer(balance));
    return v;
}

std::optional<std::int64_t> balance_in(const std::optional<ion_value>& doc) {
    if (!doc) {
        return std::nullopt;
    }
    const ion_value* data = doc->find("data");
    const ion_value* b = data != nullptr ? data->find("balance") : nullptr;
    return b != nullptr ? b->as_int64() : std::nullopt;
}

/// Creates the ledger and its table, accepting that an earlier attempt did.
bool create_ledger_and_table(std::chrono::seconds budget) {
    try {
        auto c = make_client();
        eventually(
            [&] {
                try {
                    c.create_ledger(k_ledger, false);
                } catch (const ledger_client_error& e) {
                    if (e.code() != "ledger_exists") {
                        throw;
                    }
                }
                return 0;
            },
            budget);
        eventually(
            [&] {
                try {
                    c.execute(k_ledger, {statements::create_table("acct")});
                } catch (const ledger_client_error& e) {
                    if (e.code() != "table_exists") {
                        throw;
                    }
                }
                return 0;
            },
            budget);
        return true;
    } catch (const std::exception& e) {
        BOOST_TEST_MESSAGE("not ready: " << e.what());
        return false;
    }
}

/// The ledger answers a read through every member in `nodes`.
bool wait_ready(std::chrono::seconds timeout, std::vector<std::size_t> nodes = {0, 1, 2}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = true;
        for (const auto i : nodes) {
            try {
                https_transport_options t;
                t.ca_path = certs().ca;
                t.cert_path = certs().admin_cert;
                t.key_path = certs().admin_key;
                t.timeout = 5s;
                ledger_client_options o;
                o.endpoints = {url(i)};
                o.max_redirects = 8;
                ledger_client c(o, https_transport(t));
                (void)c.get_document(k_ledger, "acct", doc_id("probe"));
            } catch (const std::exception&) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

std::string node_logs() {
    std::string all;
    for (const auto* c : k_containers) {
        all += "── " + std::string(c) + " ──\n" + os::real_exec(os::docker_logs_cmd(c, 200)).out;
    }
    return all;
}

// A kill that fails means the container was not running: it exited on its
// own, which is the failure worth reporting.
void kill_node(std::size_t i) {
    BOOST_TEST_MESSAGE("SIGKILL " << k_containers[i]);
    const auto r = os::real_exec(os::docker_kill_cmd(k_containers[i]));
    BOOST_REQUIRE_MESSAGE(r.code == 0, k_containers[i] << " was not running when the test went to "
                                                          "kill it (it exited by itself): "
                                                       << r.out);
}

void start_node(std::size_t i) {
    BOOST_TEST_MESSAGE("start " << k_containers[i]);
    const auto r = os::real_exec(os::docker_start_cmd(k_containers[i]));
    BOOST_REQUIRE_MESSAGE(r.code == 0, "start " << k_containers[i] << " failed: " << r.out);
}

void isolate(std::size_t i) {
    BOOST_TEST_MESSAGE("isolate " << k_containers[i]);
    os::checked_exec(os::real_exec, os::network_disconnect_cmd(k_network, k_containers[i]));
}

void rejoin(std::size_t i) {
    BOOST_TEST_MESSAGE("rejoin " << k_containers[i]);
    os::checked_exec(os::real_exec,
                     os::network_connect_cmd(k_network, k_containers[i], k_services[i]));
}

/// N from ledger_ctl's "verified N blocks of ledger bank", or -1.
long verified_blocks(const std::string& out) {
    constexpr std::string_view prefix = "verified ";
    const auto at = out.find(prefix);
    if (at == std::string::npos || out.find(" blocks of ledger bank", at) == std::string::npos) {
        return -1;
    }
    try {
        return std::stol(out.substr(at + prefix.size()));
    } catch (const std::exception&) {
        return -1;
    }
}

/// What one writer knows about its documents: inserts acknowledged and
/// inserts whose outcome is unknown, and its counter's last acknowledged
/// and last attempted values.
struct writer_record {
    std::vector<std::pair<std::string, std::int64_t>> acked;
    std::vector<std::string> unknown;
    std::int64_t counter_acked = 0;
    std::int64_t counter_attempted = 0;
    long errors = 0;
};

/// One writer: inserts w<t>x<i> with balance i, and every fifth step
/// replaces its counter document c<t> with the next value. An operation
/// is retried for up to 20 s (an insert that finds its document already
/// there was written by an earlier, unconfirmed attempt); after that its
/// outcome is unknown and the writer moves on.
void run_writer(std::size_t t, const std::atomic<bool>& stop, writer_record& rec) {
    auto c = make_client(5s);
    const auto tag = std::to_string(t);
    const auto counter = doc_id("c" + tag);
    for (std::int64_t i = 0; !stop.load(); ++i) {
        if (i % 5 == 4 && rec.counter_attempted > 0) {
            const auto next = rec.counter_attempted + 1;
            rec.counter_attempted = next;
            try {
                eventually(
                    [&] {
                        return c.execute(k_ledger, {statements::replace("acct", counter,
                                                                        account("c" + tag, next))});
                    },
                    20s);
                rec.counter_acked = next;
            } catch (const std::exception&) {
                ++rec.errors;
            }
            continue;
        }
        const auto name = "w" + tag + "x" + std::to_string(i);
        const auto first = rec.counter_attempted == 0;
        const auto id = first ? counter : doc_id(name);
        const auto balance = first ? std::int64_t{1} : i;
        if (first) {
            rec.counter_attempted = 1;
        }
        try {
            eventually(
                [&] {
                    try {
                        c.execute(k_ledger,
                                  {statements::insert("acct", id,
                                                      account(first ? "c" + tag : name, balance))});
                    } catch (const ledger_client_error& e) {
                        if (e.code() != "document_exists") {
                            throw;
                        }
                    }
                    return 0;
                },
                20s);
            if (first) {
                rec.counter_acked = 1;
            } else {
                rec.acked.emplace_back(id, balance);
            }
        } catch (const std::exception&) {
            ++rec.errors;
            if (!first) {
                rec.unknown.push_back(id);
            }
        }
        std::this_thread::sleep_for(20ms);
    }
}

struct NodeLogsOnFailure : boost::unit_test::test_observer {
    void test_unit_finish(const boost::unit_test::test_unit& tu, unsigned long) override {
        if (tu.p_type != boost::unit_test::TUT_CASE ||
            boost::unit_test::results_collector.results(tu.p_id).passed()) {
            return;
        }
        std::cerr << "── node logs after " << tu.full_name() << " failed ──\n" << node_logs();
    }
};

}  // namespace

struct LedgerChaosFixture {
    LedgerChaosFixture() {
        boost::unit_test::framework::register_observer(_observer);
        ::setenv("KYTHIRA_LEDGER_CHAOS_PKI_DIR", certs().dir.c_str(), 1);
        os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
        os::checked_exec(os::real_exec, compose_cmd({"up", "-d"}));
        if (!create_ledger_and_table(180s) || !wait_ready(120s)) {
            BOOST_TEST_MESSAGE(node_logs());
            os::try_exec(os::real_exec, compose_cmd({"down", "-v", "--remove-orphans"}));
            BOOST_FAIL("the ledger chaos cluster did not become ready");
        }
    }
    ~LedgerChaosFixture() {
        boost::unit_test::framework::deregister_observer(_observer);
        try {
            os::real_exec(compose_cmd({"down", "-v", "--remove-orphans"}));
        } catch (...) {
        }
    }
    LedgerChaosFixture(const LedgerChaosFixture&) = delete;
    LedgerChaosFixture& operator=(const LedgerChaosFixture&) = delete;
    LedgerChaosFixture(LedgerChaosFixture&&) = delete;
    LedgerChaosFixture& operator=(LedgerChaosFixture&&) = delete;

private:
    NodeLogsOnFailure _observer;
};

BOOST_TEST_GLOBAL_FIXTURE(LedgerChaosFixture);

BOOST_AUTO_TEST_CASE(kills_and_partitions_leave_every_replica_with_the_same_verified_journal,
                     *boost::unit_test::timeout(1500)) {
    std::atomic<bool> stop{false};
    std::array<writer_record, k_writers> records;
    std::vector<std::thread> writers;
    for (std::size_t t = 0; t < k_writers; ++t) {
        writers.emplace_back([&, t] { run_writer(t, stop, records[t]); });
    }
    struct joiner {
        std::atomic<bool>& stop;
        std::vector<std::thread>& threads;
        ~joiner() {
            stop = true;
            for (auto& th : threads) {
                if (th.joinable()) {
                    th.join();
                }
            }
        }
    } const join{stop, writers};

    // Faults. Each kill is SIGKILL: whatever a member acknowledged must
    // already be on its disk.
    std::this_thread::sleep_for(5s);
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        kill_node(i);
        std::this_thread::sleep_for(5s);
        start_node(i);
        std::this_thread::sleep_for(10s);
    }
    // Each member cut off in turn, so one of the rounds isolates the
    // ledger's leader with no quorum on its side.
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        isolate(i);
        std::this_thread::sleep_for(6s);
        rejoin(i);
        std::this_thread::sleep_for(8s);
    }
    // Two at once: no quorum until they are back.
    kill_node(0);
    kill_node(1);
    std::this_thread::sleep_for(4s);
    start_node(0);
    start_node(1);
    std::this_thread::sleep_for(10s);
    // The whole cluster.
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        kill_node(i);
    }
    std::this_thread::sleep_for(3s);
    for (std::size_t i = 0; i < k_containers.size(); ++i) {
        start_node(i);
    }
    BOOST_REQUIRE_MESSAGE(wait_ready(180s), "the cluster is not ready after the full restart");
    std::this_thread::sleep_for(10s);
    stop = true;
    for (auto& th : writers) {
        th.join();
    }

    // Every acknowledged write is there.
    auto c = make_client();
    std::size_t acked = 0;
    std::size_t unknown = 0;
    long errors = 0;
    for (std::size_t t = 0; t < k_writers; ++t) {
        const auto& r = records[t];
        for (const auto& [id, balance] : r.acked) {
            const auto doc = eventually([&] { return c.get_document(k_ledger, "acct", id); }, 60s);
            BOOST_CHECK_MESSAGE(balance_in(doc) == balance,
                                "acknowledged insert " << id << " (balance " << balance
                                                       << ") reads back as "
                                                       << (doc ? doc->to_text() : "missing"));
        }
        const auto counter = balance_in(eventually(
            [&] { return c.get_document(k_ledger, "acct", doc_id("c" + std::to_string(t))); },
            60s));
        // Missing only if even its insert was never acknowledged.
        const bool counter_ok = counter.has_value()
                                    ? *counter >= r.counter_acked && *counter <= r.counter_attempted
                                    : r.counter_acked == 0;
        BOOST_CHECK_MESSAGE(counter_ok, "writer " << t << "'s counter is " << counter.value_or(-1)
                                                  << ", acknowledged " << r.counter_acked
                                                  << ", attempted " << r.counter_attempted);
        acked += r.acked.size();
        unknown += r.unknown.size();
        errors += r.errors;
    }
    BOOST_TEST_MESSAGE("acknowledged inserts " << acked << ", unknown " << unknown
                                               << ", operations given up " << errors);
    // The load was real, on both sides of the faults.
    BOOST_CHECK_GT(acked, 200U);

    // The journal re-hashes to the cluster's signed digest through the API.
    const std::string api_cmd = std::string(KYTHIRA_LEDGER_CTL) + " verify --api " + url(0) + "," +
                                url(1) + "," + url(2) + " --ledger bank --tls-ca " + certs().ca +
                                " --cert " + certs().admin_cert + " --key " + certs().admin_key +
                                " --ca " + certs().ca;
    const auto api = os::real_exec({"sh", "-c", api_cmd});
    BOOST_REQUIRE_MESSAGE(api.code == 0, api.out);
    const auto blocks = verified_blocks(api.out);
    BOOST_TEST_MESSAGE(api.out);
    BOOST_CHECK_GT(blocks, static_cast<long>(acked));

    // The digest every member's own copy must hash to, in the PKI
    // directory every member mounts. No write follows, so its tip is the
    // journal's last block.
    const auto digest = eventually([&] { return c.digest(k_ledger); }, 60s);
    const auto bin = digest.to_binary();
    write_public_file(certs().dir / "digest.ion", std::string(bin.begin(), bin.end()));

    // Each member re-hashes its journal, its documents and its segment
    // files, read beside the running node, to that signed digest. A member
    // still catching up fails with the digest's tip beyond its journal, so
    // each gets a minute.
    for (const auto* container : k_containers) {
        os::CmdResult r;
        const auto deadline = std::chrono::steady_clock::now() + 60s;
        do {
            r = os::real_exec({os::container_runtime(), "exec", container,
                               "/usr/local/bin/ledger_ctl", "verify", "--data-dir", "/data",
                               "--ledger", "bank", "--digest", "/pki/digest.ion", "--ca",
                               "/pki/ca.pem"});
            if (r.code == 0 && verified_blocks(r.out) == blocks) {
                break;
            }
            std::this_thread::sleep_for(1s);
        } while (std::chrono::steady_clock::now() < deadline);
        BOOST_TEST_MESSAGE(container << ": " << r.out);
        BOOST_CHECK_MESSAGE(r.code == 0, container << ": " << r.out);
        BOOST_CHECK_MESSAGE(verified_blocks(r.out) == blocks,
                            container << " holds " << verified_blocks(r.out)
                                      << " blocks, the API verified " << blocks);
    }
}
