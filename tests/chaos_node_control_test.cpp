// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_node's HTTP control plane (.kiro/specs/cairn-chaos-testing/
// Requirement 1, tasks 2.1-2.6): the request and response formats in
// control_codec.hpp, the state machine's command encodings including the new
// cas, and every route of http_control.hpp served by a real one-node cluster
// on loopback.

#define BOOST_TEST_MODULE chaos_node_control_test
#include <boost/test/unit_test.hpp>

#include "control_codec.hpp"
#include "http_control.hpp"

#include <raft/console_logger.hpp>
#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include "test_timeout_scale.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using kythira::testing::scaled_deadline;
using kythira::testing::scaled_timeout;
namespace control = chaos_node::control;
using kv = kythira::test_key_value_state_machine<std::uint64_t>;

auto bytes(std::initializer_list<int> in) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (int b : in) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

auto apply(kv& sm, const std::vector<std::byte>& cmd, std::uint64_t index)
    -> std::vector<std::byte> {
    return sm.apply(cmd, index);
}

}  // namespace

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInit {
    FollyInit() {
        int argc = 1;
        char arg0[] = "chaos_node_control_test";
        char* args[] = {arg0, nullptr};
        char** argv = args;
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInit);
#endif

// ── test_key_value_state_machine encodings (task 2.5) ────────────────────────

BOOST_AUTO_TEST_SUITE(state_machine_encodings)

// Every test and example that uses this state machine depends on these bytes;
// adding cas must not move them.
BOOST_AUTO_TEST_CASE(put_get_del_bytes_are_unchanged) {
    BOOST_TEST(kv::make_put_command("ab", "x") ==
               bytes({1, 2, 0, 0, 0, 'a', 'b', 1, 0, 0, 0, 'x'}));
    BOOST_TEST(kv::make_get_command("ab") == bytes({0, 2, 0, 0, 0, 'a', 'b'}));
    BOOST_TEST(kv::make_del_command("ab") == bytes({2, 2, 0, 0, 0, 'a', 'b'}));
    BOOST_TEST(static_cast<int>(kv::command_type::get) == 0);
    BOOST_TEST(static_cast<int>(kv::command_type::put) == 1);
    BOOST_TEST(static_cast<int>(kv::command_type::del) == 2);
}

BOOST_AUTO_TEST_CASE(cas_bytes) {
    BOOST_TEST(kv::make_cas_command("k", std::string("e"), "v") ==
               bytes({3, 1, 0, 0, 0, 'k', 1, 1, 0, 0, 0, 'e', 1, 0, 0, 0, 'v'}));
    BOOST_TEST(kv::make_cas_command("k", std::nullopt, "v") ==
               bytes({3, 1, 0, 0, 0, 'k', 0, 0, 0, 0, 0, 1, 0, 0, 0, 'v'}));
}

BOOST_AUTO_TEST_CASE(cas_applies_only_on_match) {
    kv sm;
    // Absent key: expecting absence applies, expecting a value does not.
    BOOST_TEST(apply(sm, kv::make_cas_command("k", std::string("a"), "b"), 1) == bytes({0}));
    BOOST_TEST(!sm.contains("k"));
    BOOST_TEST(apply(sm, kv::make_cas_command("k", std::nullopt, "a"), 2) == bytes({1}));
    BOOST_TEST(sm.get_value("k").value() == "a");
    // Present key: expecting absence fails, a wrong value fails, the right one applies.
    BOOST_TEST(apply(sm, kv::make_cas_command("k", std::nullopt, "z"), 3) == bytes({0}));
    BOOST_TEST(apply(sm, kv::make_cas_command("k", std::string("x"), "z"), 4) == bytes({0}));
    BOOST_TEST(sm.get_value("k").value() == "a");
    BOOST_TEST(apply(sm, kv::make_cas_command("k", std::string("a"), "b"), 5) == bytes({1}));
    BOOST_TEST(sm.get_value("k").value() == "b");
    BOOST_TEST(sm.get_last_applied_index() == 5U);
}

BOOST_AUTO_TEST_CASE(truncated_cas_is_refused) {
    kv sm;
    auto cmd = kv::make_cas_command("k", std::string("e"), "v");
    for (std::size_t len = 1; len < cmd.size(); ++len) {
        std::vector<std::byte> cut(cmd.begin(), cmd.begin() + static_cast<std::ptrdiff_t>(len));
        BOOST_CHECK_THROW(sm.apply(cut, 1), std::invalid_argument);
    }
    BOOST_TEST(!sm.contains("k"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── control_codec.hpp ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(codec)

BOOST_AUTO_TEST_CASE(base64_matches_rfc4648_vectors) {
    auto enc = [](std::string_view s) {
        std::vector<std::byte> b;
        for (char c : s) {
            b.push_back(static_cast<std::byte>(c));
        }
        return control::base64_encode(b);
    };
    BOOST_TEST(enc("") == "");
    BOOST_TEST(enc("f") == "Zg==");
    BOOST_TEST(enc("fo") == "Zm8=");
    BOOST_TEST(enc("foo") == "Zm9v");
    BOOST_TEST(enc("foob") == "Zm9vYg==");
    BOOST_TEST(enc("fooba") == "Zm9vYmE=");
    BOOST_TEST(enc("foobar") == "Zm9vYmFy");
    BOOST_TEST(control::base64_encode(bytes({0xFF, 0xFE})) == "//4=");
}

BOOST_AUTO_TEST_CASE(missing_entry_reasons) {
    BOOST_TEST(control::missing_entry_reason(0, 0, 10) == "compacted");
    BOOST_TEST(control::missing_entry_reason(5, 5, 10) == "compacted");
    BOOST_TEST(control::missing_entry_reason(11, 5, 10) == "beyond_end");
    BOOST_TEST(control::missing_entry_reason(7, 5, 10) == "missing");
}

BOOST_AUTO_TEST_CASE(log_range_defaults_and_clamps) {
    auto r = control::parse_log_range("3", std::nullopt);
    BOOST_REQUIRE(r);
    BOOST_TEST(r.value->from == 3U);
    BOOST_TEST(r.value->to == 3U + control::k_max_log_range - 1);

    r = control::parse_log_range("1", "5000");
    BOOST_REQUIRE(r);
    BOOST_TEST(r.value->to == control::k_max_log_range);

    r = control::parse_log_range("2", "4");
    BOOST_REQUIRE(r);
    BOOST_TEST(r.value->to == 4U);

    r = control::parse_log_range("18446744073709551615", std::nullopt);
    BOOST_REQUIRE(r);
    BOOST_TEST(r.value->to == UINT64_MAX);
}

BOOST_AUTO_TEST_CASE(log_range_refusals) {
    BOOST_TEST(!control::parse_log_range(std::nullopt, "4"));
    BOOST_TEST(!control::parse_log_range("0", std::nullopt));
    BOOST_TEST(!control::parse_log_range("x", std::nullopt));
    BOOST_TEST(!control::parse_log_range("5", "4"));
    BOOST_TEST(!control::parse_log_range("5", "-1"));
    BOOST_TEST(!control::parse_log_range("18446744073709551616", std::nullopt));
}

BOOST_AUTO_TEST_CASE(command_request_put_is_the_old_body) {
    auto r = control::parse_command_request(R"({"key":"a","value":"1"})");
    BOOST_REQUIRE(r);
    BOOST_TEST((r.value->op == control::command_request::op_kind::put));
    BOOST_TEST(control::encode_command(*r.value) == kv::make_put_command("a", "1"));
}

BOOST_AUTO_TEST_CASE(command_request_cas) {
    auto r = control::parse_command_request(R"({"op":"cas","key":"a","expected":"1","value":"2"})");
    BOOST_REQUIRE(r);
    BOOST_TEST(control::encode_command(*r.value) ==
               kv::make_cas_command("a", std::string("1"), "2"));

    r = control::parse_command_request(R"({"op":"cas","key":"a","expected":null,"value":"2"})");
    BOOST_REQUIRE(r);
    BOOST_TEST(!r.value->expected.has_value());
}

BOOST_AUTO_TEST_CASE(command_request_refusals) {
    BOOST_TEST(!control::parse_command_request("not json"));
    BOOST_TEST(!control::parse_command_request(R"({"key":"a"})"));
    BOOST_TEST(!control::parse_command_request(R"({"key":1,"value":"1"})"));
    BOOST_TEST(!control::parse_command_request(R"({"op":"del","key":"a","value":"1"})"));
    // cas without expected is ambiguous between "absent" and a forgotten field.
    BOOST_TEST(!control::parse_command_request(R"({"op":"cas","key":"a","value":"1"})"));
    BOOST_TEST(
        !control::parse_command_request(R"({"op":"cas","key":"a","expected":3,"value":"1"})"));
}

BOOST_AUTO_TEST_CASE(membership_request) {
    auto r = control::parse_membership_request<kythira::host_node_id>(R"({"node_id":4})");
    BOOST_REQUIRE(r);
    BOOST_TEST((*r.value == kythira::host_node_id_from_number(4)));
    r = control::parse_membership_request<kythira::host_node_id>(R"({"node_id":"4"})");
    BOOST_REQUIRE(r);
    BOOST_TEST((*r.value == kythira::host_node_id_from_number(4)));
    BOOST_TEST(!control::parse_membership_request<kythira::host_node_id>(R"({"node_id":-1})"));
    BOOST_TEST(!control::parse_membership_request<kythira::host_node_id>(R"({"id":4})"));
    BOOST_TEST(!control::parse_membership_request<kythira::host_node_id>(R"({"node_id":"x y"})"));
}

BOOST_AUTO_TEST_CASE(kv_read_escapes_caller_strings) {
    auto out = boost::json::serialize(
        control::kv_read_json("k\"1", control::read_mode::local, std::string("v\\\"x"), 7, 6));
    auto back = boost::json::parse(out).as_object();
    BOOST_TEST(back.at("key").as_string() == "k\"1");
    BOOST_TEST(back.at("value").as_string() == "v\\\"x");
    BOOST_TEST(back.at("mode").as_string() == "local");
    BOOST_TEST(back.at("found").as_bool());

    auto none = control::kv_read_json("k", control::read_mode::linearizable, std::nullopt, 1, 1);
    BOOST_TEST(none.at("value").is_null());
    BOOST_TEST(!none.at("found").as_bool());
}

BOOST_AUTO_TEST_CASE(change_in_progress_maps_from_message) {
    BOOST_TEST(control::is_change_in_progress("Configuration change already in progress"));
    BOOST_TEST(!control::is_change_in_progress("not leader"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── http_control.hpp against a real one-node cluster ─────────────────────────

namespace {

struct control_types : kythira::basic_tcp_raft_types<kythira::host_node_id> {
    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
};
using control_node = kythira::node<control_types>;

auto free_port() -> std::uint16_t {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    ::close(fd);
    return ntohs(addr.sin_port);
}

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds deadline = scaled_deadline(10000)) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

// One voter, so it elects itself and commits alone: every route can be
// exercised end to end without a second process.
class one_node_fixture {
public:
    one_node_fixture()
        : _self(kythira::host_node_id_from_number(1)),
          _rpc_port(free_port()),
          _http_port(free_port()) {
        kythira::raft_configuration cfg;
        cfg._election_timeout_min = scaled_deadline(150);
        cfg._election_timeout_max = scaled_deadline(300);
        cfg._heartbeat_interval = std::chrono::milliseconds{30};
        cfg._rpc_timeout = scaled_deadline(500);
        kythira::node_config<control_types> ncfg{
            .node_id = _self,
            .network_client = kythira::basic_tcp_rpc_client<kythira::host_node_id>{},
            .network_server =
                kythira::basic_tcp_rpc_server<kythira::host_node_id>{_rpc_port, "127.0.0.1"},
            .persistence = {},
            .logger = kythira::console_logger{kythira::log_level::warning},
            .metrics = {},
            .membership = {},
            .config = cfg,
        };
        _node = std::make_unique<control_node>(std::move(ncfg));
        _node->set_cluster_configuration({_self});
        _node->start();
        _ticker = std::thread([this] {
            while (!_stop.load()) {
                _node->check_election_timeout();
                _node->check_heartbeat_timeout();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        });
        _http = std::make_unique<chaos_node::http_control_tmpl<control_node>>(*_node, _self,
                                                                              _http_port);
        _http->start();
        _client = std::make_unique<httplib::Client>("127.0.0.1", _http_port);
        _client->set_read_timeout(std::chrono::seconds{10});
    }

    ~one_node_fixture() {
        _http->stop();
        _stop.store(true);
        _ticker.join();
        _node->stop();
    }

    one_node_fixture(const one_node_fixture&) = delete;
    auto operator=(const one_node_fixture&) -> one_node_fixture& = delete;

    auto get(const std::string& path) -> std::pair<int, boost::json::object> {
        auto res = _client->Get(path);
        BOOST_REQUIRE(res);
        return {res->status, boost::json::parse(res->body).as_object()};
    }

    auto post(const std::string& path, const std::string& body)
        -> std::pair<int, boost::json::object> {
        auto res = _client->Post(path, body, "application/json");
        BOOST_REQUIRE(res);
        return {res->status, boost::json::parse(res->body).as_object()};
    }

    auto wait_for_leader() -> bool {
        // The HTTP server comes up asynchronously; /health doubles as its probe.
        return wait_until([this] {
            auto res = _client->Get("/status");
            return res && res->status == 200 &&
                   boost::json::parse(res->body).as_object().at("role").as_string() == "leader";
        });
    }

    [[nodiscard]] auto self() const -> const kythira::host_node_id& { return _self; }
    auto node() -> control_node& { return *_node; }

private:
    kythira::host_node_id _self;
    std::uint16_t _rpc_port;
    std::uint16_t _http_port;
    std::unique_ptr<control_node> _node;
    std::atomic<bool> _stop{false};
    std::thread _ticker;
    std::unique_ptr<chaos_node::http_control_tmpl<control_node>> _http;
    std::unique_ptr<httplib::Client> _client;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(routes)

BOOST_AUTO_TEST_CASE(every_route_on_a_one_node_cluster,
                     *boost::unit_test::timeout(scaled_timeout(60))) {
    one_node_fixture f;
    BOOST_REQUIRE(f.wait_for_leader());
    const auto self = control::node_id_value(f.self());

    // 2.2 status
    auto [st, status] = f.get("/status");
    BOOST_TEST(st == 200);
    BOOST_TEST(status.at("leader_id") == self);
    BOOST_TEST(status.at("config").at("voters").as_array().size() == 1U);
    BOOST_TEST(status.at("config").at("voters").as_array().at(0) == self);
    BOOST_TEST(status.at("config").at("learners").as_array().empty());
    BOOST_TEST(!status.at("config").at("joint").as_bool());
    BOOST_TEST(status.at("snapshot_index").to_number<std::uint64_t>() == 0U);
    BOOST_TEST(status.at("last_log_term") == status.at("term"));

    // 2.5 put, then cas that fails and one that applies
    auto [p1, put] = f.post("/command", R"({"key":"k","value":"a"})");
    BOOST_TEST(p1 == 200);
    BOOST_TEST(put.at("success").as_bool());
    BOOST_TEST(!put.contains("applied"));

    auto [c1, cas_miss] =
        f.post("/command", R"({"op":"cas","key":"k","expected":"x","value":"b"})");
    BOOST_TEST(c1 == 200);
    BOOST_TEST(!cas_miss.at("applied").as_bool());
    auto [c2, cas_hit] = f.post("/command", R"({"op":"cas","key":"k","expected":"a","value":"b"})");
    BOOST_TEST(c2 == 200);
    BOOST_TEST(cas_hit.at("applied").as_bool());
    auto [c3, cas_new] =
        f.post("/command", R"({"op":"cas","key":"n","expected":null,"value":"1"})");
    BOOST_TEST(c3 == 200);
    BOOST_TEST(cas_new.at("applied").as_bool());

    auto [bad, bad_body] = f.post("/command", R"({"op":"cas","key":"k","value":"b"})");
    BOOST_TEST(bad == 400);
    BOOST_TEST(bad_body.at("error").as_string() == "bad_request");

    // 2.4 both read modes see the cas result
    auto [k1, lin] = f.get("/kv/k?mode=linearizable");
    BOOST_TEST(k1 == 200);
    BOOST_TEST(lin.at("value").as_string() == "b");
    BOOST_TEST(lin.at("mode").as_string() == "linearizable");
    auto [k2, local] = f.get("/kv/k?mode=local");
    BOOST_TEST(k2 == 200);
    BOOST_TEST(local.at("value").as_string() == "b");
    BOOST_TEST(local.at("last_applied").to_number<std::uint64_t>() >=
               cas_hit.at("commit_index").to_number<std::uint64_t>());
    auto [k3, absent] = f.get("/kv/missing");
    BOOST_TEST(k3 == 200);
    BOOST_TEST(!absent.at("found").as_bool());
    auto [k4, bad_mode] = f.get("/kv/k?mode=stale");
    BOOST_TEST(k4 == 400);

    // 2.1 single entries and ranges
    auto [s2, status2] = f.get("/status");
    const auto last = status2.at("last_log_index").to_number<std::uint64_t>();
    BOOST_REQUIRE(last >= 5U);  // no-op, put, three cas
    auto [l1, entry] = f.get("/log/" + std::to_string(last));
    BOOST_TEST(l1 == 200);
    BOOST_TEST(entry.at("index").to_number<std::uint64_t>() == last);
    BOOST_TEST(entry.at("type").as_string() == "normal");
    BOOST_TEST(entry.at("command_b64").as_string() ==
               control::base64_encode(kv::make_cas_command("n", std::nullopt, "1")));
    auto [l2, beyond] = f.get("/log/" + std::to_string(last + 1));
    BOOST_TEST(l2 == 404);
    BOOST_TEST(beyond.at("error").as_string() == "beyond_end");
    auto [l3, zero] = f.get("/log/0");
    BOOST_TEST(l3 == 404);
    BOOST_TEST(zero.at("error").as_string() == "compacted");

    auto [r1, range] = f.get("/log?from=1&to=" + std::to_string(last + 50));
    BOOST_TEST(r1 == 200);
    BOOST_TEST(range.at("entries").as_array().size() == last);
    BOOST_TEST(range.at("entries").as_array().at(0).at("index").to_number<std::uint64_t>() == 1U);
    auto [r2, bad_range] = f.get("/log?to=3");
    BOOST_TEST(r2 == 400);

    // 2.3 one election, in the current term
    auto [e1, elections] = f.get("/elections");
    BOOST_TEST(e1 == 200);
    BOOST_TEST(elections.at("node_id") == self);
    BOOST_REQUIRE(elections.at("elections").as_array().size() == 1U);
    BOOST_TEST(elections.at("elections").as_array().at(0).at("term") == status2.at("term"));

    // 2.6 a malformed body is a 400 before the node is asked. A real add or
    // remove needs a second node, which the Docker scenarios provide.
    auto [m1, malformed] = f.post("/membership/add", R"({"id":2})");
    BOOST_TEST(m1 == 400);
}

BOOST_AUTO_TEST_SUITE_END()
