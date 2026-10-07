// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Chaos scenarios for a cluster whose Raft RPC runs over mutual TLS, with
// certificates ca_service issues at startup (docker/mtls-chaos-compose.yml).
//
// The first two cases are the crash scenarios crash_recovery_test.cpp runs on
// plain TCP, repeated on the TLS transport: elections, replication and
// catch-up have to survive a handshake on every RPC. The last two put an
// impostor at node3's address while node3 is down and check that the leader
// never replicates a single entry to it. Over plain TCP the impostor would be
// a full member: it claims node 3's id and answers at node 3's name, so the
// leader would backfill its log and its commit index would follow the
// cluster's. Only the trust policy keeps it out.

#define BOOST_TEST_MODULE mtls_chaos_test
#include <boost/test/unit_test.hpp>

#include "harness.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace docker_chaos;

namespace {

// ELECTION_TIMEOUT_MAX_MS in docker/mtls-chaos-compose.yml.
constexpr auto k_election_max = 2000ms;

auto mtls_compose_file() -> std::string {
    const char* env = std::getenv("KYTHIRA_MTLS_COMPOSE_FILE");
    if ((env != nullptr) && (*env != 0)) {
        return env;
    }
    return "docker/mtls-chaos-compose.yml";
}

auto compose_rogue_cmd(std::initializer_list<std::string> args) -> std::vector<std::string> {
    auto cmd = os::compose_prefix();
    cmd.insert(cmd.end(), {"-f", mtls_compose_file(), "--profile", "rogue"});
    cmd.insert(cmd.end(), args);
    return cmd;
}

auto commit_index_of(ChaosNode& n) -> std::optional<std::int64_t> {
    try {
        return n.status()["commit_index"].as_int64();
    } catch (...) {
        return std::nullopt;
    }
}

// Polls until `n`'s commit index reaches `target`.
auto wait_for_commit(ChaosNode& n, std::int64_t target, std::chrono::milliseconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto ci = commit_index_of(n); ci && *ci >= target) {
            return true;
        }
        std::this_thread::sleep_for(250ms);
    }
    return false;
}

auto submit_ok(ChaosNode& leader, const std::string& key, const std::string& value) -> bool {
    try {
        auto r = leader.submit_command(key, value);
        return r.contains("success") && r["success"].as_bool();
    } catch (...) {
        return false;
    }
}

// One impostor service from the "rogue" profile, started without its
// dependencies (the compose file explains why) and removed again on scope
// exit, before the fixture's `compose down` needs the network gone.
class rogue_service {
public:
    rogue_service(std::string service, std::string container, int http_port)
        : _service(std::move(service)),
          _container(container),
          node(3, http_port, 0, std::move(container), os::real_exec, real_http_get,
               real_http_post) {
        os::checked_exec(os::real_exec, compose_rogue_cmd({"up", "-d", "--no-deps", _service}));
    }
    ~rogue_service() {
        os::try_exec(os::real_exec, compose_rogue_cmd({"rm", "-s", "-f", _service}));
    }
    rogue_service(const rogue_service&) = delete;
    auto operator=(const rogue_service&) -> rogue_service& = delete;

    void wait_healthy(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (node.is_healthy()) {
                return;
            }
            std::this_thread::sleep_for(250ms);
        }
        throw std::runtime_error(_service + " did not become healthy");
    }

    [[nodiscard]] auto logs(int tail_lines) const -> std::string {
        return os::real_exec(os::docker_logs_cmd(_container, tail_lines)).out;
    }

    [[nodiscard]] auto logs_contain(std::string_view pattern) const -> bool {
        return logs(200).find(pattern) != std::string::npos;
    }

private:
    std::string _service;
    std::string _container;

public:
    ChaosNode node;
};

struct MtlsFixture {
    ChaosCluster cluster;

    // Startup covers both one-shot CA runs as well as the nodes.
    MtlsFixture() : cluster(mtls_compose_file(), std::chrono::seconds{90}) {
        cluster.start();
        cluster.wait_for_leader(k_election_max * 15);
    }

    ~MtlsFixture() {
        try {
            cluster.stop();
        } catch (...) {
        }
    }
    MtlsFixture(const MtlsFixture&) = delete;
    auto operator=(const MtlsFixture&) -> MtlsFixture& = delete;
};

// Submits through whichever node leads now, retrying across elections until
// `timeout`. Leadership may move between the two real members on a loaded
// runner (a handshake per RPC, one member down); that is not what the
// impostor cases measure. Each failed attempt is reported with what came
// back and how long it took, so a red run says why.
auto submit_via_leader(ChaosCluster& c, const std::string& key, const std::string& value,
                       std::chrono::milliseconds timeout) -> bool {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto started = std::chrono::steady_clock::now();
        std::string outcome;
        int via = 0;
        try {
            auto& leader = c.wait_for_leader(k_election_max * 5);
            via = leader.id();
            auto r = leader.submit_command(key, value);
            if (r.contains("success") && r["success"].as_bool()) {
                return true;
            }
            outcome = boost::json::serialize(r);
        } catch (const std::exception& e) {
            outcome = e.what();
        }
        BOOST_TEST_MESSAGE("submit " << key << " via node" << via << " failed after "
                                     << std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started)
                                            .count()
                                     << "ms: " << outcome);
        std::this_thread::sleep_for(250ms);
    }
    return false;
}

// The members log a DEBUG line per heartbeat and a warning per rejected
// AppendEntries to the impostor; neither says anything new, so both are
// dropped and the last `keep` remaining lines printed.
void dump_logs(MtlsFixture& f, const rogue_service& rogue) {
    constexpr std::size_t keep = 60;
    for (int id : {1, 2}) {
        std::vector<std::string> kept;
        for (auto& line : f.cluster.log_lines(id, 5000)) {
            if (line.find(" DEBUG: ") != std::string::npos ||
                line.find("[ErrorHandler]") != std::string::npos ||
                line.find("rejected by trust policy") != std::string::npos) {
                continue;
            }
            kept.push_back(std::move(line));
        }
        BOOST_TEST_MESSAGE("---- node" << id << " (" << kept.size() << " non-debug lines) ----");
        for (auto i = kept.size() > keep ? kept.size() - keep : 0; i < kept.size(); ++i) {
            BOOST_TEST_MESSAGE(kept[i]);
        }
    }
    BOOST_TEST_MESSAGE("---- impostor ----\n" << rogue.logs(40));
}

// Stops node3, puts `rogue` in its place, and checks that while the two
// remaining members keep committing, the impostor never receives an entry
// and never wins an election. Then node3 comes back and catches up, so the
// cluster is still whole once the impostor is gone.
//
// Only the impostor's state is asserted on. Elections between nodes 1 and 2
// are reported, not failed on: under rootless Podman a two-member quorum
// paying a TLS handshake per RPC can lose a heartbeat deadline, and the
// impostor's commit index and role are what show whether it got in.
void check_impostor_is_shut_out(MtlsFixture& f, const std::string& service,
                                const std::string& container, int http_port) {
    auto& n3 = f.cluster.node(3);
    n3.stop();

    auto& leader = f.cluster.wait_for_leader(k_election_max * 10);
    BOOST_TEST_REQUIRE(leader.id() != 3);
    const auto term_before = leader.status()["term"].as_int64();

    std::int64_t cluster_commit = 0;
    {
        rogue_service rogue(service, container, http_port);
        rogue.wait_healthy(30s);
        // The impostor runs the TLS path too, so a failure below is the
        // trust policy's doing, not a plaintext node talking past TLS ones.
        BOOST_TEST_REQUIRE(rogue.logs_contain("rpc: mutual TLS"));

        bool all_committed = true;
        for (int i = 0; i < 5; ++i) {
            if (!submit_via_leader(f.cluster, service + std::to_string(i), "v" + std::to_string(i),
                                   30s)) {
                all_committed = false;
                BOOST_ERROR("the two real members must still commit with an impostor at node3");
            }
        }
        if (auto ci = commit_index_of(f.cluster.wait_for_leader(k_election_max * 10))) {
            cluster_commit = *ci;
        }
        BOOST_TEST(cluster_commit >= 5);

        // Long enough for many heartbeats and several of the impostor's
        // own election timeouts.
        auto deadline = std::chrono::steady_clock::now() + k_election_max * 8;
        while (std::chrono::steady_clock::now() < deadline) {
            auto s = rogue.node.status();
            BOOST_TEST_REQUIRE(s["commit_index"].as_int64() == 0,
                               service << " received replicated entries");
            BOOST_TEST_REQUIRE(s["role"].as_string() != "leader", service << " won an election");
            std::this_thread::sleep_for(250ms);
        }

        const auto term_after =
            f.cluster.wait_for_leader(k_election_max * 10).status()["term"].as_int64();
        if (term_after != term_before) {
            BOOST_TEST_MESSAGE("nodes 1 and 2 held " << (term_after - term_before)
                                                     << " election(s) during the impostor run");
        }
        if (!all_committed || cluster_commit < 5) {
            dump_logs(f, rogue);
        }
    }

    n3.restart(/*wait=*/true, 30s);
    BOOST_TEST(wait_for_commit(n3, cluster_commit, 30s),
               "node3 did not catch up after the impostor left");
    f.cluster.assert_no_split_brain();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(docker_chaos_mtls)

BOOST_FIXTURE_TEST_CASE(commits_replicate_over_mtls, MtlsFixture, *boost::unit_test::timeout(240)) {
    for (int id = 1; id <= 3; ++id) {
        cluster.wait_for_log(id, "rpc: mutual TLS", 5s);
    }
    auto& leader = cluster.wait_for_leader(k_election_max * 10);
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(submit_ok(leader, "k" + std::to_string(i), "v" + std::to_string(i)));
    }
    auto target = *commit_index_of(leader);
    for (auto* n : cluster.all_nodes()) {
        BOOST_TEST(wait_for_commit(*n, target, 15s),
                   "node " << n->id() << " did not reach commit index " << target);
    }
    cluster.assert_no_split_brain();
}

BOOST_FIXTURE_TEST_CASE(leader_crash_reelects_and_catches_up_over_mtls, MtlsFixture,
                        *boost::unit_test::timeout(240)) {
    auto& old_leader = cluster.wait_for_leader(k_election_max * 10);
    const int old_id = old_leader.id();
    const auto old_term = old_leader.status()["term"].as_int64();

    old_leader.kill();

    auto& new_leader = cluster.wait_for_leader(k_election_max * 10);
    BOOST_TEST(new_leader.id() != old_id);
    BOOST_TEST(new_leader.status()["term"].as_int64() > old_term);
    for (int i = 0; i < 5; ++i) {
        BOOST_TEST(submit_ok(new_leader, "after" + std::to_string(i), "v"));
    }
    auto target = *commit_index_of(new_leader);

    // The old leader rejoins with its on-disk log and must be brought
    // forward over the same mutually authenticated channel.
    old_leader.restart(/*wait=*/true, 30s);
    BOOST_TEST(wait_for_commit(old_leader, target, 30s),
               "restarted node " << old_id << " did not catch up");
    cluster.assert_no_split_brain();
}

// A certificate named node3, but from another root.
BOOST_FIXTURE_TEST_CASE(foreign_ca_impostor_gets_no_entries, MtlsFixture,
                        *boost::unit_test::timeout(300)) {
    check_impostor_is_shut_out(*this, "rogue-foreign", "chaos_mtls_rogue_foreign", 8084);
}

// A certificate from the cluster's own root, for a name (node4) that is not a
// member: chaining to the root is not enough.
BOOST_FIXTURE_TEST_CASE(non_member_certificate_impostor_gets_no_entries, MtlsFixture,
                        *boost::unit_test::timeout(300)) {
    check_impostor_is_shut_out(*this, "rogue-intruder", "chaos_mtls_rogue_intruder", 8085);
}

BOOST_AUTO_TEST_SUITE_END()
