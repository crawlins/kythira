// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE multi_raft_node_s3_process_test
#include <boost/test/unit_test.hpp>

// Three `multi_raft_node --persistence s3` processes against
// tests/s3_stateful_mock.hpp (.kiro/specs/multi-raft-node-s3-persistence/
// tasks 7 and 8, Requirements 1.4, 4.4, 10.3, 10.4 and 11.3-11.7).
//
// A unit test of the store cannot show that a cluster on S3 elects, commits,
// survives a full restart, refuses a duplicate host, or batches under group
// commit, because each of those needs the host's own wiring: the SDK scope in
// `main`, `make_store()` called from `store_factory`, the durability class
// reaching `node`. So these run the built binary, each with an explicit,
// scrubbed environment (static dummy keys, an empty HOME, IMDS off), so no
// developer credential and no `~/.aws` reaches a case.
//
// Registered only when the S3 arm is compiled in (Requirement 10.6).

#include "kv_data_path.hpp"
#include "multi_raft_kv_workload.hpp"
#include "s3_stateful_mock.hpp"

#include <boost/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <csignal>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef MULTI_RAFT_NODE_PATH
#error "MULTI_RAFT_NODE_PATH must name the built multi_raft_node binary"
#endif

using kythira::bench::kv_data_client;
using kythira::bench::kv_outcome;
using kythira::bench::wire_read_kind;
using kythira::testing::s3_stateful_mock;
using namespace std::chrono_literals;

namespace {

auto contains(const std::string& haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

auto slurp(const std::filesystem::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// A port nothing is listening on now. Released before use, so another
/// process could take it in between; the window is microseconds on a test
/// machine and every other process test here accepts it.
auto free_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    ::close(fd);
    return ntohs(addr.sin_port);
}

/// Static dummy keys, an empty HOME (no `~/.aws`), IMDS off, and nothing else.
auto scrubbed_env() -> std::vector<std::string> {
    static const auto home = [] {
        auto dir = std::filesystem::temp_directory_path() / "kythira-s3-process-empty-home";
        std::filesystem::create_directories(dir);
        return dir.string();
    }();
    return {"AWS_ACCESS_KEY_ID=AKIDPROCESSTEST", "AWS_SECRET_ACCESS_KEY=process-test-secret",
            "AWS_EC2_METADATA_DISABLED=true", "HOME=" + home, "PATH=/usr/bin:/bin"};
}

struct host_ports {
    std::uint16_t raft{};
    std::uint16_t data{};
    std::uint16_t control{};
};

/// One running `multi_raft_node`. Its output goes to a file kept for the
/// failure message.
class host_process {
public:
    host_process(std::vector<std::string> args, const std::string& log_name)
        : _log(std::filesystem::temp_directory_path() /
               ("kythira-s3-" + log_name + "-" + std::to_string(::getpid()) + ".log")) {
        const int fd = ::open(_log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        BOOST_REQUIRE(fd >= 0);

        std::string binary = MULTI_RAFT_NODE_PATH;
        std::vector<char*> argv{binary.data()};
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        auto env = scrubbed_env();
        std::vector<char*> envp;
        for (auto& entry : env) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
        const int spawned =
            ::posix_spawn(&_pid, binary.c_str(), &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        ::close(fd);
        BOOST_REQUIRE_MESSAGE(spawned == 0, "could not spawn " << binary);
    }

    ~host_process() {
        if (_pid > 0) {
            stop(SIGKILL);
        }
        std::error_code ignored;
        std::filesystem::remove(_log, ignored);
    }

    host_process(const host_process&) = delete;
    auto operator=(const host_process&) -> host_process& = delete;
    host_process(host_process&&) = delete;
    auto operator=(host_process&&) -> host_process& = delete;

    /// Signals the host and reaps it. Returns the wait status.
    auto stop(int signal = SIGTERM) -> int {
        if (_pid <= 0) {
            return _status;
        }
        ::kill(_pid, signal);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (true) {
            const pid_t done = ::waitpid(_pid, &_status, WNOHANG);
            if (done == _pid) {
                break;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                ::kill(_pid, SIGKILL);
                ::waitpid(_pid, &_status, 0);
                break;
            }
            std::this_thread::sleep_for(20ms);
        }
        _pid = -1;
        return _status;
    }

    /// Waits for the process to exit on its own, up to @p limit.
    auto wait_exit(std::chrono::milliseconds limit) -> std::optional<int> {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::waitpid(_pid, &_status, WNOHANG) == _pid) {
                _pid = -1;
                return _status;
            }
            std::this_thread::sleep_for(20ms);
        }
        return std::nullopt;
    }

    [[nodiscard]] auto running() const -> bool { return _pid > 0; }
    [[nodiscard]] auto output() const -> std::string { return slurp(_log); }

private:
    std::filesystem::path _log;
    pid_t _pid{-1};
    int _status{0};
};

auto http_get(std::uint16_t port, const std::string& path) -> std::optional<std::string> {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1s);
    client.set_read_timeout(3s);
    auto res = client.Get(path);
    if (!res || res->status != 200) {
        return std::nullopt;
    }
    return res->body;
}

/// A three-host cluster on one mock, all hosts sharing one bucket and one
/// prefix, as an operator would type it.
class s3_cluster {
public:
    s3_cluster(s3_stateful_mock& mock, std::size_t groups, std::vector<std::string> extra)
        : _mock(mock), _groups(groups), _extra(std::move(extra)) {
        for (std::uint64_t id = 1; id <= 3; ++id) {
            _ports[id] = {free_port(), free_port(), free_port()};
        }
    }

    ~s3_cluster() { stop_all(SIGKILL); }
    s3_cluster(const s3_cluster&) = delete;
    auto operator=(const s3_cluster&) -> s3_cluster& = delete;
    s3_cluster(s3_cluster&&) = delete;
    auto operator=(s3_cluster&&) -> s3_cluster& = delete;

    /// The arguments host `id` runs with, at `ports`.
    [[nodiscard]] auto args(std::uint64_t id, const host_ports& ports) const
        -> std::vector<std::string> {
        std::vector<std::string> out{"--node-id",
                                     std::to_string(id),
                                     "--bind",
                                     "127.0.0.1",
                                     "--raft-port",
                                     std::to_string(ports.raft),
                                     "--data-port",
                                     std::to_string(ports.data),
                                     "--control-port",
                                     std::to_string(ports.control),
                                     "--groups",
                                     std::to_string(_groups),
                                     "--election-timeout-min",
                                     "300",
                                     "--election-timeout-max",
                                     "600",
                                     "--heartbeat-interval",
                                     "50",
                                     "--op-timeout",
                                     "5000",
                                     "--persistence",
                                     "s3",
                                     "--s3-bucket",
                                     "b",
                                     "--s3-prefix",
                                     "run",
                                     "--s3-endpoint",
                                     _mock.endpoint(),
                                     "--s3-region",
                                     "us-east-1",
                                     "--s3-timeout",
                                     "5000"};
        for (std::uint64_t peer = 1; peer <= 3; ++peer) {
            const auto& p = peer == id ? ports : _ports.at(peer);
            out.push_back("--peer");
            out.push_back(std::to_string(peer) + "=http://127.0.0.1:" + std::to_string(p.raft));
        }
        out.insert(out.end(), _extra.begin(), _extra.end());
        return out;
    }

    auto start(std::uint64_t id) -> void {
        _hosts[id] =
            std::make_unique<host_process>(args(id, _ports.at(id)), "host" + std::to_string(id));
    }
    auto start_all() -> void {
        for (std::uint64_t id = 1; id <= 3; ++id) {
            start(id);
        }
    }
    auto stop(std::uint64_t id, int signal = SIGTERM) -> int {
        auto it = _hosts.find(id);
        if (it == _hosts.end() || !it->second) {
            return 0;
        }
        return it->second->stop(signal);
    }
    auto stop_all(int signal = SIGTERM) -> void {
        for (auto& [id, host] : _hosts) {
            if (host) {
                host->stop(signal);
            }
        }
    }

    /// Every running host reports every group led, within @p limit.
    auto wait_ready(std::chrono::milliseconds limit = 60s) -> void {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (auto& [id, host] : _hosts) {
            if (!host || !host->running()) {
                continue;
            }
            while (!http_get(_ports.at(id).control, "/ready")) {
                BOOST_REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline,
                                      "host " << id << " never became ready:\n"
                                              << host->output());
                BOOST_REQUIRE_MESSAGE(host->running(), "host " << id << " exited:\n"
                                                               << host->output());
                std::this_thread::sleep_for(100ms);
            }
        }
    }

    /// The host leading `group`, if any running host says it does.
    [[nodiscard]] auto leader_of(std::uint64_t group) const -> std::optional<std::uint64_t> {
        for (const auto& [id, host] : _hosts) {
            if (!host || !host->running()) {
                continue;
            }
            const auto body = http_get(_ports.at(id).control, "/leaders");
            if (!body) {
                continue;
            }
            const auto parsed = boost::json::parse(*body);
            for (const auto& g : parsed.at("leads").as_array()) {
                if (g.to_number<std::uint64_t>() == group) {
                    return id;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto describe(std::uint64_t id) const -> boost::json::object {
        const auto body = http_get(_ports.at(id).control, "/describe");
        BOOST_REQUIRE_MESSAGE(body.has_value(), "no /describe from host " << id);
        return boost::json::parse(*body).as_object();
    }

    /// Writes `key` = `value` through whichever host leads its shard,
    /// following not-leader hints, until @p limit. True on an acknowledged
    /// write.
    auto put(const std::string& key, const std::string& value,
             std::chrono::milliseconds limit = 20s) -> bool {
        return until_ok(limit, [&](kv_data_client& client) {
            return client.put(key, kythira::testing::kv_put(key, value));
        });
    }

    /// A linearizable read through the log. The value, or nullopt.
    auto get(const std::string& key, std::chrono::milliseconds limit = 20s)
        -> std::optional<std::string> {
        std::optional<std::string> value;
        until_ok(limit, [&](kv_data_client& client) {
            auto r = client.get(key, wire_read_kind::log, kythira::testing::kv_get(key));
            if (r.ok()) {
                value =
                    std::string(reinterpret_cast<const char*>(r._value.data()), r._value.size());
            }
            return r;
        });
        return value;
    }

    [[nodiscard]] auto ports(std::uint64_t id) const -> const host_ports& { return _ports.at(id); }
    [[nodiscard]] auto host(std::uint64_t id) -> host_process& { return *_hosts.at(id); }

private:
    template<typename F> auto until_ok(std::chrono::milliseconds limit, F&& attempt) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        std::uint64_t target = 1;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!_hosts.contains(target) || !_hosts.at(target) || !_hosts.at(target)->running()) {
                target = target % 3 + 1;
                continue;
            }
            kv_data_client client("127.0.0.1", _ports.at(target).data, "application/json", 1s, 6s);
            const auto r = attempt(client);
            if (r.ok()) {
                return true;
            }
            if (r._outcome == kv_outcome::not_leader && r._leader.has_value()) {
                const auto hinted = kythira::host_node_id_text(*r._leader);
                target = std::stoull(hinted);
            } else {
                target = target % 3 + 1;
                std::this_thread::sleep_for(50ms);
            }
        }
        return false;
    }

    s3_stateful_mock& _mock;
    std::size_t _groups;
    std::vector<std::string> _extra;
    std::map<std::uint64_t, host_ports> _ports;
    std::map<std::uint64_t, std::unique_ptr<host_process>> _hosts;
};

auto value_for(std::size_t i) -> std::string {
    return "value-" + std::to_string(i);
}

/// Writes keys [first, first + count) from `writers` threads; returns the
/// keys that were acknowledged.
auto write_keys(s3_cluster& cluster, std::size_t first, std::size_t count, std::size_t writers,
                std::chrono::milliseconds per_key_limit = 20s) -> std::set<std::size_t> {
    std::mutex mu;
    std::set<std::size_t> acked;
    std::atomic<std::size_t> next{first};
    std::vector<std::thread> threads;
    for (std::size_t w = 0; w < writers; ++w) {
        threads.emplace_back([&] {
            for (auto i = next.fetch_add(1); i < first + count; i = next.fetch_add(1)) {
                if (cluster.put(kythira::testing::kv_key(i), value_for(i), per_key_limit)) {
                    std::lock_guard lock(mu);
                    acked.insert(i);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    return acked;
}

auto require_readable(s3_cluster& cluster, const std::set<std::size_t>& keys) -> void {
    std::size_t missing = 0;
    for (const auto i : keys) {
        const auto got = cluster.get(kythira::testing::kv_key(i));
        if (got != value_for(i)) {
            ++missing;
            BOOST_TEST_MESSAGE("key " << i << " read back as "
                                      << (got ? "'" + *got + "'" : std::string("nothing")));
        }
    }
    BOOST_TEST(missing == 0U, missing << " of " << keys.size() << " acknowledged keys are lost");
}

auto describe_hosts(s3_cluster& cluster) -> std::string {
    std::string out;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        out += "\n── host " + std::to_string(id) + " ──\n" + cluster.host(id).output();
    }
    return out;
}

/// The highest log index the mock holds under `log_prefix`, for either
/// layout: a singleton's key is its index, a segment's body names its last.
auto last_log_index(const s3_stateful_mock& mock, const std::string& log_prefix) -> std::uint64_t {
    std::uint64_t last = 0;
    for (const auto& key : mock.keys("b", log_prefix)) {
        const auto object = mock.object("b", key);
        if (!object) {
            continue;
        }
        const auto parsed = boost::json::parse(object->body).as_object();
        if (const auto* l = parsed.if_contains("last_index")) {
            last = std::max(last, l->to_number<std::uint64_t>());
        } else {
            last = std::max(last, parsed.at("index").to_number<std::uint64_t>());
        }
    }
    return last;
}

auto log_prefix_of(std::uint64_t host, std::uint64_t group) -> std::string {
    return "run/nodes/" + std::to_string(host) + "/groups/" + std::to_string(group) + "/log/";
}

}  // namespace

// ── Requirement 10.3 and 10.4: write, restart, read; refuse a duplicate ─────

BOOST_AUTO_TEST_SUITE(cluster_on_s3)

BOOST_AUTO_TEST_CASE(survives_a_full_restart_and_refuses_a_duplicate_host) {
    for (const char* write_behind : {"0", "1"}) {
        BOOST_TEST_CONTEXT("--s3-write-behind " << write_behind) {
            s3_stateful_mock mock;
            s3_cluster cluster(mock, 2, {"--s3-write-behind", write_behind});
            cluster.start_all();
            cluster.wait_ready();

            const auto acked = write_keys(cluster, 0, 200, 4);
            BOOST_REQUIRE_MESSAGE(acked.size() == 200U, acked.size()
                                                            << " of 200 writes acknowledged"
                                                            << describe_hosts(cluster));

            const auto described = cluster.describe(1);
            BOOST_TEST(described.at("persistence").as_string() == "s3");
            BOOST_TEST(described.at("durable").as_bool());
            const auto& s3 = described.at("s3").as_object();
            BOOST_TEST(s3.at("write_behind").as_bool() == (std::string(write_behind) == "1"));
            BOOST_TEST(s3.at("host_prefix").as_string() == "run/nodes/1");
            BOOST_TEST(s3.at("fencing").as_string() == "cas");
            BOOST_TEST(described.at("fenced_groups").as_array().empty());

            cluster.stop_all(SIGTERM);
            for (std::uint64_t host = 1; host <= 3; ++host) {
                for (std::uint64_t group = 1; group <= 2; ++group) {
                    const auto prefix = "run/nodes/" + std::to_string(host) + "/groups/" +
                                        std::to_string(group) + "/";
                    BOOST_TEST(!mock.keys("b", prefix).empty(), "nothing under " << prefix);
                }
            }

            cluster.start_all();
            cluster.wait_ready();
            require_readable(cluster, acked);

            // Requirement 10.4 / 11.6: a fourth process under node 1's id,
            // with its own ports, against the same bucket and prefix.
            const auto puts_before = mock.put_log().size();
            host_ports dup{free_port(), free_port(), free_port()};
            host_process duplicate(cluster.args(1, dup), "duplicate");
            const auto status = duplicate.wait_exit(30s);
            BOOST_REQUIRE_MESSAGE(status.has_value(), "the duplicate host kept running:\n"
                                                          << duplicate.output());
            BOOST_TEST(WIFEXITED(*status));
            BOOST_TEST(WEXITSTATUS(*status) != 0);
            const auto out = duplicate.output();
            BOOST_TEST(contains(out, "held by another process"), out);
            BOOST_TEST(contains(out, "run/nodes/1/groups/1/owner"), out);
            // It refused at the owner claim and wrote nothing. The cluster is
            // idle, so any PUT in this window would be the duplicate's.
            BOOST_TEST(mock.put_log().size() == puts_before);
            // And host 1 still serves.
            BOOST_TEST((cluster.get(kythira::testing::kv_key(7)) == value_for(7)));
            BOOST_TEST(cluster.describe(1).at("fenced_groups").as_array().empty());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Requirement 11: group commit through the binary ─────────────────────────

BOOST_AUTO_TEST_SUITE(write_behind)

/// B = entries the leader made durable / log PUTs it sent, over a window of
/// 16 concurrent writers with every PUT delayed 50 ms.
struct batching_result {
    double b{};
    std::size_t puts{};
    std::uint64_t entries{};
    std::uint64_t flushes{};
};

auto measure_batching(const char* write_behind, const char* log) -> batching_result {
    s3_stateful_mock mock;
    s3_cluster cluster(mock, 1, {"--s3-write-behind", write_behind, "--s3-log", log});
    cluster.start_all();
    cluster.wait_ready();
    // A warm-up write, so the window starts with a settled leader.
    BOOST_REQUIRE(cluster.put("warm", "up"));
    const auto leader = cluster.leader_of(1);
    BOOST_REQUIRE(leader.has_value());
    const auto prefix = log_prefix_of(*leader, 1);

    const auto entries_before = last_log_index(mock, prefix);
    const auto flushes_before =
        cluster.describe(*leader).at("s3").as_object().at("flushes").to_number<std::uint64_t>();
    mock.reset_counters();
    mock.set_put_delay(50ms);
    const auto acked = write_keys(cluster, 0, 320, 16);
    mock.set_put_delay(0ms);
    BOOST_REQUIRE_MESSAGE(acked.size() == 320U, acked.size() << " of 320 acknowledged");
    BOOST_REQUIRE_MESSAGE((cluster.leader_of(1) == leader), "leadership moved during the window");

    batching_result r;
    r.puts = mock.put_count(prefix);
    r.entries = last_log_index(mock, prefix) - entries_before;
    r.flushes =
        cluster.describe(*leader).at("s3").as_object().at("flushes").to_number<std::uint64_t>() -
        flushes_before;
    r.b = r.puts == 0 ? 0.0 : static_cast<double>(r.entries) / static_cast<double>(r.puts);
    return r;
}

BOOST_AUTO_TEST_CASE(group_commit_batches_and_the_baseline_does_not) {
    const auto on = measure_batching("1", "segmented");
    const auto off = measure_batching("0", "per-entry");
    BOOST_TEST_MESSAGE("write-behind on, segmented: B = "
                       << on.b << " (" << on.entries << " entries in " << on.puts << " log PUTs, "
                       << on.flushes << " flushes)");
    BOOST_TEST_MESSAGE("write-behind off, per-entry: B = " << off.b << " (" << off.entries
                                                           << " entries in " << off.puts
                                                           << " log PUTs)");
    BOOST_TEST(on.b > 2.0, "B = " << on.b);
    BOOST_TEST(off.entries == off.puts, "B = " << off.b);
    // Requirement 11.7: a flush of at most 16 entries fits one segment, so the
    // status surface's flush count is the mock's log PUT count.
    BOOST_TEST(on.flushes == on.puts);
}

BOOST_AUTO_TEST_CASE(acknowledged_writes_survive_a_leader_sigkill) {
    s3_stateful_mock mock;
    s3_cluster cluster(mock, 1, {"--s3-write-behind", "1", "--s3-log", "segmented"});
    cluster.start_all();
    cluster.wait_ready();
    BOOST_REQUIRE(cluster.put("warm", "up"));
    const auto leader = cluster.leader_of(1);
    BOOST_REQUIRE(leader.has_value());

    mock.set_put_delay(50ms);
    std::set<std::size_t> acked;
    std::thread load([&] { acked = write_keys(cluster, 0, 2000, 16, 3s); });
    std::this_thread::sleep_for(2s);
    // SIGKILL, not SIGTERM: a graceful stop could flush the staged run and
    // hide exactly the loss this case exists to catch.
    cluster.stop(*leader, SIGKILL);
    load.join();
    mock.set_put_delay(0ms);
    BOOST_TEST_MESSAGE(acked.size() << " writes acknowledged around the kill");
    BOOST_REQUIRE(!acked.empty());

    cluster.start(*leader);
    cluster.wait_ready();
    require_readable(cluster, acked);
}

BOOST_AUTO_TEST_CASE(a_failed_flush_never_acknowledges_a_lost_entry) {
    s3_stateful_mock mock;
    s3_cluster cluster(mock, 1, {"--s3-write-behind", "1", "--s3-log", "segmented"});
    cluster.start_all();
    cluster.wait_ready();
    BOOST_REQUIRE(cluster.put("warm", "up"));
    const auto leader = cluster.leader_of(1);
    BOOST_REQUIRE(leader.has_value());

    mock.set_put_delay(20ms);
    mock.fail_next_puts(log_prefix_of(*leader, 1), 1);
    const auto acked = write_keys(cluster, 0, 200, 16, 5s);
    mock.set_put_delay(0ms);
    BOOST_TEST_MESSAGE(acked.size() << " of 200 acknowledged with one leader log PUT failed");
    BOOST_REQUIRE_MESSAGE(mock.pending_failures() == 0U, "the injected failure never fired");
    BOOST_REQUIRE(!acked.empty());
    require_readable(cluster, acked);

    // Later writes still succeed.
    BOOST_TEST(cluster.put("after", "failure"));

    cluster.stop_all(SIGTERM);
    cluster.start_all();
    cluster.wait_ready();
    require_readable(cluster, acked);
}

BOOST_AUTO_TEST_SUITE_END()
