// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE object_store_batched_layout_unit_test
#include <boost/test/unit_test.hpp>

#include "legacy_layout_reader.hpp"
#include "mock_object_store.hpp"
#include "object_store_crash_oracle.hpp"

#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Engine-level coverage for `.kiro/specs/object-store-batched-layout/` that is
// not a conformance case: the test infrastructure itself (crash injection, the
// crash oracle, the frozen legacy reader), and the combined hard state's request
// log, codec, load precedence and refusals. The conformance suite runs over the
// combined layout in object_store_persistence_unit_test.cpp.
//
// The crash cases follow Requirement 15.3: for every n from 0 to an operation's
// request count, the store "crashes" after its n-th request; a fresh engine then
// loads, its state must be one the interrupted call's contract permits, its
// next mutation must succeed, and a further fresh engine must read back exactly
// what that one holds.

namespace {

namespace legacy = kythira::legacy_layout;
namespace oracle = kythira::crash_oracle;

using log_entry_t = kythira::log_entry<std::uint64_t, std::uint64_t>;
using store_t = kythira::mock_object_store;
using engine_t = kythira::object_store_persistence_engine<store_t>;
using fenced_engine_t = kythira::fenced_object_store_persistence_engine<store_t>;
using text_engine_t = kythira::object_store_persistence_engine<store_t, std::string>;
using model_t = oracle::model<std::uint64_t>;

const std::string k_bucket = "kythira";
const std::string k_prefix = "raft";
const std::string k_term = k_prefix + "/term";
const std::string k_vote = k_prefix + "/voted_for";
const std::string k_owner_id = "node-a";

auto make_entry(std::uint64_t term, std::uint64_t index) -> log_entry_t {
    const std::string payload = "cmd-" + std::to_string(index);
    std::vector<std::byte> command;
    for (const char c : payload) {
        command.push_back(static_cast<std::byte>(c));
    }
    return log_entry_t{term, index, std::move(command)};
}

auto make_run(std::uint64_t term, std::uint64_t first, std::uint64_t count)
    -> std::vector<log_entry_t> {
    std::vector<log_entry_t> run;
    for (std::uint64_t i = 0; i < count; ++i) {
        run.push_back(make_entry(term, first + i));
    }
    return run;
}

auto separate() -> kythira::object_persistence_options {
    return {};
}

auto combined() -> kythira::object_persistence_options {
    kythira::object_persistence_options opts;
    opts.hard_state = kythira::hard_state_layout::combined;
    return opts;
}

auto fenced(kythira::object_persistence_options opts) -> kythira::object_persistence_options {
    opts.owner_id = k_owner_id;
    return opts;
}

/// Construction must fail with a `std::runtime_error` whose message contains
/// every one of `needles`.
template<typename Engine>
auto construction_fails_naming(const store_t& store,
                               const kythira::object_persistence_options& opts,
                               const std::vector<std::string>& needles) -> bool {
    try {
        Engine eng{store, k_bucket, k_prefix, opts};
        BOOST_ERROR("construction was expected to fail");
        return false;
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        bool all = true;
        for (const auto& needle : needles) {
            if (what.find(needle) == std::string::npos) {
                BOOST_ERROR("message lacks \"" + needle + "\": " + what);
                all = false;
            }
        }
        return all;
    }
}

/// Requirement 15.3 for one operation over one engine type: arm a crash after
/// every n from 0 until the operation completes uninterrupted, and check each
/// outcome against the oracle. `setup` brings a fresh store to the pre-call
/// state; `op` is the interrupted call; `next` is "its next mutation".
///
/// Returns the operation's request count — the n at which it first completed —
/// so a case can pin it.
template<typename Engine>
auto crash_at_every_request(const kythira::object_persistence_options& opts, oracle::operation kind,
                            const std::function<void(Engine&)>& setup,
                            const std::function<void(Engine&)>& op,
                            const std::function<void(Engine&)>& next) -> std::size_t {
    // The uninterrupted run, for `post` and the request count.
    model_t pre;
    model_t post;
    std::size_t requests = 0;
    {
        store_t store;
        Engine eng{store, k_bucket, k_prefix, opts};
        setup(eng);
        pre = oracle::capture(eng);
        store.clear_requests();
        op(eng);
        requests = store.request_log().size();
        post = oracle::capture(eng);
    }
    const auto legal = oracle::legal_loaded_states(kind, pre, post);

    for (std::size_t n = 0; n <= requests; ++n) {
        BOOST_TEST_CONTEXT("crash after request " << n << " of " << requests) {
            store_t store;
            {
                Engine eng{store, k_bucket, k_prefix, opts};
                setup(eng);
                store.crash_after_requests(n);
                if (n < requests) {
                    BOOST_CHECK_THROW(op(eng), std::exception);
                    BOOST_TEST(store.crashed());
                } else {
                    BOOST_CHECK_NO_THROW(op(eng));
                    BOOST_TEST(!store.crashed());
                }
                store.clear_crash();
            }
            Engine restarted{store, k_bucket, k_prefix, opts};
            const model_t loaded = oracle::capture(restarted);
            BOOST_TEST(oracle::is_legal(loaded, legal), "loaded " << oracle::describe(loaded));
            BOOST_CHECK_NO_THROW(next(restarted));
            const model_t after_next = oracle::capture(restarted);

            Engine again{store, k_bucket, k_prefix, opts};
            BOOST_TEST((oracle::capture(again) == after_next),
                       "reloaded " << oracle::describe(oracle::capture(again)) << ", expected "
                                   << oracle::describe(after_next));
        }
    }
    return requests;
}

}  // namespace

// ── Test infrastructure (task 2) ─────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(crash_injection_infrastructure)

// The knob itself: n requests are served, every one after them is refused
// before it is logged or applied, and clearing it restores the store.
BOOST_AUTO_TEST_CASE(crash_after_requests_serves_n_and_refuses_the_rest_without_applying_them) {
    store_t store;
    store.crash_after_requests(2);
    BOOST_CHECK_NO_THROW(static_cast<void>(store.put_object(k_bucket, "a", "1")));
    BOOST_CHECK_NO_THROW(static_cast<void>(store.get_object(k_bucket, "a")));
    BOOST_TEST(!store.crashed());

    BOOST_CHECK_THROW(static_cast<void>(store.put_object(k_bucket, "b", "2")), std::runtime_error);
    BOOST_CHECK_THROW(store.delete_object(k_bucket, "a"), std::runtime_error);
    BOOST_CHECK_THROW(static_cast<void>(store.list_keys(k_bucket, "")), std::runtime_error);
    BOOST_CHECK_THROW(static_cast<void>(store.put_object_if(
                          k_bucket, "c", "3", kythira::precondition{kythira::if_absent{}})),
                      std::runtime_error);
    BOOST_TEST(store.crashed());
    // Nothing after the second request reached the bucket or the log.
    BOOST_TEST(store.has("a"));
    BOOST_TEST(!store.has("b"));
    BOOST_TEST(!store.has("c"));
    BOOST_TEST(store.request_log().size() == 2U);

    store.clear_crash();
    BOOST_TEST(!store.crashed());
    BOOST_CHECK_NO_THROW(static_cast<void>(store.put_object(k_bucket, "b", "2")));
    BOOST_TEST(store.has("b"));
}

// n = 0 is "the process died before its first request".
BOOST_AUTO_TEST_CASE(a_crash_after_zero_requests_refuses_the_first) {
    store_t store;
    store.crash_after_requests(0);
    BOOST_CHECK_THROW(static_cast<void>(store.put_object(k_bucket, "a", "1")), std::runtime_error);
    BOOST_TEST(store.keys().empty());
    BOOST_TEST(store.request_log().empty());
}

// The oracle's sets, pinned on small models so a case that relies on them is
// relying on something checked.
BOOST_AUTO_TEST_CASE(the_oracle_lists_exactly_the_states_each_operation_permits) {
    model_t pre;
    pre.term = 4;
    pre.vote = 1;
    model_t post;
    post.term = 5;
    post.vote = 2;

    const auto atomic =
        oracle::legal_loaded_states(oracle::operation::atomic_hard_state, pre, post);
    BOOST_TEST(atomic.size() == 2U);

    const auto ordered = oracle::legal_loaded_states(oracle::operation::term_then_vote, pre, post);
    BOOST_TEST(ordered.size() == 3U);
    model_t torn = pre;
    torn.term = 5;
    BOOST_TEST(oracle::is_legal(torn, ordered));
    // Old term with the new vote is a second vote in a term already voted in.
    model_t backwards = pre;
    backwards.vote = 2;
    BOOST_TEST(!oracle::is_legal(backwards, ordered));
    BOOST_TEST(!oracle::is_legal(torn, atomic));

    model_t log_pre;
    model_t log_post;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        log_pre.log[i] = oracle::entry_fact{1, {}, 0};
    }
    log_post = log_pre;
    for (std::uint64_t i = 4; i <= 6; ++i) {
        log_post.log[i] = oracle::entry_fact{1, {}, 0};
    }
    BOOST_TEST(
        oracle::legal_loaded_states(oracle::operation::extend_log, log_pre, log_post).size() == 4U);
    BOOST_TEST(
        oracle::legal_loaded_states(oracle::operation::truncate_log, log_post, log_pre).size() ==
        4U);
    model_t compacted = log_post;
    compacted.log.erase(1);
    compacted.log.erase(2);
    const auto compact =
        oracle::legal_loaded_states(oracle::operation::compact_log, log_post, compacted);
    BOOST_TEST(compact.size() == 3U);
    // A suffix, never a prefix: compaction goes lowest first.
    model_t prefix_only = log_post;
    prefix_only.log.erase(6);
    BOOST_TEST(!oracle::is_legal(prefix_only, compact));
}

BOOST_AUTO_TEST_SUITE_END()

// ── The default layout under crash injection: today's behaviour, pinned ───────
//
// No behaviour change is asked of task 2; these cases run the new
// infrastructure against the shipped engine, so a later layout that fails the
// same cases is failing something the default already passes.

BOOST_AUTO_TEST_SUITE(default_layout_crash_injection)

BOOST_AUTO_TEST_CASE(save_hard_state_at_every_request_is_term_then_vote) {
    const std::size_t requests = crash_at_every_request<engine_t>(
        separate(), oracle::operation::term_then_vote, [](engine_t& e) { e.save_hard_state(4, 1); },
        [](engine_t& e) { e.save_hard_state(5, 2); }, [](engine_t& e) { e.save_hard_state(6, 3); });
    BOOST_TEST(requests == 2U);
}

BOOST_AUTO_TEST_CASE(a_batched_append_at_every_request_loads_a_prefix_of_the_run) {
    kythira::object_persistence_options opts = separate();
    opts.append_concurrency = 1;
    const auto run = make_run(1, 3, 4);
    const std::size_t requests = crash_at_every_request<engine_t>(
        opts, oracle::operation::extend_log,
        [](engine_t& e) {
            e.append_log_entry(make_entry(1, 1));
            e.append_log_entry(make_entry(1, 2));
        },
        [&run](engine_t& e) { e.append_log_entries(run); },
        [](engine_t& e) {
            const auto last = e.get_last_log_index();
            e.append_log_entry(make_entry(2, last + 1));
        });
    BOOST_TEST(requests == 4U);
}

BOOST_AUTO_TEST_CASE(a_truncation_at_every_request_loads_a_prefix_no_shorter_than_the_result) {
    const std::size_t requests = crash_at_every_request<engine_t>(
        separate(), oracle::operation::truncate_log,
        [](engine_t& e) {
            for (std::uint64_t i = 1; i <= 5; ++i) {
                e.append_log_entry(make_entry(1, i));
            }
        },
        [](engine_t& e) { e.truncate_log(3); },
        [](engine_t& e) { e.append_log_entry(make_entry(2, e.get_last_log_index() + 1)); });
    BOOST_TEST(requests == 3U);
}

BOOST_AUTO_TEST_CASE(a_compaction_at_every_request_loads_a_suffix_no_shorter_than_the_result) {
    const std::size_t requests = crash_at_every_request<engine_t>(
        separate(), oracle::operation::compact_log,
        [](engine_t& e) {
            for (std::uint64_t i = 1; i <= 5; ++i) {
                e.append_log_entry(make_entry(1, i));
            }
        },
        [](engine_t& e) { e.delete_log_entries_before(4); },
        [](engine_t& e) { e.append_log_entry(make_entry(2, e.get_last_log_index() + 1)); });
    BOOST_TEST(requests == 3U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── The frozen legacy reader ─────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(legacy_layout_reader_suite)

// The control without which every "refused" below proves nothing: the frozen
// predicates accept what the default layout writes.
BOOST_AUTO_TEST_CASE(the_legacy_reader_accepts_every_body_the_default_layout_writes) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, separate()};
    eng.save_hard_state(42, 7);
    eng.append_log_entry(make_entry(42, 1));

    BOOST_TEST(legacy::parse_unsigned(store.body(k_term), k_term) == 42U);
    BOOST_TEST(legacy::parse_u64(store.body(k_term), k_term) == 42U);
    BOOST_TEST(legacy::parse_unsigned(store.body(k_vote), k_vote) == 7U);
    const std::string log_key = eng.log_key(1);
    BOOST_TEST(legacy::json_to_entry(store.body(log_key)).index() == 1U);
    BOOST_TEST(legacy::check_entry_term_parse(store.body(log_key)) == 42U);
    BOOST_TEST(!legacy::engine_refuses_term(store.body(k_term)));
    BOOST_TEST(!legacy::backup_refuses_term(store.body(k_term)));
    BOOST_TEST(!legacy::engine_refuses_log_object(store.body(log_key)));
    BOOST_TEST(!legacy::backup_refuses_log_object(store.body(log_key)));
}

// Requirement 15.5 for the combined hard state: every body a combined engine
// writes to `<prefix>/term` is refused by both older readers of that key.
BOOST_AUTO_TEST_CASE(the_legacy_reader_rejects_every_combined_term_body) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, combined()};
    std::vector<std::string> bodies;
    const auto record = [&] { bodies.push_back(store.body(k_term)); };

    eng.save_current_term(0);
    record();
    eng.save_current_term(1);
    record();
    eng.save_voted_for(3);
    record();
    eng.save_hard_state(2, std::nullopt);
    record();
    eng.save_hard_state(9, 0);
    record();
    eng.save_hard_state(std::numeric_limits<std::uint64_t>::max(), 9);
    record();

    text_engine_t text{store_t{}, k_bucket, k_prefix, combined()};
    text.save_hard_state(5, std::string("12"));
    bodies.push_back(text.store().body(k_term));
    text.save_voted_for("node-b");
    bodies.push_back(text.store().body(k_term));

    for (const auto& body : bodies) {
        BOOST_TEST_CONTEXT("body " << body) {
            BOOST_TEST(legacy::engine_refuses_term(body));
            BOOST_TEST(legacy::backup_refuses_term(body));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── Combined hard state: requests and bodies (Requirement 2) ─────────────────

BOOST_AUTO_TEST_SUITE(combined_hard_state_requests)

// Property 2's failure mode, pinned: the combined layout quietly still writing
// `voted_for` "for compatibility" would double the election cost while every
// read still looked right.
BOOST_AUTO_TEST_CASE(each_hard_state_write_is_one_put_to_term_and_none_to_voted_for) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, combined()};
    store.clear_requests();

    eng.save_hard_state(3, 1);
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());

    store.clear_requests();
    eng.save_current_term(4);
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());

    store.clear_requests();
    eng.save_voted_for(2);
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());

    // A candidate's term + 1 and self-vote, and a follower's grant at a newer
    // term: both values change, still one PUT.
    store.clear_requests();
    eng.save_hard_state(5, 5);
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());

    BOOST_TEST(!store.has(k_vote));
    BOOST_TEST(store.body(k_term) == "{\"term\":5,\"voted_for\":5}");
}

// The same under fencing, where the one PUT is also the one conditional write.
BOOST_AUTO_TEST_CASE(a_fenced_combined_write_is_one_conditional_put) {
    store_t store;
    fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
    store.clear_requests();
    eng.save_hard_state(1, 1);
    eng.save_hard_state(2, 2);
    BOOST_TEST(store.count_requests("PUT " + k_term) == 2U);
    BOOST_TEST(store.count_requests("PUT " + k_vote) == 0U);
    BOOST_TEST(store.request_log().size() == 2U);
    BOOST_TEST(!eng.is_fenced());
}

// Requirement 2.3: nothing changed, nothing sent.
BOOST_AUTO_TEST_CASE(an_unchanged_save_hard_state_sends_nothing) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, combined()};
    eng.save_hard_state(3, 1);
    store.clear_requests();
    eng.save_hard_state(3, 1);
    BOOST_TEST(store.request_log().empty());

    // …including the cold-start pair, which is already what the mirror holds.
    store_t fresh;
    engine_t cold{fresh, k_bucket, k_prefix, combined()};
    fresh.clear_requests();
    cold.save_hard_state(0, std::nullopt);
    BOOST_TEST(fresh.request_log().empty());
    BOOST_TEST(fresh.keys().empty());
}

// Requirement 2.3's other half: each single-value method carries the mirror's
// current value of the other.
BOOST_AUTO_TEST_CASE(the_single_value_methods_carry_the_other_value_from_the_mirror) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, combined()};
    eng.save_voted_for(7);
    BOOST_TEST(store.body(k_term) == "{\"term\":0,\"voted_for\":7}");
    eng.save_current_term(8);
    BOOST_TEST(store.body(k_term) == "{\"term\":8,\"voted_for\":7}");
    eng.save_hard_state(9, std::nullopt);
    BOOST_TEST(store.body(k_term) == "{\"term\":9,\"voted_for\":null}");
    eng.save_voted_for(1);
    BOOST_TEST(store.body(k_term) == "{\"term\":9,\"voted_for\":1}");

    engine_t reader{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(reader.load_current_term() == 9U);
    BOOST_TEST(reader.load_voted_for().value() == 1U);
}

// `to_number<std::uint64_t>`, not the entry codec's `as_int64()`: the whole
// TermId range survives.
BOOST_AUTO_TEST_CASE(the_full_term_range_round_trips) {
    store_t store;
    constexpr auto k_max = std::numeric_limits<std::uint64_t>::max();
    {
        engine_t eng{store, k_bucket, k_prefix, combined()};
        eng.save_hard_state(k_max, k_max);
    }
    BOOST_TEST(store.body(k_term) ==
               "{\"term\":18446744073709551615,\"voted_for\":18446744073709551615}");
    engine_t reader{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(reader.load_current_term() == k_max);
    BOOST_TEST(reader.load_voted_for().value() == k_max);
}

// A string NodeId is a JSON string, and survives a reload as itself.
BOOST_AUTO_TEST_CASE(a_string_node_id_is_a_json_string) {
    store_t store;
    {
        text_engine_t eng{store, k_bucket, k_prefix, combined()};
        eng.save_hard_state(3, std::string("node-b"));
    }
    BOOST_TEST(store.body(k_term) == "{\"term\":3,\"voted_for\":\"node-b\"}");
    text_engine_t reader{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(reader.load_voted_for().value() == "node-b");
    // A digit-only string id stays a string, not a number.
    reader.save_voted_for("12");
    BOOST_TEST(store.body(k_term) == "{\"term\":3,\"voted_for\":\"12\"}");
}

// Requirement 2.4: the mirror moves only after the store acknowledges.
BOOST_AUTO_TEST_CASE(a_failed_combined_write_leaves_both_values_unchanged) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, combined()};
    eng.save_hard_state(2, 1);
    store.fail_puts_for_key(k_term);
    BOOST_CHECK_THROW(eng.save_hard_state(3, 2), std::runtime_error);
    BOOST_CHECK_THROW(eng.save_current_term(4), std::runtime_error);
    BOOST_CHECK_THROW(eng.save_voted_for(5), std::runtime_error);
    store.fail_puts_for_key("");

    BOOST_TEST(eng.load_current_term() == 2U);
    BOOST_TEST(eng.load_voted_for().value() == 1U);
    BOOST_TEST(store.body(k_term) == "{\"term\":2,\"voted_for\":1}");
}

// Requirement 4.1: create-only on a prefix that has never held the record, so
// two fresh engines racing for one prefix cannot both write it.
BOOST_AUTO_TEST_CASE(the_first_fenced_combined_write_is_create_only) {
    store_t store;
    fenced_engine_t a{store, k_bucket, k_prefix, fenced(combined())};
    fenced_engine_t b{store, k_bucket, k_prefix, fenced(combined())};
    BOOST_CHECK_NO_THROW(a.save_hard_state(1, 1));
    BOOST_CHECK_THROW(b.save_hard_state(1, 2), kythira::persistence_fenced_error);
    BOOST_TEST(b.is_fenced());
    BOOST_TEST(store.body(k_term) == "{\"term\":1,\"voted_for\":1}");
    // The negative control: the writer whose record it is keeps writing.
    BOOST_CHECK_NO_THROW(a.save_hard_state(2, 1));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Combined hard state: load precedence and refusals (Requirements 2.6, 3) ───

BOOST_AUTO_TEST_SUITE(combined_hard_state_load)

// Requirement 2.6: every malformed combined record fails construction naming
// the key. Strictness is what makes this binary refuse a *future* format the way
// an older one refuses this.
BOOST_AUTO_TEST_CASE(every_malformed_combined_record_fails_construction_naming_the_key) {
    const std::vector<std::string> malformed{
        "{",
        "{}",
        "{\"term\":1}",
        "{\"voted_for\":null}",
        "{\"voted_for\":null,\"term\":1}",
        "{\"term\":1,\"voted_for\":null,\"extra\":0}",
        "{\"term\":1,\"vote\":null}",
        "{\"term\":\"1\",\"voted_for\":null}",
        "{\"term\":1.0,\"voted_for\":null}",
        "{\"term\":1.5,\"voted_for\":null}",
        "{\"term\":-1,\"voted_for\":null}",
        "{\"term\":null,\"voted_for\":null}",
        "{\"term\":true,\"voted_for\":null}",
        "{\"term\":18446744073709551616,\"voted_for\":null}",
        "{\"term\":1,\"voted_for\":\"7\"}",
        "{\"term\":1,\"voted_for\":7.5}",
        "{\"term\":1,\"voted_for\":-3}",
        "{\"term\":1,\"voted_for\":true}",
        "{\"term\":1,\"voted_for\":[]}",
        "{\"term\":1,\"voted_for\":{}}",
        "{\"term\":1,\"voted_for\":null}trailing",
        "{\"term\":1,\"voted_for\":null}{}",
    };
    for (const auto& body : malformed) {
        BOOST_TEST_CONTEXT("body " << body) {
            store_t store;
            store.seed(k_term, body);
            BOOST_TEST(construction_fails_naming<engine_t>(store, combined(), {k_term}));
            BOOST_TEST(
                construction_fails_naming<fenced_engine_t>(store, fenced(combined()), {k_term}));
            // A refused construction writes nothing, fenced or not.
            BOOST_TEST(store.keys() == std::vector<std::string>{k_term},
                       boost::test_tools::per_element());
        }
    }
}

// For a string NodeId the vote is a string, and a number is the wrong type.
BOOST_AUTO_TEST_CASE(a_string_node_id_engine_refuses_a_numeric_vote) {
    store_t store;
    store.seed(k_term, "{\"term\":1,\"voted_for\":7}");
    BOOST_TEST(construction_fails_naming<text_engine_t>(store, combined(), {k_term}));
}

// Requirement 3.5: a `separate` engine refuses a combined record, whatever the
// vote slot holds, and says that the downgrade is not supported.
BOOST_AUTO_TEST_CASE(a_separate_engine_refuses_a_combined_term_with_the_downgrade_message) {
    for (const auto& vote_slot :
         {std::optional<std::string>{}, std::optional<std::string>{"retired"},
          std::optional<std::string>{"7"}, std::optional<std::string>{"none"}}) {
        BOOST_TEST_CONTEXT("vote slot " << vote_slot.value_or("<absent>")) {
            store_t store;
            store.seed(k_term, "{\"term\":4,\"voted_for\":2}");
            if (vote_slot) {
                store.seed(k_vote, *vote_slot);
            }
            const std::vector<std::string> needles{k_term, "hard_state_layout::separate",
                                                   "downgrade is not supported"};
            BOOST_TEST(construction_fails_naming<engine_t>(store, separate(), needles));
            BOOST_TEST(
                construction_fails_naming<fenced_engine_t>(store, fenced(separate()), needles));
            BOOST_TEST(!store.has(k_prefix + "/owner"));
        }
    }
}

// The design's last load-precedence row: `retired` is written only after the
// combined record, so beside a decimal or absent `term` it is corruption, at
// either setting.
BOOST_AUTO_TEST_CASE(retired_beside_anything_but_a_combined_term_is_corruption) {
    for (const auto& term_slot : {std::optional<std::string>{"6"}, std::optional<std::string>{}}) {
        BOOST_TEST_CONTEXT("term slot " << term_slot.value_or("<absent>")) {
            store_t store;
            if (term_slot) {
                store.seed(k_term, *term_slot);
            }
            store.seed(k_vote, "retired");
            const std::vector<std::string> needles{"corrupt object " + k_vote, "retired"};
            BOOST_TEST(construction_fails_naming<engine_t>(store, separate(), needles));
            BOOST_TEST(construction_fails_naming<engine_t>(store, combined(), needles));
            BOOST_TEST(construction_fails_naming<text_engine_t>(store, separate(), needles));
        }
    }
}

// A combined record beside `retired` is a migrated prefix, and loads.
BOOST_AUTO_TEST_CASE(a_combined_term_beside_a_retired_vote_slot_loads) {
    store_t store;
    store.seed(k_term, "{\"term\":6,\"voted_for\":3}");
    store.seed(k_vote, "retired");
    engine_t eng{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(eng.load_current_term() == 6U);
    BOOST_TEST(eng.load_voted_for().value() == 3U);
    store.clear_requests();
    eng.save_hard_state(7, std::nullopt);
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());
    BOOST_TEST(store.body(k_vote) == "retired");
}

// Requirement 3.1: beside a combined `term` the vote slot is not state — absent
// or retired, the vote is the record's.
BOOST_AUTO_TEST_CASE(the_vote_slot_is_ignored_only_beside_a_combined_term) {
    store_t store;
    store.seed(k_term, "{\"term\":2,\"voted_for\":null}");
    {
        engine_t eng{store, k_bucket, k_prefix, combined()};
        BOOST_TEST(!eng.load_voted_for().has_value());
    }
    store.seed(k_vote, "retired");
    {
        engine_t eng{store, k_bucket, k_prefix, combined()};
        BOOST_TEST(!eng.load_voted_for().has_value());
    }
}

// Task 3's guard: a `combined` engine over a legacy prefix is refused at
// construction, because the migration that would move it in place is task 4 —
// so no build of this task can half-migrate a bucket. Every legacy shape, fenced
// and unfenced, and nothing written on the way to the refusal.
BOOST_AUTO_TEST_CASE(a_combined_engine_refuses_a_legacy_prefix_until_migration_exists) {
    struct shape {
        std::optional<std::string> term;
        std::optional<std::string> vote;
        std::string named;
    };
    const std::vector<shape> shapes{
        {"5", std::nullopt, k_term},
        {"5", "3", k_term},
        {"5", "none", k_term},
        {std::nullopt, "3", k_vote},
        {std::nullopt, "none", k_vote},
        // A combined record beside a legacy vote is a migration whose
        // retirement is pending — task 4's to complete, so refused here too.
        {"{\"term\":5,\"voted_for\":3}", "3", k_vote},
        {"{\"term\":5,\"voted_for\":3}", "none", k_vote},
    };
    for (const auto& s : shapes) {
        BOOST_TEST_CONTEXT("term " << s.term.value_or("<absent>") << ", vote "
                                   << s.vote.value_or("<absent>")) {
            store_t store;
            if (s.term) {
                store.seed(k_term, *s.term);
            }
            if (s.vote) {
                store.seed(k_vote, *s.vote);
            }
            const std::vector<std::string> before = store.keys();
            const std::vector<std::string> needles{s.named, "migration not yet supported"};
            BOOST_TEST(construction_fails_naming<engine_t>(store, combined(), needles));
            BOOST_TEST(
                construction_fails_naming<fenced_engine_t>(store, fenced(combined()), needles));
            BOOST_TEST(store.keys() == before, boost::test_tools::per_element());
            BOOST_TEST(store.count_requests("PUT") == 0U);
        }
    }
}

// The refusal is a refusal of *legacy hard state*, not of a used prefix: a
// prefix holding only a log and a snapshot — what a fresh combined engine
// writes before its first election — loads.
BOOST_AUTO_TEST_CASE(a_prefix_with_a_log_but_no_hard_state_is_not_legacy) {
    store_t store;
    {
        engine_t eng{store, k_bucket, k_prefix, combined()};
        eng.append_log_entry(make_entry(0, 1));
    }
    engine_t eng{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(eng.get_last_log_index() == 1U);
    BOOST_CHECK_NO_THROW(eng.save_hard_state(1, 1));
}

// A legacy body is still read exactly as today by a `separate` engine, so the
// default's load path is unchanged next to the new one.
BOOST_AUTO_TEST_CASE(a_separate_engine_reads_a_legacy_prefix_as_it_always_has) {
    store_t store;
    store.seed(k_term, "12\n");
    store.seed(k_vote, "4");
    store.clear_requests();
    engine_t eng{store, k_bucket, k_prefix, separate()};
    BOOST_TEST(eng.load_current_term() == 12U);
    BOOST_TEST(eng.load_voted_for().value() == 4U);
    const std::vector<std::string> expected{"LIST " + k_prefix + "/", "GET " + k_term,
                                            "GET " + k_vote};
    BOOST_TEST(store.request_log() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Combined hard state under crash injection (Requirement 15.3) ──────────────

BOOST_AUTO_TEST_SUITE(combined_hard_state_crash_injection)

// One request per method, so n = 0 and n = 1 are every boundary there is: the
// fresh engine reads the old pair or the new one, never anything between.
template<typename Engine>
auto crash_each_hard_state_method(const kythira::object_persistence_options& opts) -> void {
    const auto setup = [](Engine& e) { e.save_hard_state(4, 1); };
    const auto next = [](Engine& e) { e.save_hard_state(e.load_current_term() + 1, 9); };

    BOOST_TEST_CONTEXT("save_hard_state") {
        BOOST_TEST((crash_at_every_request<Engine>(
                        opts, oracle::operation::atomic_hard_state, setup,
                        [](Engine& e) { e.save_hard_state(5, 2); }, next) == 1U));
    }
    BOOST_TEST_CONTEXT("save_current_term") {
        BOOST_TEST((crash_at_every_request<Engine>(
                        opts, oracle::operation::atomic_hard_state, setup,
                        [](Engine& e) { e.save_current_term(5); }, next) == 1U));
    }
    BOOST_TEST_CONTEXT("save_voted_for") {
        BOOST_TEST((crash_at_every_request<Engine>(
                        opts, oracle::operation::atomic_hard_state, setup,
                        [](Engine& e) { e.save_voted_for(2); }, next) == 1U));
    }
    // The same from a cold start, where the write is the record's creation.
    BOOST_TEST_CONTEXT("first write on a fresh prefix") {
        BOOST_TEST((crash_at_every_request<Engine>(
                        opts, oracle::operation::atomic_hard_state, [](Engine&) {},
                        [](Engine& e) { e.save_hard_state(1, 1); }, next) == 1U));
    }
}

BOOST_AUTO_TEST_CASE(each_combined_method_at_every_request_reads_the_old_or_the_new_pair) {
    crash_each_hard_state_method<engine_t>(combined());
}

BOOST_AUTO_TEST_CASE(each_fenced_combined_method_at_every_request_reads_the_old_or_the_new_pair) {
    crash_each_hard_state_method<fenced_engine_t>(fenced(combined()));
}

BOOST_AUTO_TEST_SUITE_END()
