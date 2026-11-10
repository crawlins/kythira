// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Forwarding proposals to a group's leader (`.kiro/specs/
// kinesis-compatible-streams/` task 6): the gateway-to-gateway wire and its
// HMAC, and `raft_stream_cluster` over a stand-in multi-raft host.

#define BOOST_TEST_MODULE kinesis_forward_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_forward.hpp>
#include <kythira/kinesis/kinesis_raft_cluster.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kin = kythira::kinesis;
namespace fwd = kythira::kinesis::forward;

namespace {

auto bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

const auto t0 = std::chrono::system_clock::time_point{std::chrono::seconds{1'800'000'000}};

}  // namespace

BOOST_AUTO_TEST_SUITE(wire)

BOOST_AUTO_TEST_CASE(a_body_round_trips_with_nul_and_high_bytes) {
    const std::string key("\0meta/k", 7);
    const fwd::proposal p{._key = key,
                          ._command = bytes(std::string("\x00\xff\x80z", 4)),
                          ._admission = kin::admission::shard_limits};
    const auto body = fwd::encode_body(p);
    BOOST_TEST(body.size() == 4 + 7 + 1 + 4U);
    const auto back = fwd::decode_body(body);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((back->_key == key));
    BOOST_TEST((back->_command == p._command));
    BOOST_TEST((back->_admission == kin::admission::shard_limits));
}

BOOST_AUTO_TEST_CASE(a_short_or_overlong_body_is_refused) {
    BOOST_TEST(!fwd::decode_body("").has_value());
    BOOST_TEST(!fwd::decode_body(std::string("\0\0\0", 3)).has_value());
    // Key length 5 with only 2 bytes behind it.
    BOOST_TEST(!fwd::decode_body(std::string("\0\0\0\5ab", 6)).has_value());
    // Key length 0, no admission byte.
    BOOST_TEST(!fwd::decode_body(std::string("\0\0\0\0", 4)).has_value());
    // Key length 0, admission none, empty command: well formed.
    BOOST_TEST(fwd::decode_body(std::string("\0\0\0\0\0", 5)).has_value());
    // An admission byte this build does not know.
    BOOST_TEST(!fwd::decode_body(std::string("\0\0\0\0\x07", 5)).has_value());
}

BOOST_AUTO_TEST_CASE(a_signature_verifies_only_for_its_body_secret_and_time) {
    const std::string body = "payload";
    const auto h = fwd::sign("s3cret", body, t0);
    BOOST_TEST(fwd::verify("s3cret", h, body, t0));
    BOOST_TEST(fwd::verify("s3cret", h, body, t0 + std::chrono::seconds{60}));
    BOOST_TEST(fwd::verify("s3cret", h, body, t0 - std::chrono::seconds{60}));
    BOOST_TEST(!fwd::verify("s3cret", h, body, t0 + std::chrono::seconds{61}));
    BOOST_TEST(!fwd::verify("s3cret", h, body, t0 - std::chrono::seconds{61}));
    BOOST_TEST(!fwd::verify("other", h, body, t0));
    BOOST_TEST(!fwd::verify("s3cret", h, "payloaD", t0));
    // An empty secret verifies nothing, not even its own signature.
    BOOST_TEST(!fwd::verify("", fwd::sign("", body, t0), body, t0));
}

BOOST_AUTO_TEST_CASE(a_malformed_header_is_refused) {
    const std::string body = "b";
    const auto h = fwd::sign("k", body, t0);
    const auto colon = h.find(':');
    BOOST_TEST(!fwd::verify("k", "", body, t0));
    BOOST_TEST(!fwd::verify("k", h.substr(colon), body, t0));            // no time
    BOOST_TEST(!fwd::verify("k", h.substr(0, colon), body, t0));         // no MAC
    BOOST_TEST(!fwd::verify("k", "x" + h, body, t0));                    // non-digit
    BOOST_TEST(!fwd::verify("k", h.substr(0, h.size() - 1), body, t0));  // short MAC
    BOOST_TEST(!fwd::verify("k", std::string(20, '1') + h.substr(colon), body, t0));
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

/// What `submit_command` hands back: `raft_stream_cluster` only calls
/// `get()`, so a settled value or exception stands in for any future type.
struct settled {
    std::vector<std::byte> _value;
    std::exception_ptr _error;

    auto get() -> std::vector<std::byte> {
        if (_error) {
            std::rethrow_exception(_error);
        }
        return std::move(_value);
    }
};

/// A stand-in for `multi_raft`: one group, a leader that may be elsewhere.
struct fake_host {
    using node_id_type = std::string;
    using group_id_type = std::uint64_t;
    using state_machine_type = kin::stream_state_machine<std::uint64_t>;

    struct descriptor {
        group_id_type _group_id;
    };

    std::string _self = "n1";
    std::optional<std::string> _leader = "n1";
    bool _routed = true;
    bool _throw_other = false;
    bool _hint_self = false;  // not leader, yet the hint names this node
    kin::stream_state_machine<std::uint64_t> _sm;
    std::uint64_t _index = 0;
    std::vector<std::string> _submitted;

    [[nodiscard]] auto node_id() const -> const std::string& { return _self; }

    [[nodiscard]] auto resolve(const std::string& /*key*/) const -> std::optional<descriptor> {
        if (!_routed) {
            return std::nullopt;
        }
        return descriptor{._group_id = 7};
    }

    template<typename F>
    auto with_group_state_machine(group_id_type group, F&& f)
        -> std::optional<decltype(f(_sm, descriptor{}))> {
        return std::forward<F>(f)(_sm, descriptor{._group_id = group});
    }

    auto submit_command(const std::string& key, const std::vector<std::byte>& command,
                        std::chrono::milliseconds /*timeout*/) -> settled {
        _submitted.push_back(key);
        if (_throw_other) {
            return {._value = {},
                    ._error = std::make_exception_ptr(std::runtime_error("timed out"))};
        }
        if (_hint_self || _leader != _self) {
            return {
                ._value = {},
                ._error = std::make_exception_ptr(
                    kythira::shard_not_leader_exception<group_id_type, node_id_type>(7, _leader))};
        }
        return {._value = _sm.apply(command, ++_index), ._error = nullptr};
    }
};

auto create(const std::string& name) -> std::vector<std::byte> {
    return kin::encode_command(kin::create_stream_command{
        ._account = "111122223333", ._name = name, ._shard_count = 1, ._created_ms = 1});
}

struct forward_log {
    std::vector<std::string> _leaders;
    std::vector<kin::admission> _modes;
    std::vector<std::vector<std::byte>> _commands;
    std::optional<kin::apply_result> _answer = kin::apply_result{._id = 99};
};

auto make_forwarder(forward_log& log) {
    return [&log](const std::string& leader, const std::string& /*key*/,
                  const std::vector<std::byte>& command, kin::admission mode) {
        log._leaders.push_back(leader);
        log._modes.push_back(mode);
        log._commands.push_back(command);
        return log._answer;
    };
}

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_cluster)

BOOST_AUTO_TEST_CASE(the_leader_proposes_locally) {
    fake_host host;
    forward_log log;
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log));
    const auto r = c.propose("k", create("s"));
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST((r->_status == kin::apply_status::ok));
    BOOST_TEST(log._leaders.empty());
    BOOST_TEST(c.forwarded_count() == 0U);
    // Reads see the local replica.
    bool seen = false;
    BOOST_TEST(c.read(
        "k", [&seen](const auto& sm) { seen = sm.find_stream("111122223333", "s") != nullptr; }));
    BOOST_TEST(seen);
}

BOOST_AUTO_TEST_CASE(a_follower_forwards_to_the_hinted_leader) {
    fake_host host;
    host._leader = "n2";
    forward_log log;
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log));
    const auto r = c.propose("k", create("s"));
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST(r->_id == 99U);
    BOOST_TEST_REQUIRE(log._leaders.size() == 1U);
    BOOST_TEST(log._leaders[0] == "n2");
    BOOST_TEST(c.forwarded_count() == 1U);
    // An unreachable leader is "not now".
    log._answer.reset();
    BOOST_TEST(!c.propose("k", create("s")).has_value());
}

BOOST_AUTO_TEST_CASE(a_forwarded_proposal_is_never_forwarded_again) {
    fake_host host;
    host._leader = "n2";
    forward_log log;
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log));
    BOOST_TEST(!c.propose_local("k", create("s")).has_value());
    BOOST_TEST(log._leaders.empty());
}

BOOST_AUTO_TEST_CASE(no_hint_a_self_hint_or_no_forwarder_fails_as_unavailable) {
    fake_host host;
    forward_log log;
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log));
    host._leader.reset();
    BOOST_TEST(!c.propose("k", create("s")).has_value());
    // A stale hint naming this node itself must not loop.
    host._leader = "n1";
    host._hint_self = true;
    BOOST_TEST(!c.propose("k", create("s")).has_value());
    BOOST_TEST(log._leaders.empty());

    fake_host other;
    other._leader = "n2";
    kin::raft_stream_cluster<fake_host> bare(other, {});
    BOOST_TEST(!bare.propose("k", create("s")).has_value());
}

BOOST_AUTO_TEST_CASE(other_failures_and_unrouted_keys_are_not_now) {
    fake_host host;
    forward_log log;
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log));
    host._throw_other = true;
    BOOST_TEST(!c.propose("k", create("s")).has_value());
    host._routed = false;
    BOOST_TEST(!c.read("k", [](const auto&) {}));
    BOOST_TEST(log._leaders.empty());
}

namespace {

/// A stream with one open shard on `host`'s state machine, and an append of
/// `n` 100-byte records to it.
auto open_shard(fake_host& host) -> std::string {
    (void)host._sm.apply(create("s"), ++host._index);
    const auto key = kin::routing_key(1, 0);
    (void)host._sm.apply(kin::encode_command(kin::open_shard_command{key, 0}), ++host._index);
    return key;
}

auto append(const std::string& key, std::size_t n) -> std::vector<std::byte> {
    kin::append_records_command a;
    a._key = key;
    for (std::size_t i = 0; i < n; ++i) {
        a._records.push_back({"pk", std::vector<std::byte>(100, std::byte{1})});
    }
    return kin::encode_command(a);
}

auto count(const kin::apply_result& r) -> std::uint64_t {
    return r._last_counter - r._first_counter + 1;
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_leader_admits_writes_against_its_buckets) {
    fake_host host;
    const auto key = open_shard(host);
    forward_log log;
    std::int64_t now = 0;
    kin::write_throttle throttle({}, [&now] { return now; });
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log), std::chrono::seconds{5},
                                          &throttle);
    // 1000 records/s: 600, then 400 of the next 600, then nothing.
    auto r = c.propose(key, append(key, 600), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST(count(*r) == 600U);
    r = c.propose(key, append(key, 600), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST((r->_status == kin::apply_status::ok));
    BOOST_TEST(count(*r) == 400U);
    r = c.propose(key, append(key, 1), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST((r->_status == kin::apply_status::throttled));
    // Without admission the switch is off: everything goes through.
    r = c.propose(key, append(key, 5), kin::admission::none);
    BOOST_TEST((r && r->_status == kin::apply_status::ok));
    // Half a second refills half the bucket.
    now += 500;
    r = c.propose(key, append(key, 600), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST(count(*r) == 500U);
}

BOOST_AUTO_TEST_CASE(the_byte_limit_admits_a_prefix) {
    fake_host host;
    const auto key = open_shard(host);
    forward_log log;
    std::int64_t now = 0;
    kin::write_throttle throttle({}, [&now] { return now; });
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log), std::chrono::seconds{5},
                                          &throttle);
    kin::append_records_command big;
    big._key = key;
    for (int i = 0; i < 4; ++i) {
        // 400 KiB each with its key. A record is taken while the 1 MiB
        // bucket is still positive before it: the third overdraws it and
        // the fourth waits.
        big._records.push_back({"pk", std::vector<std::byte>(400 * 1024 - 2, std::byte{2})});
    }
    const auto r = c.propose(key, kin::encode_command(big), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST(count(*r) == 3U);
}

BOOST_AUTO_TEST_CASE(a_follower_refunds_and_the_leader_admits) {
    fake_host host;
    const auto key = open_shard(host);
    host._leader = "n2";
    forward_log log;
    std::int64_t now = 0;
    kin::write_throttle throttle({}, [&now] { return now; });
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log), std::chrono::seconds{5},
                                          &throttle);
    for (int i = 0; i < 3; ++i) {
        // Each would drain the bucket if the follower kept its tokens.
        (void)c.propose(key, append(key, 900), kin::admission::shard_limits);
    }
    BOOST_TEST_REQUIRE(log._modes.size() == 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST((log._modes[i] == kin::admission::shard_limits));
        // The whole command goes to the leader, which admits it itself.
        BOOST_TEST((log._commands[i] == append(key, 900)));
    }
    host._leader = "n1";
    const auto r = c.propose(key, append(key, 900), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST(count(*r) == 900U);
}

BOOST_AUTO_TEST_CASE(a_batch_answers_throttled_parts_in_place) {
    fake_host host;
    (void)host._sm.apply(create("s"), ++host._index);
    std::vector<std::string> keys;
    for (std::uint32_t o = 0; o < 3; ++o) {
        keys.push_back(kin::routing_key(1, o));
        (void)host._sm.apply(kin::encode_command(kin::open_shard_command{keys.back(), 0}),
                             ++host._index);
    }
    forward_log log;
    std::int64_t now = 0;
    kin::write_throttle throttle({}, [&now] { return now; });
    kin::raft_stream_cluster<fake_host> c(host, make_forwarder(log), std::chrono::seconds{5},
                                          &throttle);
    // Drain shard 1.
    (void)c.propose(keys[1], append(keys[1], 1000), kin::admission::shard_limits);
    kin::append_batch_command b;
    for (const auto& k : keys) {
        b._appends.push_back(
            std::get<kin::append_records_command>(kin::decode_command(append(k, 10))));
    }
    const auto r = c.propose(keys[0], kin::encode_command(b), kin::admission::shard_limits);
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST_REQUIRE(r->_parts.size() == 3U);
    BOOST_TEST((r->_parts[0]._status == kin::apply_status::ok));
    BOOST_TEST((r->_parts[1]._status == kin::apply_status::throttled));
    BOOST_TEST((r->_parts[2]._status == kin::apply_status::ok));
    BOOST_TEST(r->_parts[2]._last_counter == 10U);
}

BOOST_AUTO_TEST_SUITE_END()

static_assert(kin::stream_cluster<kin::raft_stream_cluster<fake_host>>);
