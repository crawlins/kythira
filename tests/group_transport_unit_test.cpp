// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file group_transport_unit_test.cpp
/// @brief Unit tests for the multi-Raft transport demultiplexer (task 6) and
///        the tombstone drop path wired into it (task 8).
///
/// Two claims are load-bearing and both are asserted by `static_assert` rather
/// than by behaviour, because a failure of either would be a compile-time lie
/// that no runtime test could reach: `group_scoped_client` satisfies
/// `network_client`, and it satisfies each optional extension **exactly when
/// the inner client does**. An unconditional forwarding method would make the
/// second claim false in the direction that matters — a client advertising
/// pre-vote it cannot send.
///
/// The behavioural half is about isolation: three groups sharing one server,
/// each seeing only its own messages, and a message for a tombstoned group
/// never reaching the host's replica-creation callback at all.

#define BOOST_TEST_MODULE group_transport_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/group_storage.hpp>
#include <raft/group_transport.hpp>
#include <raft/network.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using kythira::group_rpc_handlers;
using kythira::group_rpc_messages;
using kythira::group_scoped_client;
using kythira::group_scoped_server;
using kythira::multi_group_network_server;
using kythira::tombstone_reason;
using kythira::tombstone_set;
using kythira::unknown_group_action;

namespace {

using messages = group_rpc_messages<>;
using rv_request = messages::request_vote_request_type;
using rv_response = messages::request_vote_response_type;
using pv_request = messages::request_pre_vote_request_type;
using pv_response = messages::request_pre_vote_response_type;
using ae_request = messages::append_entries_request_type;
using ae_response = messages::append_entries_response_type;
using is_request = messages::install_snapshot_request_type;
using is_response = messages::install_snapshot_response_type;
using fl_request = messages::fetch_log_entries_request_type;
using fl_response = messages::fetch_log_entries_response_type;

/// A `network_server` that keeps the handlers it is given so a test can feed
/// messages straight in, without a simulator or a socket.
///
/// The dispatch under test is entirely local to `multi_group_network_server` —
/// it happens after the real server has produced a decoded request — so a real
/// transport would add latency and flake without testing anything more.
class recording_server {
public:
    auto register_request_vote_handler(std::function<rv_response(const rv_request&)> h) -> void {
        _rv = std::move(h);
    }
    auto register_request_pre_vote_handler(std::function<pv_response(const pv_request&)> h)
        -> void {
        _pv = std::move(h);
    }
    auto register_append_entries_handler(std::function<ae_response(const ae_request&)> h) -> void {
        _ae = std::move(h);
    }
    auto register_install_snapshot_handler(std::function<is_response(const is_request&)> h)
        -> void {
        _is = std::move(h);
    }
    auto register_fetch_log_entries_handler(std::function<fl_response(const fl_request&)> h)
        -> void {
        _fl = std::move(h);
    }

    auto start() -> void { _running = true; }
    auto stop() -> void { _running = false; }
    [[nodiscard]] auto is_running() const -> bool { return _running; }

    // ── the test's way in ────────────────────────────────────────────────────
    [[nodiscard]] auto deliver(const rv_request& r) const -> rv_response { return _rv(r); }
    [[nodiscard]] auto deliver(const pv_request& r) const -> pv_response { return _pv(r); }
    [[nodiscard]] auto deliver(const ae_request& r) const -> ae_response { return _ae(r); }
    [[nodiscard]] auto deliver(const is_request& r) const -> is_response { return _is(r); }
    [[nodiscard]] auto deliver(const fl_request& r) const -> fl_response { return _fl(r); }
    [[nodiscard]] auto has_pre_vote_handler() const -> bool { return static_cast<bool>(_pv); }

private:
    std::function<rv_response(const rv_request&)> _rv;
    std::function<pv_response(const pv_request&)> _pv;
    std::function<ae_response(const ae_request&)> _ae;
    std::function<is_response(const is_request&)> _is;
    std::function<fl_response(const fl_request&)> _fl;
    bool _running{false};
};

using demux_type = multi_group_network_server<recording_server, std::uint64_t, messages>;

/// A `network_client` with only the three required sends, recording what it saw.
class minimal_client {
public:
    auto send_request_vote(std::uint64_t target, const rv_request& req, std::chrono::milliseconds)
        -> kythira::future_default<rv_response> {
        _last_vote_group = req.group_id();
        _last_target = target;
        return kythira::future_factory_default::makeFuture(
            rv_response{._term = req.term(), ._vote_granted = true, ._group_id = req.group_id()});
    }
    auto send_append_entries(std::uint64_t, const ae_request& req, std::chrono::milliseconds)
        -> kythira::future_default<ae_response> {
        _last_append_group = req.group_id();
        return kythira::future_factory_default::makeFuture(
            ae_response{._term = req.term(),
                        ._success = true,
                        ._conflict_index = std::nullopt,
                        ._conflict_term = std::nullopt,
                        ._group_id = req.group_id()});
    }
    auto send_install_snapshot(std::uint64_t, const is_request& req, std::chrono::milliseconds)
        -> kythira::future_default<is_response> {
        _last_snapshot_group = req.group_id();
        return kythira::future_factory_default::makeFuture(
            is_response{._term = req.term(), ._group_id = req.group_id()});
    }

    std::uint64_t _last_vote_group{0};
    std::uint64_t _last_append_group{0};
    std::uint64_t _last_snapshot_group{0};
    std::uint64_t _last_target{0};
};

/// The same, plus every optional extension.
class full_client : public minimal_client {
public:
    auto send_request_pre_vote(std::uint64_t, const pv_request& req, std::chrono::milliseconds)
        -> kythira::future_default<pv_response> {
        _last_pre_vote_group = req.group_id();
        return kythira::future_factory_default::makeFuture(
            pv_response{._term = req.term(), ._vote_granted = true, ._group_id = req.group_id()});
    }
    auto send_fetch_log_entries(std::uint64_t, const fl_request& req, std::chrono::milliseconds)
        -> kythira::future_default<fl_response> {
        _last_fetch_group = req.group_id();
        return kythira::future_factory_default::makeFuture(
            fl_response{._responder_id = 1,
                        ._available = true,
                        ._prev_log_term = 0,
                        ._entries = {},
                        ._group_id = req.group_id()});
    }
    auto send_cluster_join_request(const std::string&, const kythira::cluster_join_request<>&,
                                   std::chrono::milliseconds)
        -> kythira::future_default<kythira::cluster_join_response<>> {
        _join_calls++;
        return kythira::future_factory_default::makeFuture(
            kythira::cluster_join_response<>{.accepted = true, .redirect = std::nullopt});
    }
    auto send_cluster_leave_request(const std::string&, const kythira::cluster_leave_request<>&,
                                    std::chrono::milliseconds)
        -> kythira::future_default<kythira::cluster_leave_response<>> {
        _leave_calls++;
        return kythira::future_factory_default::makeFuture(
            kythira::cluster_leave_response<>{.accepted = true, .redirect = std::nullopt});
    }

    std::uint64_t _last_pre_vote_group{0};
    std::uint64_t _last_fetch_group{0};
    int _join_calls{0};
    int _leave_calls{0};
};

auto vote_for(std::uint64_t group, std::uint64_t term) -> rv_request {
    return rv_request{._term = term,
                      ._candidate_id = 1,
                      ._last_log_index = 0,
                      ._last_log_term = 0,
                      ._group_id = group};
}

auto append_for(std::uint64_t group, std::uint64_t term) -> ae_request {
    return ae_request{._term = term,
                      ._leader_id = 1,
                      ._prev_log_index = 0,
                      ._prev_log_term = 0,
                      ._entries = {},
                      ._leader_commit = 0,
                      ._group_id = group};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(group_transport_unit)

// ── concept conformance ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_scoped_views_satisfy_the_transport_concepts) {
    static_assert(kythira::network_server<recording_server>);
    static_assert(kythira::network_server<group_scoped_server<demux_type>>);
    static_assert(kythira::network_server_with_pre_vote<group_scoped_server<demux_type>>);
    static_assert(kythira::network_server_with_log_fetch<group_scoped_server<demux_type>>);

    static_assert(kythira::network_client<minimal_client>);
    static_assert(kythira::network_client<group_scoped_client<minimal_client>>);
    static_assert(kythira::network_client<group_scoped_client<full_client>>);
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(optional_client_extensions_propagate_exactly_when_the_inner_client_has_them) {
    // The direction that matters is the negative one. An unconditional
    // forwarding method would make `group_scoped_client<minimal_client>`
    // advertise pre-vote it cannot send, and `node<Types>`'s `if constexpr`
    // would then take a branch that does not compile.
    static_assert(!kythira::network_client_with_pre_vote<minimal_client>);
    static_assert(!kythira::network_client_with_pre_vote<group_scoped_client<minimal_client>>);
    static_assert(!kythira::network_client_with_log_fetch<group_scoped_client<minimal_client>>);
    static_assert(!kythira::network_client_with_cluster_join<group_scoped_client<minimal_client>>);
    static_assert(!kythira::network_client_with_cluster_leave<group_scoped_client<minimal_client>>);

    static_assert(kythira::network_client_with_pre_vote<full_client>);
    static_assert(kythira::network_client_with_pre_vote<group_scoped_client<full_client>>);
    static_assert(kythira::network_client_with_log_fetch<group_scoped_client<full_client>>);
    static_assert(kythira::network_client_with_cluster_join<group_scoped_client<full_client>>);
    static_assert(kythira::network_client_with_cluster_leave<group_scoped_client<full_client>>);
    BOOST_CHECK(true);
}

// ── stamping ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_scoped_client_stamps_its_group_on_every_outbound_request) {
    full_client inner;
    group_scoped_client<full_client> scoped{inner, 42};

    std::ignore = scoped.send_request_vote(
        7, rv_request{._term = 1, ._candidate_id = 1, ._last_log_index = 0, ._last_log_term = 0},
        std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._last_vote_group, 42u);
    BOOST_CHECK_EQUAL(inner._last_target, 7u);

    std::ignore = scoped.send_append_entries(7, ae_request{}, std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._last_append_group, 42u);

    std::ignore = scoped.send_install_snapshot(7, is_request{}, std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._last_snapshot_group, 42u);

    std::ignore = scoped.send_request_pre_vote(7, pv_request{}, std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._last_pre_vote_group, 42u);

    std::ignore = scoped.send_fetch_log_entries(7, fl_request{}, std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._last_fetch_group, 42u);
}

BOOST_AUTO_TEST_CASE(a_scoped_client_overwrites_a_group_id_the_caller_already_set) {
    // `node<Types>` builds its requests with designated initialisers and never
    // sets `_group_id`; but a caller that did must not be able to address
    // another group through this view.
    full_client inner;
    group_scoped_client<full_client> scoped{inner, 42};
    auto req = vote_for(99, 1);
    std::ignore = scoped.send_request_vote(7, req, std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._last_vote_group, 42u);
}

BOOST_AUTO_TEST_CASE(bootstrap_rpcs_are_forwarded_unstamped) {
    // cluster_join/leave are addressed by *address*, carry no group id, and are
    // a whole-node operation. Forwarding them verbatim keeps the extension
    // concept satisfied without pretending they are group-scoped.
    full_client inner;
    group_scoped_client<full_client> scoped{inner, 42};
    std::ignore = scoped.send_cluster_join_request("node-2:5000", kythira::cluster_join_request<>{},
                                                   std::chrono::milliseconds{10});
    std::ignore = scoped.send_cluster_leave_request(
        "node-2:5000", kythira::cluster_leave_request<>{}, std::chrono::milliseconds{10});
    BOOST_CHECK_EQUAL(inner._join_calls, 1);
    BOOST_CHECK_EQUAL(inner._leave_calls, 1);
}

// ── demultiplexing ───────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(three_groups_share_one_server_and_each_sees_only_its_own_messages) {
    demux_type demux{recording_server{}};

    std::vector<std::uint64_t> seen_by_1, seen_by_2, seen_by_3;
    auto install = [&demux](std::uint64_t group, std::vector<std::uint64_t>& sink) {
        group_scoped_server<demux_type> scoped{demux, group};
        scoped.register_request_vote_handler([&sink, group](const rv_request& req) {
            sink.push_back(req.term());
            return rv_response{._term = req.term(), ._vote_granted = true, ._group_id = group};
        });
        scoped.register_append_entries_handler([group](const ae_request& req) {
            return ae_response{._term = req.term(),
                               ._success = true,
                               ._conflict_index = std::nullopt,
                               ._conflict_term = std::nullopt,
                               ._group_id = group};
        });
        scoped.register_install_snapshot_handler([group](const is_request& req) {
            return is_response{._term = req.term(), ._group_id = group};
        });
    };
    install(1, seen_by_1);
    install(2, seen_by_2);
    install(3, seen_by_3);

    demux.start();
    BOOST_CHECK_EQUAL(demux.group_count(), 3u);

    // Terms are used as message identity so each sink records what it received.
    std::ignore = demux.inner().deliver(vote_for(1, 100));
    std::ignore = demux.inner().deliver(vote_for(2, 200));
    std::ignore = demux.inner().deliver(vote_for(2, 201));
    std::ignore = demux.inner().deliver(vote_for(3, 300));

    BOOST_REQUIRE_EQUAL(seen_by_1.size(), 1u);
    BOOST_CHECK_EQUAL(seen_by_1[0], 100u);
    BOOST_REQUIRE_EQUAL(seen_by_2.size(), 2u);
    BOOST_CHECK_EQUAL(seen_by_2[0], 200u);
    BOOST_CHECK_EQUAL(seen_by_2[1], 201u);
    BOOST_REQUIRE_EQUAL(seen_by_3.size(), 1u);
    BOOST_CHECK_EQUAL(seen_by_3[0], 300u);
}

BOOST_AUTO_TEST_CASE(a_response_carries_the_group_it_was_addressed_to) {
    demux_type demux{recording_server{}};
    group_scoped_server<demux_type> scoped{demux, 5};
    scoped.register_request_vote_handler([](const rv_request& req) {
        return rv_response{._term = req.term(), ._vote_granted = true, ._group_id = 5};
    });
    scoped.register_append_entries_handler([](const ae_request& req) {
        return ae_response{._term = req.term(),
                           ._success = true,
                           ._conflict_index = std::nullopt,
                           ._conflict_term = std::nullopt,
                           ._group_id = 5};
    });
    scoped.register_install_snapshot_handler(
        [](const is_request& req) { return is_response{._term = req.term(), ._group_id = 5}; });
    demux.start();

    const auto resp = demux.inner().deliver(vote_for(5, 9));
    BOOST_CHECK_EQUAL(resp.group_id(), 5u);
    BOOST_CHECK(resp.vote_granted());
}

BOOST_AUTO_TEST_CASE(the_unknown_group_callback_fires_exactly_once_per_message) {
    demux_type demux{recording_server{}};
    demux.start();

    int calls = 0;
    std::vector<std::uint64_t> asked_about;
    demux.set_unknown_group_handler([&](const std::uint64_t& g) {
        ++calls;
        asked_about.push_back(g);
        return unknown_group_action::drop;
    });

    const auto resp = demux.inner().deliver(vote_for(77, 3));
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_REQUIRE_EQUAL(asked_about.size(), 1u);
    BOOST_CHECK_EQUAL(asked_about[0], 77u);

    // A dropped message still answers, so the sender learns something rather
    // than waiting out its RPC deadline — refusing the vote at the sender's own
    // term neither grants anything nor bumps a term.
    BOOST_CHECK(!resp.vote_granted());
    BOOST_CHECK_EQUAL(resp.term(), 3u);
    BOOST_CHECK_EQUAL(resp.group_id(), 77u);
    BOOST_CHECK_EQUAL(demux.stale_group_message_count(), 1u);
}

BOOST_AUTO_TEST_CASE(a_created_replica_receives_the_message_that_created_it) {
    // Lazy replica creation: a node held offline through a split acquires the
    // child on the first inbound message rather than from a control-plane push.
    demux_type demux{recording_server{}};
    demux.start();

    std::vector<std::uint64_t> delivered;
    demux.set_unknown_group_handler([&](const std::uint64_t& g) {
        group_scoped_server<demux_type> scoped{demux, g};
        scoped.register_request_vote_handler([&delivered, g](const rv_request& req) {
            delivered.push_back(req.term());
            return rv_response{._term = req.term(), ._vote_granted = true, ._group_id = g};
        });
        scoped.register_append_entries_handler([g](const ae_request& req) {
            return ae_response{._term = req.term(),
                               ._success = true,
                               ._conflict_index = std::nullopt,
                               ._conflict_term = std::nullopt,
                               ._group_id = g};
        });
        scoped.register_install_snapshot_handler([g](const is_request& req) {
            return is_response{._term = req.term(), ._group_id = g};
        });
        return unknown_group_action::created;
    });

    const auto resp = demux.inner().deliver(vote_for(88, 4));
    BOOST_CHECK(resp.vote_granted());
    BOOST_REQUIRE_EQUAL(delivered.size(), 1u);
    BOOST_CHECK_EQUAL(delivered[0], 4u);
    BOOST_CHECK(demux.has_group(88));
    BOOST_CHECK_EQUAL(demux.stale_group_message_count(), 0u);

    // And the second message goes straight through, with no further callback.
    std::ignore = demux.inner().deliver(vote_for(88, 5));
    BOOST_CHECK_EQUAL(delivered.size(), 2u);
}

BOOST_AUTO_TEST_CASE(unregistering_a_group_sends_its_messages_back_to_the_unknown_path) {
    demux_type demux{recording_server{}};
    group_scoped_server<demux_type> scoped{demux, 6};
    scoped.register_request_vote_handler([](const rv_request& req) {
        return rv_response{._term = req.term(), ._vote_granted = true, ._group_id = 6};
    });
    scoped.register_append_entries_handler([](const ae_request& req) {
        return ae_response{._term = req.term(),
                           ._success = true,
                           ._conflict_index = std::nullopt,
                           ._conflict_term = std::nullopt,
                           ._group_id = 6};
    });
    scoped.register_install_snapshot_handler(
        [](const is_request& req) { return is_response{._term = req.term(), ._group_id = 6}; });
    demux.start();

    BOOST_CHECK(demux.inner().deliver(vote_for(6, 1)).vote_granted());
    BOOST_CHECK(demux.unregister_group(6));
    BOOST_CHECK(!demux.unregister_group(6));
    BOOST_CHECK(!demux.inner().deliver(vote_for(6, 2)).vote_granted());
    BOOST_CHECK_EQUAL(demux.stale_group_message_count(), 1u);
}

// ── tombstones (task 8) ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_tombstoned_group_is_dropped_without_reaching_the_unknown_callback) {
    // This is the whole point of the tombstone set: a partitioned peer's stale
    // AppendEntries for a merged-away group must not be allowed to re-create a
    // replica whose range someone else now owns.
    demux_type demux{recording_server{}};
    demux.start();

    tombstone_set<std::uint64_t> dead;
    dead.insert(9, tombstone_reason::merged_away, std::chrono::system_clock::now());
    demux.set_tombstone_predicate([&dead](const std::uint64_t& g) { return dead.contains(g); });

    int unknown_calls = 0;
    demux.set_unknown_group_handler([&](const std::uint64_t&) {
        ++unknown_calls;
        return unknown_group_action::created;
    });

    const auto resp = demux.inner().deliver(append_for(9, 3));
    BOOST_CHECK(!resp.success());
    BOOST_CHECK_EQUAL(resp.group_id(), 9u);
    BOOST_CHECK_EQUAL(unknown_calls, 0);
    BOOST_CHECK_EQUAL(demux.stale_group_message_count(), 1u);
    BOOST_CHECK(!demux.has_group(9));

    // A group that is *not* tombstoned still reaches the callback.
    std::ignore = demux.inner().deliver(append_for(10, 3));
    BOOST_CHECK_EQUAL(unknown_calls, 1);
}

BOOST_AUTO_TEST_SUITE_END()
