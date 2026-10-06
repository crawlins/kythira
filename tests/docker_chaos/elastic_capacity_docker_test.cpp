// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file elastic_capacity_docker_test.cpp
/// @brief Elastic shard capacity end to end on containers, with no cloud
///        credentials (`.kiro/specs/elastic-shard-capacity/` task 16,
///        Requirements 1.6 and 16.5).
///
/// Three `multi_raft_node` containers from `docker/elastic-capacity-compose.yml`
/// hold two shards. Load is written through the real data path until the
/// key-count split policy splits both, which takes shards per node over the
/// controller's high watermark. The controller, running in node 1, then creates
/// a fourth container through `docker_quorum_manager` and admits it onto shards
/// as a learner that catches up and is promoted. The test passes when the
/// controller records the admission complete and the new machine is a voter
/// in at least one shard, with no shard ever below three voters.
///
/// **Runtime-neutral (CLAUDE.md).** Everything goes through
/// `container_runtime()` and `compose_prefix()`. Under Podman the controller's
/// container API is the rootless Podman socket, found with `podman info` unless
/// CONTAINER_SOCKET already names one; Podman serves the Docker-compatible API
/// there, so the same manager drives both.
///
/// **It cleans up after itself and checks that it did.** Teardown removes every
/// container carrying this run's `kythira.cluster` label — including the one the
/// controller created, which compose knows nothing about — and the last
/// assertion is a leak audit that fails the run if any remains.
///
/// Gated on `KYTHIRA_DOCKER_INTEGRATION_TESTS=1`, as the other scenario tests
/// are: without a container runtime there is nothing to test.

#define BOOST_TEST_MODULE elastic_capacity_docker_test
#include <boost/test/unit_test.hpp>

#include "os_faults.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace os = docker_chaos::os;
namespace json = boost::json;

constexpr int k_controller_port = 27003;
constexpr std::array<int, 3> k_data_ports{27101, 27102, 27103};
constexpr std::uint64_t k_key_count = 1000;  // KEY_COUNT in the compose file
constexpr std::size_t k_keys_written = 240;  // four times SPLIT_KEYS, over two shards

auto enabled() -> bool {
    const char* e = std::getenv("KYTHIRA_DOCKER_INTEGRATION_TESTS");
    return e != nullptr && std::string(e) == "1";
}

auto compose_file() -> std::string {
    if (const char* e = std::getenv("KYTHIRA_ELASTIC_CAPACITY_COMPOSE_FILE");
        (e != nullptr) && (*e != 0)) {
        return e;
    }
    return "docker/elastic-capacity-compose.yml";
}

/// The workload's key format (`tests/multi_raft_kv_workload.hpp`), so keys
/// fall in the ranges the compose file's GROUPS and KEY_COUNT tile.
auto kv_key(std::uint64_t n) -> std::string {
    auto s = std::to_string(n);
    return std::string(s.size() < 10 ? 10 - s.size() : 0, '0') + s;
}

/// One run's cluster: up in the constructor, down and audited in the
/// destructor whatever the test did in between.
struct cluster {
    std::string _name;
    os::CmdExecutor _exec{os::real_exec};

    cluster() : _name("ecap" + std::to_string(::getpid())) {
        ::setenv("CAPACITY_CLUSTER", _name.c_str(), 1);
        if (std::getenv("CONTAINER_SOCKET") == nullptr && os::container_runtime() == "podman") {
            // The Docker-compatible API the controller dials, under rootless
            // Podman. `podman system service` (or the podman.socket user unit)
            // must be serving it.
            auto out = os::checked_exec(
                _exec, {"podman", "info", "--format", "{{.Host.RemoteSocket.Path}}"});
            while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
                out.pop_back();
            }
            ::setenv("CONTAINER_SOCKET", out.c_str(), 1);
        }
        teardown();  // a crashed earlier run with the same pid
        auto up = os::compose_prefix();
        up.insert(up.end(), {"-f", compose_file(), "-p", _name, "up", "-d"});
        os::checked_exec(_exec, up);
    }

    ~cluster() {
        try {
            teardown();
        } catch (...) {
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    auto teardown() -> void {
        // The controller's container first: compose never created it, so
        // `down` would leave it attached to the network and the network
        // undeletable.
        for (const auto& id : ids_labelled()) {
            os::try_exec(_exec, {os::container_runtime(), "rm", "--force", id});
        }
        auto down = os::compose_prefix();
        down.insert(down.end(),
                    {"-f", compose_file(), "-p", _name, "down", "--volumes", "--remove-orphans"});
        os::try_exec(_exec, down);
        os::try_exec(_exec,
                     {os::container_runtime(), "network", "rm", "kythira-" + _name + "-net"});
    }

    /// Every container, running or not, carrying this run's cluster label.
    [[nodiscard]] auto ids_labelled(const std::string& extra = {}) const
        -> std::vector<std::string> {
        std::vector<std::string> cmd{
            os::container_runtime(),         "ps", "--all", "--quiet", "--filter",
            "label=kythira.cluster=" + _name};
        if (!extra.empty()) {
            cmd.insert(cmd.end(), {"--filter", "label=" + extra});
        }
        const auto r = _exec(cmd);
        std::vector<std::string> ids;
        std::istringstream in(r.out);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) {
                ids.push_back(line);
            }
        }
        return ids;
    }

    [[nodiscard]] auto logs(const std::string& container) const -> std::string {
        return _exec({os::container_runtime(), "logs", "--tail", "40", container}).out;
    }
};

/// The controller's own view, from its control plane.
auto status() -> std::optional<json::object> {
    httplib::Client c("127.0.0.1", k_controller_port);
    c.set_connection_timeout(2s);
    c.set_read_timeout(5s);
    auto res = c.Get("/capacity/status");
    if (!res || res->status != 200) {
        return std::nullopt;
    }
    try {
        return json::parse(res->body).as_object();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

auto wait_for(const std::function<bool()>& pred, std::chrono::seconds limit) -> bool {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1s);
    }
    return pred();
}

/// `test_key_value_state_machine::make_put_command`'s encoding: the byte 1,
/// then the key and the value, each after its little-endian u32 length.
/// Spelled out here because that header pulls in the future backend (folly
/// in CI), which these scenario binaries deliberately do not link.
auto put_command(const std::string& key, const std::string& value) -> std::string {
    std::string out(1, '\x01');
    for (const auto* part : {&key, &value}) {
        const auto n = static_cast<std::uint32_t>(part->size());
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>((n >> shift) & 0xffU));
        }
        out += *part;
    }
    return out;
}

/// One write through the real data path, to whichever host leads the key.
auto put(const std::string& key) -> bool {
    const auto body = put_command(key, "v-" + key);
    for (int attempt = 0; attempt < 30; ++attempt) {
        for (const auto port : k_data_ports) {
            httplib::Client c("127.0.0.1", port);
            c.set_connection_timeout(2s);
            c.set_read_timeout(5s);
            auto res = c.Post("/kv/put?key=" + key, body, "application/json");
            if (res && res->status == 200) {
                return true;
            }
        }
        std::this_thread::sleep_for(200ms);
    }
    return false;
}

auto fresh_nodes(const json::object& s) -> std::size_t {
    std::size_t n = 0;
    for (const auto& v : s.at("nodes").as_array()) {
        if (!v.as_object().at("stale").as_bool()) {
            ++n;
        }
    }
    return n;
}

auto shards(const json::object& s) -> const json::array& {
    return s.at("shards").as_array();
}

auto ids_of(const json::value& v) -> std::set<std::uint64_t> {
    std::set<std::uint64_t> out;
    for (const auto& x : v.as_array()) {
        out.insert(json::value_to<std::uint64_t>(x));
    }
    return out;
}

/// The completed scale-out intent, if there is one.
auto completed_scale_out(const json::object& s) -> std::optional<json::object> {
    for (const auto& v : s.at("intents").as_array()) {
        const auto& i = v.as_object();
        if (i.at("kind").as_string() == "scale_out" && i.at("state").as_string() == "completed") {
            return i;
        }
    }
    return std::nullopt;
}

auto counter(const json::object& s, const std::string& name) -> std::uint64_t {
    const auto& c = s.at("counters").as_object();
    return c.contains(name) ? json::value_to<std::uint64_t>(c.at(name)) : 0;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(elastic_capacity_docker)

BOOST_AUTO_TEST_CASE(a_loaded_cluster_splits_provisions_a_container_and_admits_it,
                     *boost::unit_test::timeout(900)) {
    if (!enabled()) {
        BOOST_TEST_MESSAGE("KYTHIRA_DOCKER_INTEGRATION_TESTS is not 1; skipping");
        return;
    }
    std::size_t min_voters_seen = 3;
    {
        cluster c;

        // 1. Three hosts heartbeating into the controller.
        BOOST_REQUIRE_MESSAGE(wait_for([] { return status() && fresh_nodes(*status()) == 3; }, 90s),
                              "the controller never saw three fresh hosts\n"
                                  << c.logs("kythira-" + c._name + "-1"));
        // Shards are reported by their leaders, so they can trail the hosts'
        // first heartbeats by an election and a heartbeat.
        BOOST_REQUIRE_MESSAGE(
            wait_for([] { return status() && shards(*status()).size() == 2; }, 60s),
            "the controller never saw the two starting shards\n"
                << c.logs("kythira-" + c._name + "-1"));

        // 2. Load through the data path, spread evenly over the key space.
        std::size_t written = 0;
        for (std::size_t i = 0; i < k_keys_written; ++i) {
            written += put(kv_key(i * k_key_count / k_keys_written)) ? 1 : 0;
        }
        BOOST_TEST_MESSAGE("wrote " << written << " of " << k_keys_written << " keys");
        BOOST_REQUIRE_GE(written, k_keys_written * 9 / 10);

        // 3. The cluster splits: more shards than it started with.
        BOOST_REQUIRE_MESSAGE(
            wait_for([] { return status() && shards(*status()).size() > 2; }, 120s),
            "no split reached the controller\n"
                << c.logs("kythira-" + c._name + "-1"));

        // 4. A fourth machine is provisioned and admitted. Every shard keeps
        //    three voters throughout (Requirement 10.3: never remove before
        //    the replacement votes).
        std::optional<json::object> done;
        const bool admitted = wait_for(
            [&] {
                const auto s = status();
                if (!s) {
                    return false;
                }
                for (const auto& v : shards(*s)) {
                    min_voters_seen =
                        std::min(min_voters_seen, v.as_object().at("voters").as_array().size());
                }
                done = completed_scale_out(*s);
                return done.has_value();
            },
            420s);
        if (!admitted) {
            if (const auto s = status()) {
                BOOST_TEST_MESSAGE("controller status: " << json::serialize(*s));
            }
            BOOST_TEST_MESSAGE(c.logs("kythira-" + c._name + "-1"));
            BOOST_TEST_MESSAGE(c.logs("kythira-" + c._name + "-4"));
        }
        BOOST_REQUIRE_MESSAGE(admitted, "no scale-out intent completed");

        const auto s = *status();
        const auto node = json::value_to<std::uint64_t>(done->at("node"));
        BOOST_TEST_MESSAGE("admitted node " << node << ": " << json::serialize(s.at("shards")));
        BOOST_CHECK_GT(node, 3U);
        BOOST_CHECK_EQUAL(json::value_to<std::size_t>(s.at("cluster_size")), 4U);
        std::size_t voter_on_new = 0;
        for (const auto& v : shards(s)) {
            const auto& o = v.as_object();
            BOOST_CHECK_GE(o.at("voters").as_array().size(), 3U);
            voter_on_new += ids_of(o.at("voters")).contains(node) ? 1 : 0;
        }
        BOOST_CHECK_GE(voter_on_new, 1U);
        BOOST_CHECK_GE(counter(s, "admission{outcome=completed}"), 1U);
        BOOST_CHECK_GE(counter(s, "shards_moved"), 1U);

        // 5. The container is the one the intent created, found by the
        //    idempotency key the controller put on it.
        const auto key = std::string(done->at("key").as_string());
        BOOST_CHECK_EQUAL(c.ids_labelled("kythira.idempotency-key=" + key).size(), 1U);
    }
    BOOST_CHECK_GE(min_voters_seen, 3U);

    // 6. The leak audit: nothing this run created is left.
    const auto name = "ecap" + std::to_string(::getpid());
    const auto left = os::real_exec({os::container_runtime(), "ps", "--all", "--quiet", "--filter",
                                     "label=kythira.cluster=" + name});
    BOOST_CHECK_MESSAGE(left.out.empty(), "containers left behind: " << left.out);
}

BOOST_AUTO_TEST_SUITE_END()
