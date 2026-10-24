// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The workload client's core and the history format it writes
// (cairn-chaos-testing task 4), against a scripted transport.

#define BOOST_TEST_MODULE chaos_workload_unit_test
#include <boost/test/unit_test.hpp>

#include "history.hpp"
#include "workload.hpp"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace h = docker_chaos::history;
namespace w = chaos_workload;

namespace {

// Answers each request from a per-(address, path prefix) queue of scripted
// responses, and records what was asked.
struct scripted {
    std::map<std::string, std::deque<w::response>> replies;
    std::vector<std::pair<std::string, std::string>> asked;  // address, path

    void on(const std::string& address, const std::string& path_prefix, w::response r) {
        replies[address + " " + path_prefix].push_back(std::move(r));
    }

    auto transport() -> w::transport {
        return [this](const std::string& address, const std::string& path,
                      const std::string& /*body*/) -> w::response {
            asked.emplace_back(address, path);
            for (auto& [k, q] : replies) {
                auto space = k.find(' ');
                if (k.substr(0, space) == address && path.rfind(k.substr(space + 1), 0) == 0 &&
                    !q.empty()) {
                    auto r = q.front();
                    q.pop_front();
                    return r;
                }
            }
            return w::response{.status = 0, .body = {}, .sent = false, .error = "unscripted"};
        };
    }
};

auto three_nodes() -> std::map<int, std::string> {
    return {{1, "n1:8080"}, {2, "n2:8080"}, {3, "n3:8080"}};
}

auto write_op(const std::string& key, const std::string& value) -> w::operation {
    return w::operation{
        .f = h::op_kind::write, .key = key, .value = value, .expected = {}, .local_read = false};
}

auto cas_op(const std::string& key, const std::string& expected, const std::string& value)
    -> w::operation {
    return w::operation{.f = h::op_kind::cas,
                        .key = key,
                        .value = value,
                        .expected = expected,
                        .local_read = false};
}

auto read_op(const std::string& key, bool local = false) -> w::operation {
    return w::operation{
        .f = h::op_kind::read, .key = key, .value = {}, .expected = {}, .local_read = local};
}

const w::response ok_put{
    .status = 200, .body = R"({"success":true,"commit_index":4})", .sent = true, .error = {}};
const w::response not_leader{
    .status = 503, .body = R"({"error":"not_leader"})", .sent = true, .error = {}};

}  // namespace

BOOST_AUTO_TEST_SUITE(config)

BOOST_AUTO_TEST_CASE(parse_mix_defaults_and_overrides) {
    auto m = w::parse_mix("");
    BOOST_TEST(m.write == 4U);
    m = w::parse_mix("read=0,local=0,write=1,cas=0");
    BOOST_TEST(m.linearizable_read == 0U);
    BOOST_TEST(m.write == 1U);
    BOOST_CHECK_THROW((void)w::parse_mix("bogus=1"), std::invalid_argument);
    BOOST_CHECK_THROW((void)w::parse_mix("write"), std::invalid_argument);
    BOOST_CHECK_THROW((void)w::parse_mix("read=0,local=0,write=0,cas=0"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(parse_nodes_table) {
    auto n = w::parse_nodes("1=c5node1:8080,2=c5node2:8080");
    BOOST_TEST(n.size() == 2U);
    BOOST_TEST(n.at(2) == "c5node2:8080");
    BOOST_CHECK_THROW((void)w::parse_nodes(""), std::invalid_argument);
    BOOST_CHECK_THROW((void)w::parse_nodes("1="), std::invalid_argument);
    BOOST_CHECK_THROW((void)w::parse_nodes("c5node1:8080"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(leader_of_numeric_text_and_null) {
    BOOST_TEST(w::leader_of(R"({"leader_id":3})").value() == 3);
    BOOST_TEST(w::leader_of(R"({"leader_id":"2"})").value() == 2);
    BOOST_TEST(!w::leader_of(R"({"leader_id":null})").has_value());
    BOOST_TEST(!w::leader_of(R"({"leader_id":"aws-ec2:x:y"})").has_value());
    BOOST_TEST(!w::leader_of("not json").has_value());
}

BOOST_AUTO_TEST_CASE(generator_is_seeded_and_covers_every_shape) {
    w::generator a(42, 3, 5, w::mix{});
    w::generator b(42, 3, 5, w::mix{});
    std::set<std::string> shapes;
    std::set<std::string> keys;
    for (int i = 0; i < 400; ++i) {
        auto x = a.next();
        auto y = b.next();
        BOOST_TEST(x.key == y.key);
        BOOST_TEST((x.f == y.f));
        BOOST_TEST((x.value == y.value));
        keys.insert(x.key);
        shapes.insert(std::string(h::to_string(x.f)) + (x.local_read ? "-local" : ""));
        if (x.f == h::op_kind::cas) {
            BOOST_TEST(x.expected.has_value());
        }
    }
    BOOST_TEST(shapes.size() == 4U);
    BOOST_TEST(keys == (std::set<std::string>{"k0", "k1", "k2"}));
    BOOST_CHECK_THROW(w::generator(1, 0, 5, w::mix{}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(outcomes)

BOOST_AUTO_TEST_CASE(write_ok_on_first_node) {
    scripted s;
    s.on("n1:8080", "/command", ok_put);
    w::client c(three_nodes(), s.transport());
    auto e = c.run(write_op("k0", "3"));
    BOOST_TEST((e.type == h::event_type::ok));
    BOOST_TEST(e.node == 1);
    BOOST_TEST(e.value.value() == "3");
    BOOST_TEST(e.attempts.size() == 1U);
}

// Requirement 4.3: a follower's not_leader sends the client to the leader
// its /status names, and both attempts are on the record.
BOOST_AUTO_TEST_CASE(not_leader_redirects_through_status) {
    scripted s;
    s.on("n1:8080", "/command", not_leader);
    s.on("n1:8080", "/status",
         w::response{.status = 200, .body = R"({"leader_id":3})", .sent = true, .error = {}});
    s.on("n3:8080", "/command", ok_put);
    w::client c(three_nodes(), s.transport());
    auto e = c.run(write_op("k0", "1"));
    BOOST_TEST((e.type == h::event_type::ok));
    BOOST_TEST(e.node == 3);
    BOOST_REQUIRE(e.attempts.size() == 2U);
    BOOST_TEST(e.attempts[0].node == 1);
    BOOST_TEST(e.attempts[0].status == 503);
    BOOST_TEST(e.attempts[1].node == 3);

    // The next operation starts at the leader it learned.
    s.on("n3:8080", "/command", ok_put);
    auto again = c.run(write_op("k0", "2"));
    BOOST_TEST(again.attempts.size() == 1U);
    BOOST_TEST(again.node == 3);
}

BOOST_AUTO_TEST_CASE(refused_connection_moves_on_without_effect) {
    scripted s;
    s.on("n1:8080", "/command",
         w::response{.status = 0, .body = {}, .sent = false, .error = "Connection"});
    s.on("n2:8080", "/command", ok_put);
    w::client c(three_nodes(), s.transport());
    auto e = c.run(write_op("k1", "4"));
    BOOST_TEST((e.type == h::event_type::ok));
    BOOST_TEST(e.node == 2);
    BOOST_TEST(e.attempts.size() == 2U);
}

// Requirement 4.2: a timeout after sending is info, never fail.
BOOST_AUTO_TEST_CASE(timeout_after_send_is_info) {
    scripted s;
    s.on("n1:8080", "/command",
         w::response{.status = 0, .body = {}, .sent = true, .error = "Read"});
    w::client c(three_nodes(), s.transport());
    auto e = c.run(write_op("k0", "1"));
    BOOST_TEST((e.type == h::event_type::info));
    BOOST_TEST(e.error.find("Read") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(commit_failed_is_info_because_the_leader_accepted_it) {
    scripted s;
    s.on("n1:8080", "/command",
         w::response{.status = 503,
                     .body = R"({"error":"commit_failed","detail":"timeout"})",
                     .sent = true,
                     .error = {}});
    w::client c(three_nodes(), s.transport());
    auto e = c.run(write_op("k0", "1"));
    BOOST_TEST((e.type == h::event_type::info));
    BOOST_TEST(e.attempts.size() == 1U);
}

BOOST_AUTO_TEST_CASE(bad_request_is_fail) {
    scripted s;
    s.on("n1:8080", "/command",
         w::response{
             .status = 400, .body = R"({"error":"bad_request"})", .sent = true, .error = {}});
    w::client c(three_nodes(), s.transport());
    BOOST_TEST((c.run(write_op("k0", "1")).type == h::event_type::fail));
}

BOOST_AUTO_TEST_CASE(cas_applied_or_not) {
    scripted s;
    s.on("n1:8080", "/command",
         w::response{.status = 200,
                     .body = R"({"success":true,"commit_index":5,"applied":true})",
                     .sent = true,
                     .error = {}});
    s.on("n1:8080", "/command",
         w::response{.status = 200,
                     .body = R"({"success":true,"commit_index":6,"applied":false})",
                     .sent = true,
                     .error = {}});
    s.on("n1:8080", "/command", ok_put);  // no "applied": cannot tell
    w::client c(three_nodes(), s.transport());
    auto hit = c.run(cas_op("k0", "1", "2"));
    BOOST_TEST((hit.type == h::event_type::ok));
    BOOST_TEST(hit.expected.value() == "1");
    BOOST_TEST((c.run(cas_op("k0", "1", "2")).type == h::event_type::fail));
    BOOST_TEST((c.run(cas_op("k0", "1", "2")).type == h::event_type::info));
}

BOOST_AUTO_TEST_CASE(reads_found_absent_and_redirected) {
    scripted s;
    s.on("n1:8080", "/kv/k0",
         w::response{.status = 200,
                     .body = R"({"key":"k0","found":true,"value":"3","commit_index":4,)"
                             R"("last_applied":4})",
                     .sent = true,
                     .error = {}});
    s.on("n1:8080", "/kv/k1",
         w::response{
             .status = 503, .body = R"({"error":"read_failed"})", .sent = true, .error = {}});
    s.on("n1:8080", "/status",
         w::response{.status = 200, .body = R"({"leader_id":"2"})", .sent = true, .error = {}});
    s.on("n2:8080", "/kv/k1",
         w::response{.status = 200,
                     .body = R"({"key":"k1","found":false,"value":null})",
                     .sent = true,
                     .error = {}});
    w::client c(three_nodes(), s.transport());
    auto found = c.run(read_op("k0"));
    BOOST_TEST((found.type == h::event_type::ok));
    BOOST_TEST(found.value.value() == "3");
    BOOST_TEST(found.mode == "linearizable");
    auto absent = c.run(read_op("k1"));
    BOOST_TEST((absent.type == h::event_type::ok));
    BOOST_TEST(!absent.value.has_value());
    BOOST_TEST(absent.node == 2);
}

BOOST_AUTO_TEST_CASE(local_read_goes_to_the_pinned_node) {
    scripted s;
    s.on("n3:8080", "/kv/k0",
         w::response{.status = 200,
                     .body = R"({"key":"k0","found":true,"value":"1"})",
                     .sent = true,
                     .error = {}});
    w::client c(three_nodes(), s.transport());
    auto e = c.run(read_op("k0", true), 3);
    BOOST_TEST((e.type == h::event_type::ok));
    BOOST_TEST(e.node == 3);
    BOOST_TEST(e.mode == "local");
    BOOST_TEST(s.asked.front().second == "/kv/k0?mode=local");
}

BOOST_AUTO_TEST_CASE(no_leader_anywhere_is_fail) {
    scripted s;
    for (const auto* a : {"n1:8080", "n2:8080", "n3:8080"}) {
        for (int i = 0; i < 2; ++i) {
            s.on(a, "/command", not_leader);
            s.on(a, "/status",
                 w::response{
                     .status = 200, .body = R"({"leader_id":null})", .sent = true, .error = {}});
        }
    }
    w::client c(three_nodes(), s.transport());
    auto e = c.run(write_op("k0", "1"));
    BOOST_TEST((e.type == h::event_type::fail));
    BOOST_TEST(e.attempts.size() == 6U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(history_format)

BOOST_AUTO_TEST_CASE(design_example_lines_round_trip) {
    const std::string invoke =
        R"({"p":3,"type":"invoke","f":"cas","key":"k2","value":["4","7"],"t":18273645})";
    auto e = h::from_line(invoke);
    BOOST_TEST(e.process == 3);
    BOOST_TEST((e.type == h::event_type::invoke));
    BOOST_TEST(e.expected.value() == "4");
    BOOST_TEST(e.value.value() == "7");
    BOOST_TEST(h::to_line(e) == invoke);

    h::event done = e;
    done.type = h::event_type::ok;
    done.node = 2;
    done.attempts = {h::attempt{.node = 1, .status = 503, .error = {}},
                     h::attempt{.node = 2, .status = 200, .error = {}}};
    auto back = h::from_line(h::to_line(done));
    BOOST_TEST(back.node == 2);
    BOOST_TEST(back.attempts.size() == 2U);
    BOOST_TEST(back.attempts[0].status == 503);
}

BOOST_AUTO_TEST_CASE(cas_expecting_absence_and_reads) {
    h::event cas{.process = 1,
                 .type = h::event_type::invoke,
                 .f = h::op_kind::cas,
                 .key = "k",
                 .value = "1",
                 .expected = std::nullopt,
                 .time_ns = 5,
                 .mode = {},
                 .node = 0,
                 .attempts = {},
                 .error = {}};
    auto line = h::to_line(cas);
    BOOST_TEST(line.find(R"("value":[null,"1"])") != std::string::npos);
    BOOST_TEST(!h::from_line(line).expected.has_value());

    auto read = h::from_line(
        R"({"p":0,"type":"ok","f":"read","key":"k","value":null,"t":9,"mode":"local"})");
    BOOST_TEST(!read.value.has_value());
    BOOST_TEST(read.mode == "local");
    BOOST_CHECK_THROW(
        (void)h::from_line(R"({"p":0,"type":"maybe","f":"read","key":"k","value":null,"t":9})"),
        std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(parse_history_skips_log_noise) {
    const std::string log =
        "[chaos_workload] nodes=5\n"
        R"({"p":0,"type":"invoke","f":"write","key":"k0","value":"1","t":1})"
        "\r\n"
        R"({"p":0,"type":"ok","f":"write","key":"k0","value":"1","t":2,"node":1})"
        "\n[chaos_workload] done\n";
    auto hist = h::parse_history(log);
    BOOST_TEST(hist.size() == 2U);
    BOOST_TEST(h::structural_error(hist).empty());
}

BOOST_AUTO_TEST_CASE(structural_rules) {
    auto ev = [](std::int64_t p, h::event_type t, const std::string& key = "k") {
        return h::event{.process = p,
                        .type = t,
                        .f = h::op_kind::write,
                        .key = key,
                        .value = "1",
                        .expected = {},
                        .time_ns = 0,
                        .mode = {},
                        .node = 0,
                        .attempts = {},
                        .error = {}};
    };
    using t = h::event_type;
    BOOST_TEST(
        h::structural_error({ev(0, t::invoke), ev(0, t::ok), ev(0, t::invoke), ev(1, t::invoke),
                             ev(0, t::fail), ev(1, t::info), ev(2, t::invoke)})
            .empty());
    BOOST_TEST(!h::structural_error({ev(0, t::invoke), ev(0, t::invoke)}).empty());
    BOOST_TEST(!h::structural_error({ev(0, t::ok)}).empty());
    BOOST_TEST(!h::structural_error({ev(0, t::invoke), ev(0, t::info), ev(0, t::invoke)}).empty());
    BOOST_TEST(!h::structural_error({ev(0, t::invoke, "a"), ev(0, t::ok, "b")}).empty());
}

BOOST_AUTO_TEST_SUITE_END()
