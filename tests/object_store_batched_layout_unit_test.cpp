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
#include <type_traits>
#include <utility>
#include <vector>

// Engine-level coverage for `.kiro/specs/object-store-batched-layout/` that is
// not a conformance case: the test infrastructure itself (crash injection, the
// crash oracle, the frozen legacy reader), and the combined hard state's request
// log, codec, load precedence, refusals and in-place migration. The conformance
// suite runs over the combined layout in object_store_persistence_unit_test.cpp.
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

// Task 3 refused a `combined` engine over any legacy prefix, so that no build
// could half-migrate a bucket. With the migration in place every legacy shape
// loads as the legacy pair, and construction writes nothing for it (Requirement
// 3.2) — under fencing, nothing but the owner object. A legacy vote slot is
// owed a retirement, which waits for the first hard-state write's m1.
BOOST_AUTO_TEST_CASE(a_combined_engine_loads_every_legacy_shape_and_writes_nothing_for_it) {
    struct shape {
        std::optional<std::string> term;
        std::optional<std::string> vote;
        std::uint64_t loaded_term;
        std::optional<std::uint64_t> loaded_vote;
    };
    const std::vector<shape> shapes{
        {"5", std::nullopt, 5, std::nullopt},    {"5", "3", 5, 3},
        {"5", "none", 5, std::nullopt},          {std::nullopt, "3", 0, 3},
        {std::nullopt, "none", 0, std::nullopt},
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
            {
                engine_t eng{store, k_bucket, k_prefix, combined()};
                BOOST_TEST(eng.load_current_term() == s.loaded_term);
                BOOST_TEST((eng.load_voted_for() == s.loaded_vote));
                BOOST_TEST(eng.vote_slot_retirement_pending() == s.vote.has_value());
                BOOST_TEST(store.count_requests("PUT") == 0U);
            }
            {
                fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
                BOOST_TEST(eng.load_current_term() == s.loaded_term);
                BOOST_TEST((eng.load_voted_for() == s.loaded_vote));
                BOOST_TEST(eng.vote_slot_retirement_pending() == s.vote.has_value());
                const std::vector<std::string> puts{"PUT " + k_prefix + "/owner"};
                std::vector<std::string> sent;
                for (const auto& r : store.request_log()) {
                    if (r.starts_with("PUT")) {
                        sent.push_back(r);
                    }
                }
                BOOST_TEST(sent == puts, boost::test_tools::per_element());
            }
            BOOST_TEST(store.body(k_term) == s.term.value_or(""));
            BOOST_TEST(store.body(k_vote) == s.vote.value_or(""));
        }
    }
}

// The state a crash between m1 and m2 leaves: the combined record is the pair,
// and the vote slot beside it is ignored whatever it holds (Requirement 3.1) —
// even a body no vote parser would accept — and owed a retirement.
BOOST_AUTO_TEST_CASE(a_combined_term_beside_a_legacy_vote_slot_reads_the_record) {
    for (const std::string vote_slot : {"3", "none", "", "node-seven"}) {
        BOOST_TEST_CONTEXT("vote slot \"" << vote_slot << "\"") {
            store_t store;
            store.seed(k_term, "{\"term\":5,\"voted_for\":2}");
            store.seed(k_vote, vote_slot);
            engine_t eng{store, k_bucket, k_prefix, combined()};
            BOOST_TEST(eng.load_current_term() == 5U);
            BOOST_TEST(eng.load_voted_for().value() == 2U);
            BOOST_TEST(eng.vote_slot_retirement_pending());
            BOOST_TEST(store.count_requests("PUT") == 0U);
        }
    }
}

// What stays refused: a legacy vote slot that does not parse is still
// corruption when it *is* the vote — beside a legacy or absent `term`.
BOOST_AUTO_TEST_CASE(an_unparseable_legacy_vote_beside_a_legacy_term_is_still_corruption) {
    for (const auto& term_slot : {std::optional<std::string>{"5"}, std::optional<std::string>{}}) {
        BOOST_TEST_CONTEXT("term slot " << term_slot.value_or("<absent>")) {
            store_t store;
            if (term_slot) {
                store.seed(k_term, *term_slot);
            }
            store.seed(k_vote, "node-seven");
            BOOST_TEST(construction_fails_naming<engine_t>(store, combined(),
                                                           {"corrupt object " + k_vote}));
            BOOST_TEST(construction_fails_naming<fenced_engine_t>(store, fenced(combined()),
                                                                  {"corrupt object " + k_vote}));
            BOOST_TEST(store.count_requests("PUT") == 0U);
        }
    }
}

// A prefix holding only a log — what a fresh combined engine writes before its
// first election — holds no hard state to migrate: it loads, and its first
// hard-state write creates the record with no retirement after it.
BOOST_AUTO_TEST_CASE(a_prefix_with_a_log_but_no_hard_state_is_not_legacy) {
    store_t store;
    {
        engine_t eng{store, k_bucket, k_prefix, combined()};
        eng.append_log_entry(make_entry(0, 1));
    }
    engine_t eng{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(eng.get_last_log_index() == 1U);
    BOOST_TEST(!eng.vote_slot_retirement_pending());
    store.clear_requests();
    BOOST_CHECK_NO_THROW(eng.save_hard_state(1, 1));
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());
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

// ── Combined hard state: in-place migration (Requirements 3.2-3.4, 4.2-4.4) ───

namespace {

/// The legacy prefix every migration case starts from: what a default engine of
/// the same fencing mode leaves after voting for node 1 in term 4 and
/// appending one entry — the pre-call pair `(4, 1)`.
template<typename Engine>
auto seed_legacy_prefix(const store_t& store, bool with_vote = true) -> void {
    const kythira::object_persistence_options opts =
        Engine::fencing == kythira::fencing_mode::compare_and_swap ? fenced(separate())
                                                                   : separate();
    Engine eng{store, k_bucket, k_prefix, opts};
    if (with_vote) {
        eng.save_hard_state(4, 1);
    } else {
        eng.save_current_term(4);
    }
    eng.append_log_entry(make_entry(4, 1));
}

template<typename Engine> auto combined_options() -> kythira::object_persistence_options {
    return Engine::fencing == kythira::fencing_mode::compare_and_swap ? fenced(combined())
                                                                      : combined();
}

template<typename Engine> auto separate_options() -> kythira::object_persistence_options {
    return Engine::fencing == kythira::fencing_mode::compare_and_swap ? fenced(separate())
                                                                      : separate();
}

/// The PUTs in `store`'s request log, in order.
auto puts_sent(const store_t& store) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& r : store.request_log()) {
        if (r.starts_with("PUT")) {
            out.push_back(r);
        }
    }
    return out;
}

/// The three ways `node` writes hard state, each of which may be the first
/// write on a legacy prefix and so the migration.
template<typename Engine> struct hard_state_write {
    std::string name;
    std::function<void(Engine&)> write;
    std::uint64_t term;
    std::optional<std::uint64_t> vote;
};

template<typename Engine> auto every_hard_state_write() -> std::vector<hard_state_write<Engine>> {
    return {
        {"save_hard_state", [](Engine& e) { e.save_hard_state(5, 2); }, 5, 2},
        {"save_current_term", [](Engine& e) { e.save_current_term(5); }, 5, 1},
        {"save_voted_for", [](Engine& e) { e.save_voted_for(2); }, 4, 2},
    };
}

/// Requirement 3.2 for one engine type: whichever method writes first, it is
/// m1 then m2 — and nothing else — and the prefix is migrated.
template<typename Engine> auto the_first_write_migrates() -> void {
    for (const auto& w : every_hard_state_write<Engine>()) {
        BOOST_TEST_CONTEXT(w.name) {
            store_t store;
            seed_legacy_prefix<Engine>(store);
            const std::string legacy_term_version = store.version_of(k_term);
            const std::string legacy_vote_version = store.version_of(k_vote);
            {
                Engine eng{store, k_bucket, k_prefix, combined_options<Engine>()};
                BOOST_TEST(eng.vote_slot_retirement_pending());
                store.clear_requests();
                w.write(eng);
                BOOST_TEST(puts_sent(store) ==
                               (std::vector<std::string>{"PUT " + k_term, "PUT " + k_vote}),
                           boost::test_tools::per_element());
                BOOST_TEST(store.request_log().size() == 2U);
                BOOST_TEST(!eng.vote_slot_retirement_pending());
                BOOST_TEST(!eng.last_vote_slot_retirement_error().has_value());
                BOOST_TEST(eng.load_current_term() == w.term);
                BOOST_TEST((eng.load_voted_for() == w.vote));
                BOOST_TEST(!eng.is_fenced());

                // Every later write is one PUT to `term` again.
                store.clear_requests();
                eng.save_hard_state(w.term + 1, 3);
                BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
                           boost::test_tools::per_element());
            }
            BOOST_TEST(store.version_of(k_term) != legacy_term_version);
            BOOST_TEST(store.version_of(k_vote) != legacy_vote_version);
            BOOST_TEST(store.body(k_vote) == "retired");
            BOOST_TEST(store.body(k_term) ==
                       "{\"term\":" + std::to_string(w.term + 1) + ",\"voted_for\":3}");

            Engine reader{store, k_bucket, k_prefix, combined_options<Engine>()};
            BOOST_TEST(reader.load_current_term() == w.term + 1);
            BOOST_TEST(reader.load_voted_for().value() == 3U);
            BOOST_TEST(reader.get_last_log_index() == 1U);
            BOOST_TEST(!reader.vote_slot_retirement_pending());
            BOOST_TEST(construction_fails_naming<Engine>(store, separate_options<Engine>(),
                                                         {k_term, "downgrade is not supported"}));
            BOOST_TEST(legacy::engine_refuses_term(store.body(k_term)));
            BOOST_TEST(legacy::backup_refuses_term(store.body(k_term)));
        }
    }
}

/// The crash loop over the migration's two requests, m1 and m2, with the
/// design's four-row table asserted at every boundary (Requirement 3.4):
///
/// | Crash | Store | New code reads | Older binary |
/// |---|---|---|---|
/// | before m1 lands | legacy pair | old pair | old pair |
/// | m1 landed, response lost | combined + legacy vote slot | new pair | refuses |
/// | between m1 and m2 | combined + legacy vote slot | new pair; retirement pending | refuses |
/// | m2 landed | combined + `retired` | new pair | refuses |
///
/// `lose_m1_response` turns the n = 1 run into the second row: m1 is stored and
/// its response lost, so the call is unacknowledged.
template<typename Engine> auto crash_the_migration_at_every_request(bool lose_m1_response) -> void {
    constexpr bool k_fenced = Engine::fencing == kythira::fencing_mode::compare_and_swap;
    model_t pre;
    pre.term = 4;
    pre.vote = 1;
    pre.log[1] = oracle::entry_fact{4, make_entry(4, 1).command(), 0};
    model_t post = pre;
    post.term = 5;
    post.vote = 2;
    const auto legal = oracle::legal_loaded_states(oracle::operation::atomic_hard_state, pre, post);
    constexpr std::size_t k_requests = 2;

    for (std::size_t n = 0; n <= k_requests; ++n) {
        if (lose_m1_response && n != 1) {
            continue;
        }
        BOOST_TEST_CONTEXT("crash after request "
                           << n << " of " << k_requests
                           << (lose_m1_response ? ", m1's response lost" : "")) {
            store_t store;
            seed_legacy_prefix<Engine>(store);
            {
                Engine eng{store, k_bucket, k_prefix, combined_options<Engine>()};
                BOOST_TEST((oracle::capture(eng) == pre));
                store.clear_requests();
                store.crash_after_requests(n);
                if (lose_m1_response) {
                    store.lose_next_put_responses(1);
                }
                if (n == 0 || lose_m1_response) {
                    // m1 is unacknowledged: the call fails and the mirror holds.
                    BOOST_CHECK_THROW(eng.save_hard_state(5, 2), std::exception);
                    BOOST_TEST((oracle::capture(eng) == pre));
                } else {
                    // m1 acknowledged is the commit point: a lost m2 does not
                    // fail the call (Requirement 3.3).
                    BOOST_CHECK_NO_THROW(eng.save_hard_state(5, 2));
                    BOOST_TEST((oracle::capture(eng) == post));
                    BOOST_TEST(eng.vote_slot_retirement_pending() == (n == 1));
                    BOOST_TEST(eng.last_vote_slot_retirement_error().has_value() == (n == 1));
                }
                // A fenced m1 is never retried, so with its response lost no
                // further request reaches the armed crash.
                if (!(lose_m1_response && k_fenced)) {
                    BOOST_TEST(store.crashed() == (n < k_requests));
                }
                store.clear_crash();
                store.lose_next_put_responses(0);
            }

            // The store, by row.
            const bool m1_landed = n >= 1;
            const bool m2_landed = n >= 2;
            if (m1_landed) {
                BOOST_TEST(store.body(k_term) == "{\"term\":5,\"voted_for\":2}");
            } else {
                BOOST_TEST(store.body(k_term) == "4");
            }
            BOOST_TEST(store.body(k_vote) == (m2_landed ? "retired" : "1"));

            // The older binary: the frozen parse rules, engine and backup tool.
            BOOST_TEST(legacy::engine_refuses_term(store.body(k_term)) == m1_landed);
            BOOST_TEST(legacy::backup_refuses_term(store.body(k_term)) == m1_landed);
            if (!m1_landed) {
                BOOST_TEST(legacy::parse_unsigned(store.body(k_term), k_term) == 4U);
                BOOST_TEST(legacy::parse_unsigned(store.body(k_vote), k_vote) == 1U);
            }
            // …and this binary configured `separate`, which reads the old pair
            // or refuses with the downgrade message.
            if (m1_landed) {
                BOOST_TEST(construction_fails_naming<Engine>(store, separate_options<Engine>(),
                                                             {k_term, "downgrade"}));
            } else {
                Engine old{store, k_bucket, k_prefix, separate_options<Engine>()};
                BOOST_TEST((oracle::capture(old) == pre));
            }

            // New code, after the restart.
            store.clear_requests();
            Engine restarted{store, k_bucket, k_prefix, combined_options<Engine>()};
            const model_t loaded = oracle::capture(restarted);
            BOOST_TEST(oracle::is_legal(loaded, legal), "loaded " << oracle::describe(loaded));
            BOOST_TEST((loaded == (m1_landed ? post : pre)));
            if (m1_landed && !m2_landed) {
                // Retirement pending at load: fenced construction completes
                // it, after the owner claim; unfenced construction writes
                // nothing and leaves it to the next hard-state write.
                if (k_fenced) {
                    BOOST_TEST(
                        puts_sent(store) == (std::vector<std::string>{"PUT " + k_prefix + "/owner",
                                                                      "PUT " + k_vote}),
                        boost::test_tools::per_element());
                    BOOST_TEST(store.body(k_vote) == "retired");
                    BOOST_TEST(!restarted.vote_slot_retirement_pending());
                } else {
                    BOOST_TEST(store.count_requests("PUT") == 0U);
                    BOOST_TEST(store.body(k_vote) == "1");
                    BOOST_TEST(restarted.vote_slot_retirement_pending());
                }
            }

            // Its next mutation succeeds and finishes whatever was left.
            BOOST_CHECK_NO_THROW(restarted.save_hard_state(loaded.term + 1, 9));
            BOOST_TEST(!restarted.vote_slot_retirement_pending());
            BOOST_TEST(store.body(k_vote) == "retired");
            const model_t after_next = oracle::capture(restarted);

            Engine again{store, k_bucket, k_prefix, combined_options<Engine>()};
            BOOST_TEST((oracle::capture(again) == after_next),
                       "reloaded " << oracle::describe(oracle::capture(again)) << ", expected "
                                   << oracle::describe(after_next));
            BOOST_TEST(!again.vote_slot_retirement_pending());
        }
    }
}

/// Requirement 3.3: a transient m2 failure does not fail the call that
/// committed m1; it is recorded and retried before the next hard-state write's
/// PUT, ahead of it in the request log.
template<typename Engine> auto a_transient_retirement_failure_is_retried() -> void {
    store_t store;
    seed_legacy_prefix<Engine>(store);
    Engine eng{store, k_bucket, k_prefix, combined_options<Engine>()};
    store.fail_puts_for_key(k_vote);
    BOOST_CHECK_NO_THROW(eng.save_hard_state(5, 2));
    BOOST_TEST(eng.load_current_term() == 5U);
    BOOST_TEST(eng.load_voted_for().value() == 2U);
    BOOST_TEST(eng.vote_slot_retirement_pending());
    BOOST_TEST(eng.last_vote_slot_retirement_error().has_value());
    BOOST_TEST(!eng.is_fenced());
    BOOST_TEST(store.body(k_vote) == "1");
    BOOST_TEST(store.body(k_term) == "{\"term\":5,\"voted_for\":2}");

    // A write that is not a hard-state write does not retry it.
    store.fail_puts_for_key("");
    store.clear_requests();
    eng.append_log_entry(make_entry(5, 2));
    BOOST_TEST(store.count_requests("PUT " + k_vote) == 0U);
    BOOST_TEST(eng.vote_slot_retirement_pending());

    // The next hard-state write retires first, then writes.
    store.clear_requests();
    eng.save_current_term(6);
    BOOST_TEST(store.request_log() == (std::vector<std::string>{"PUT " + k_vote, "PUT " + k_term}),
               boost::test_tools::per_element());
    BOOST_TEST(!eng.vote_slot_retirement_pending());
    BOOST_TEST(!eng.last_vote_slot_retirement_error().has_value());
    BOOST_TEST(store.body(k_vote) == "retired");

    // Nothing is owed any more.
    store.clear_requests();
    eng.save_voted_for(3);
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
               boost::test_tools::per_element());
}

/// A retry that fails again does not fail the write it precedes: the
/// retirement is housekeeping after a commit point, and the state is correct
/// without it. It stays pending and is tried again at the next write.
template<typename Engine> auto a_retry_that_fails_again_does_not_block_the_write() -> void {
    store_t store;
    seed_legacy_prefix<Engine>(store);
    Engine eng{store, k_bucket, k_prefix, combined_options<Engine>()};
    store.fail_puts_for_key(k_vote);
    eng.save_hard_state(5, 2);
    store.clear_requests();
    BOOST_CHECK_NO_THROW(eng.save_hard_state(6, 3));
    // The retry first (twice unfenced, where the PUT is retried once; once
    // fenced, where a conditional PUT never is), then the write.
    const auto log = store.request_log();
    BOOST_TEST(log.front() == "PUT " + k_vote);
    BOOST_TEST(log.back() == "PUT " + k_term);
    BOOST_TEST(store.count_requests("PUT " + k_vote) ==
               (Engine::fencing == kythira::fencing_mode::compare_and_swap ? 1U : 2U));
    BOOST_TEST(eng.vote_slot_retirement_pending());
    BOOST_TEST(store.body(k_term) == "{\"term\":6,\"voted_for\":3}");
    store.fail_puts_for_key("");
    eng.save_hard_state(7, 3);
    BOOST_TEST(!eng.vote_slot_retirement_pending());
    BOOST_TEST(store.body(k_vote) == "retired");
}

}  // namespace

BOOST_AUTO_TEST_SUITE(combined_hard_state_migration)

BOOST_AUTO_TEST_CASE(the_first_hard_state_write_on_a_legacy_prefix_is_m1_then_m2) {
    the_first_write_migrates<engine_t>();
}

BOOST_AUTO_TEST_CASE(the_first_fenced_hard_state_write_on_a_legacy_prefix_is_m1_then_m2) {
    the_first_write_migrates<fenced_engine_t>();
}

// Requirement 4.2: both migration PUTs are If-Match the legacy objects'
// versions. Shown by the store: each version the engine loaded is the one each
// PUT replaced, and an engine holding a stale one is refused (the second-writer
// cases below).
BOOST_AUTO_TEST_CASE(the_fenced_migration_is_if_match_the_legacy_versions_not_create_only) {
    store_t store;
    seed_legacy_prefix<fenced_engine_t>(store);
    fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
    // A create-only m1 or m2 would be refused against these existing objects
    // and latch a healthy node — Property 6's silent failure mode.
    BOOST_CHECK_NO_THROW(eng.save_hard_state(5, 2));
    BOOST_TEST(!eng.is_fenced());
    BOOST_TEST(store.body(k_vote) == "retired");
}

// The task's third case: no vote slot, no m2 — and under fencing m1 is still
// If-Match the legacy term.
BOOST_AUTO_TEST_CASE(a_migration_on_a_prefix_with_no_voted_for_sends_no_m2) {
    {
        store_t store;
        seed_legacy_prefix<engine_t>(store, false);
        BOOST_TEST(!store.has(k_vote));
        engine_t eng{store, k_bucket, k_prefix, combined()};
        BOOST_TEST(!eng.vote_slot_retirement_pending());
        store.clear_requests();
        eng.save_hard_state(5, 2);
        BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
                   boost::test_tools::per_element());
        BOOST_TEST(!store.has(k_vote));
    }
    {
        store_t store;
        seed_legacy_prefix<fenced_engine_t>(store, false);
        fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
        store.clear_requests();
        eng.save_hard_state(5, 2);
        BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_term},
                   boost::test_tools::per_element());
        BOOST_TEST(!store.has(k_vote));
        BOOST_TEST(!eng.is_fenced());
    }
}

// A legacy prefix with a vote slot and no `term` (a node that voted in term 0):
// m1 creates `term` — create-only under fencing, there being no legacy version
// to match — and m2 retires the slot.
BOOST_AUTO_TEST_CASE(a_legacy_prefix_with_only_a_vote_slot_migrates) {
    store_t store;
    store.seed(k_vote, "3");
    fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
    BOOST_TEST(eng.load_voted_for().value() == 3U);
    store.clear_requests();
    eng.save_current_term(1);
    BOOST_TEST(store.request_log() == (std::vector<std::string>{"PUT " + k_term, "PUT " + k_vote}),
               boost::test_tools::per_element());
    BOOST_TEST(store.body(k_term) == "{\"term\":1,\"voted_for\":3}");
    BOOST_TEST(store.body(k_vote) == "retired");
    BOOST_TEST(!eng.is_fenced());
}

// Skip-if-unchanged holds on a legacy prefix too: a write of the pair already
// loaded sends nothing, so it is not the migration.
BOOST_AUTO_TEST_CASE(an_unchanged_save_hard_state_does_not_migrate) {
    store_t store;
    seed_legacy_prefix<engine_t>(store);
    engine_t eng{store, k_bucket, k_prefix, combined()};
    store.clear_requests();
    eng.save_hard_state(4, 1);
    BOOST_TEST(store.request_log().empty());
    BOOST_TEST(store.body(k_term) == "4");
    BOOST_TEST(eng.vote_slot_retirement_pending());
}

BOOST_AUTO_TEST_CASE(a_transient_retirement_failure_is_recorded_and_retried_before_the_next_write) {
    a_transient_retirement_failure_is_retried<engine_t>();
}

BOOST_AUTO_TEST_CASE(a_fenced_transient_retirement_failure_is_recorded_and_retried) {
    a_transient_retirement_failure_is_retried<fenced_engine_t>();
}

BOOST_AUTO_TEST_CASE(a_retirement_retry_that_fails_again_does_not_fail_the_write) {
    a_retry_that_fails_again_does_not_block_the_write<engine_t>();
    a_retry_that_fails_again_does_not_block_the_write<fenced_engine_t>();
}

// Requirement 4.4: a retirement pending at load is completed during fenced
// construction, after the owner claim — and a transient failure there leaves
// construction succeeding with the retirement still pending.
BOOST_AUTO_TEST_CASE(fenced_construction_completes_a_pending_retirement) {
    store_t store;
    seed_legacy_prefix<fenced_engine_t>(store);
    {
        fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
        store.fail_puts_for_key(k_vote);
        eng.save_hard_state(5, 2);
        BOOST_TEST(eng.vote_slot_retirement_pending());
    }
    {
        // Still failing: construction succeeds, the retirement stays owed.
        store.clear_requests();
        fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
        BOOST_TEST(store.count_requests("PUT " + k_vote) == 1U);
        BOOST_TEST(eng.vote_slot_retirement_pending());
        BOOST_TEST(eng.last_vote_slot_retirement_error().has_value());
    }
    store.fail_puts_for_key("");
    store.clear_requests();
    fenced_engine_t eng{store, k_bucket, k_prefix, fenced(combined())};
    BOOST_TEST(puts_sent(store) ==
                   (std::vector<std::string>{"PUT " + k_prefix + "/owner", "PUT " + k_vote}),
               boost::test_tools::per_element());
    BOOST_TEST(!eng.vote_slot_retirement_pending());
    BOOST_TEST(store.body(k_vote) == "retired");
    BOOST_TEST(eng.load_current_term() == 5U);
}

// …and unfenced construction writes nothing for it (Requirement 3.2): the
// retirement waits for the next hard-state write.
BOOST_AUTO_TEST_CASE(unfenced_construction_leaves_a_pending_retirement_to_the_next_write) {
    store_t store;
    store.seed(k_term, "{\"term\":5,\"voted_for\":2}");
    store.seed(k_vote, "1");
    store.clear_requests();
    engine_t eng{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(store.count_requests("PUT") == 0U);
    BOOST_TEST(eng.vote_slot_retirement_pending());
    store.clear_requests();
    eng.save_hard_state(6, std::nullopt);
    BOOST_TEST(store.request_log() == (std::vector<std::string>{"PUT " + k_vote, "PUT " + k_term}),
               boost::test_tools::per_element());
    BOOST_TEST(store.body(k_vote) == "retired");
}

// Fencing, the design's first bullet: a pre-migration writer (a duplicated
// deployment of the same node, on the legacy layout) writes the vote slot
// before m2. m1 succeeds — the stale writer did not touch `term` — and m2's
// If-Match is refused, so the migrating engine latches. Detection, not
// arbitration: the committed record stays, and the latch stops this node.
BOOST_AUTO_TEST_CASE(a_second_writer_touching_voted_for_before_m2_latches_the_migrating_engine) {
    store_t store;
    seed_legacy_prefix<fenced_engine_t>(store);
    fenced_engine_t stale{store, k_bucket, k_prefix, fenced(separate())};
    fenced_engine_t migrating{store, k_bucket, k_prefix, fenced(combined())};

    BOOST_CHECK_NO_THROW(stale.save_voted_for(9));
    BOOST_TEST(store.body(k_vote) == "9");

    BOOST_CHECK_THROW(migrating.save_hard_state(5, 2), kythira::persistence_fenced_error);
    BOOST_TEST(migrating.is_fenced());
    BOOST_TEST(store.body(k_term) == "{\"term\":5,\"voted_for\":2}");
    BOOST_TEST(store.body(k_vote) == "9");
    // m1 committed before the refusal, so the mirror says what the store does.
    BOOST_TEST(migrating.load_current_term() == 5U);
    BOOST_TEST(migrating.load_voted_for().value() == 2U);

    // Latched: the next write throws without contacting the store.
    store.clear_requests();
    BOOST_CHECK_THROW(migrating.save_hard_state(6, 2), kythira::persistence_fenced_error);
    BOOST_TEST(store.request_log().empty());

    // And the refusal names the vote slot.
    try {
        migrating.save_current_term(7);
    } catch (const kythira::persistence_fenced_error& e) {
        BOOST_TEST(e.key() == k_vote);
    }
}

// The same detection when the stale write lands between a transiently failed
// m2 and its retry: the retry is refused, and the engine latches before the
// write it preceded is sent.
BOOST_AUTO_TEST_CASE(a_second_writer_inside_the_m1_m2_window_is_caught_by_the_retry) {
    store_t store;
    seed_legacy_prefix<fenced_engine_t>(store);
    fenced_engine_t stale{store, k_bucket, k_prefix, fenced(separate())};
    fenced_engine_t migrating{store, k_bucket, k_prefix, fenced(combined())};

    store.fail_puts_for_key(k_vote);
    migrating.save_hard_state(5, 2);
    store.fail_puts_for_key("");
    BOOST_TEST(migrating.vote_slot_retirement_pending());

    BOOST_CHECK_NO_THROW(stale.save_voted_for(9));
    store.clear_requests();
    BOOST_CHECK_THROW(migrating.save_hard_state(6, 2), kythira::persistence_fenced_error);
    BOOST_TEST(migrating.is_fenced());
    BOOST_TEST(store.request_log() == std::vector<std::string>{"PUT " + k_vote},
               boost::test_tools::per_element());
    BOOST_TEST(store.body(k_term) == "{\"term\":5,\"voted_for\":2}");
}

// The design's second and third bullets: once m2 has landed, the stale writer
// is the one refused — at the vote slot, whose version m2 changed, and at
// `term`, whose version m1 changed — and the migrated engine keeps writing.
BOOST_AUTO_TEST_CASE(a_second_writer_touching_voted_for_after_m2_latches_itself) {
    store_t store;
    seed_legacy_prefix<fenced_engine_t>(store);
    fenced_engine_t stale_vote{store, k_bucket, k_prefix, fenced(separate())};
    fenced_engine_t stale_term{store, k_bucket, k_prefix, fenced(separate())};
    fenced_engine_t migrating{store, k_bucket, k_prefix, fenced(combined())};

    BOOST_CHECK_NO_THROW(migrating.save_hard_state(5, 2));
    BOOST_TEST(store.body(k_vote) == "retired");

    BOOST_CHECK_THROW(stale_vote.save_voted_for(9), kythira::persistence_fenced_error);
    BOOST_TEST(stale_vote.is_fenced());
    BOOST_TEST(store.body(k_vote) == "retired");

    BOOST_CHECK_THROW(stale_term.save_current_term(6), kythira::persistence_fenced_error);
    BOOST_TEST(stale_term.is_fenced());
    BOOST_TEST(store.body(k_term) == "{\"term\":5,\"voted_for\":2}");

    // The negative control: the rightful engine is unaffected.
    BOOST_TEST(!migrating.is_fenced());
    BOOST_CHECK_NO_THROW(migrating.save_hard_state(6, 3));
}

// Requirement 3.4 / 15.3: crash at every boundary of the migration.
BOOST_AUTO_TEST_CASE(the_migration_at_every_request_matches_the_crash_table) {
    crash_the_migration_at_every_request<engine_t>(false);
}

BOOST_AUTO_TEST_CASE(the_fenced_migration_at_every_request_matches_the_crash_table) {
    crash_the_migration_at_every_request<fenced_engine_t>(false);
}

// The table's second row: m1 stored and its response lost. Unacknowledged, so
// the caller saw a failure; a restart reads the new pair, which is legal for
// an unacknowledged call, and an older binary refuses.
BOOST_AUTO_TEST_CASE(m1_landed_with_its_response_lost_reads_as_the_new_pair) {
    crash_the_migration_at_every_request<engine_t>(true);
    crash_the_migration_at_every_request<fenced_engine_t>(true);
}

// A string NodeId prefix migrates too, and its legacy vote — a bare string —
// lands in the record as a JSON string.
BOOST_AUTO_TEST_CASE(a_string_node_id_prefix_migrates) {
    store_t store;
    {
        text_engine_t eng{store, k_bucket, k_prefix, separate()};
        eng.save_hard_state(3, std::string("node-b"));
    }
    text_engine_t eng{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(eng.load_voted_for().value() == "node-b");
    eng.save_current_term(4);
    BOOST_TEST(store.body(k_term) == "{\"term\":4,\"voted_for\":\"node-b\"}");
    BOOST_TEST(store.body(k_vote) == "retired");
    text_engine_t reader{store, k_bucket, k_prefix, combined()};
    BOOST_TEST(reader.load_voted_for().value() == "node-b");
}

BOOST_AUTO_TEST_SUITE_END()

// ── Chain-walk recovery and stray hygiene (task 6) ──────────────────────────
//
// Layout-neutral: every object is still a singleton, so the walk must load
// exactly what the contiguous-run rule it replaced loaded. What changes is
// compaction, which now deletes strays before it deletes anything else.

namespace {

auto log_object_key(std::uint64_t index) -> std::string {
    std::string digits = std::to_string(index);
    return k_prefix + "/log/" + std::string(20 - digits.size(), '0') + digits;
}

auto loaded_indices(const std::vector<log_entry_t>& entries) -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> out;
    for (const auto& entry : entries) {
        out.push_back(entry.index());
    }
    return out;
}

/// Entries 1..5 on the chain, and one stray at 7 past a gap at 6 — what a
/// crash part-way through a two-entry `append_log_entries` of 6..7 leaves.
template<typename Engine>
auto seed_chain_and_stray(store_t& store, const kythira::object_persistence_options& opts) -> void {
    Engine writer{store, k_bucket, k_prefix, opts};
    for (std::uint64_t i = 1; i <= 5; ++i) {
        writer.append_log_entry(make_entry(1, i));
    }
    // A non-extending single append is written as asked: the store now holds
    // an object the chain cannot reach.
    writer.append_log_entry(make_entry(1, 7));
}

template<typename Engine> auto a_compaction_deletes_a_stray_before_anything_else() -> void {
    const auto opts = std::is_same_v<Engine, fenced_engine_t> ? fenced(separate()) : separate();
    store_t store;
    seed_chain_and_stray<Engine>(store, opts);

    Engine eng{store, k_bucket, k_prefix, opts};
    BOOST_TEST((eng.stray_log_indices() == std::vector<std::uint64_t>{7}));
    BOOST_TEST(eng.get_last_log_index() == 5U);

    store.clear_requests();
    eng.delete_log_entries_before(6);
    const auto requests = store.request_log();
    BOOST_REQUIRE(!requests.empty());
    BOOST_TEST(requests.front() == "DELETE " + log_object_key(7));
    BOOST_TEST(requests.size() == 6U);
    BOOST_TEST(eng.stray_log_indices().empty());

    // Without the stray's DELETE, entry 7 would now be the lowest log object
    // and this reload would start the log there.
    Engine reloaded{store, k_bucket, k_prefix, opts};
    BOOST_TEST(reloaded.get_last_log_index() == 0U);
    BOOST_TEST(reloaded.get_log_entries(1, 100).empty());
    BOOST_TEST(reloaded.stray_log_indices().empty());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(chain_walk_recovery)

BOOST_AUTO_TEST_CASE(the_walk_keeps_the_chain_from_the_lowest_object_and_strays_the_rest) {
    store_t store;
    {
        engine_t writer{store, k_bucket, k_prefix, separate()};
        for (const std::uint64_t i : {4, 5, 6, 8, 9, 11}) {
            writer.append_log_entry(make_entry(1, i));
        }
    }
    engine_t eng{store, k_bucket, k_prefix, separate()};
    BOOST_TEST(
        (loaded_indices(eng.get_log_entries(0, 100)) == std::vector<std::uint64_t>{4, 5, 6}));
    BOOST_TEST((eng.stray_log_indices() == std::vector<std::uint64_t>{8, 9, 11}));
}

BOOST_AUTO_TEST_CASE(a_compaction_deletes_a_stray_that_would_become_the_chain_start) {
    a_compaction_deletes_a_stray_before_anything_else<engine_t>();
}

BOOST_AUTO_TEST_CASE(a_fenced_compaction_deletes_a_stray_that_would_become_the_chain_start) {
    a_compaction_deletes_a_stray_before_anything_else<fenced_engine_t>();
}

// No stray, no change: a compaction over a clean store sends exactly the
// DELETEs it always did, lowest first.
BOOST_AUTO_TEST_CASE(a_compaction_with_no_stray_sends_only_its_deletes) {
    store_t store;
    engine_t eng{store, k_bucket, k_prefix, separate()};
    for (std::uint64_t i = 1; i <= 5; ++i) {
        eng.append_log_entry(make_entry(1, i));
    }
    store.clear_requests();
    eng.delete_log_entries_before(4);
    BOOST_TEST((store.request_log() == std::vector<std::string>{"DELETE " + log_object_key(1),
                                                                "DELETE " + log_object_key(2),
                                                                "DELETE " + log_object_key(3)}));
}

// The keep-set is the objects the run writes. Under `none` a stray the run
// overwrites is not deleted first; one it does not is.
BOOST_AUTO_TEST_CASE(a_batched_append_spares_only_the_strays_it_overwrites) {
    store_t store;
    {
        engine_t writer{store, k_bucket, k_prefix, separate()};
        for (std::uint64_t i = 1; i <= 3; ++i) {
            writer.append_log_entry(make_entry(1, i));
        }
        writer.append_log_entry(make_entry(1, 5));
        writer.append_log_entry(make_entry(1, 9));
    }
    kythira::object_persistence_options opts = separate();
    opts.append_concurrency = 1;
    engine_t eng{store, k_bucket, k_prefix, opts};
    BOOST_TEST((eng.stray_log_indices() == std::vector<std::uint64_t>{5, 9}));
    store.clear_requests();
    eng.append_log_entries(make_run(2, 4, 3));
    BOOST_TEST((store.request_log() ==
                std::vector<std::string>{"DELETE " + log_object_key(9), "PUT " + log_object_key(4),
                                         "PUT " + log_object_key(5), "PUT " + log_object_key(6)}));
    BOOST_TEST(eng.stray_log_indices().empty());
    engine_t reloaded{store, k_bucket, k_prefix, opts};
    BOOST_TEST((loaded_indices(reloaded.get_log_entries(0, 100)) ==
                std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6}));
    BOOST_TEST(reloaded.get_log_entry(5).value().term() == 2U);
}

BOOST_AUTO_TEST_SUITE_END()
