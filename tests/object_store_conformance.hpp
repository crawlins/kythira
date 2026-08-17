#pragma once

/// @file object_store_conformance.hpp
/// @brief One test body for `object_store_persistence_engine`, instantiated per
///        store.
///
/// The engine is generic over `kythira::key_object_store`, so its correctness
/// properties are proved **once** here and instantiated for every substrate:
/// the in-memory `mock_object_store`, each provider's mock server, each
/// emulator (LocalStack, Azurite), and each real bucket. That is what makes
/// five engines cost roughly one engine's worth of test review.
///
/// The division of labour is deliberate and worth keeping: **this suite proves
/// the engine**; a per-provider unit test proves that provider's *client* —
/// request shape, conditional-header spelling, listing pagination, error-to-
/// exception mapping. Neither is asked to do the other's job.
///
/// ## Instantiating it
///
/// Write a harness (see `mock_object_store_harness` in
/// `object_store_persistence_unit_test.cpp` for the reference one) and then one
/// line:
///
/// ```cpp
/// KYTHIRA_OBJECT_STORE_CONFORMANCE(my_harness, my_store)
/// ```
///
// clang-format off
/// ### The harness contract
///
/// | Member | Meaning |
/// |---|---|
/// | `using engine_t` | the engine type under test (an instantiation, or a thin derived type such as `alibaba_oss_persistence_engine`) |
/// | `make_engine()` | a fresh engine over the harness's default `{bucket, prefix}`, loading whatever is already in the store |
/// | `make_engine(bucket, prefix)` | the same, over an explicit pair — used by the validation and normalisation cases |
/// | `make_engine(options)` | the same over the default pair, with explicit `object_persistence_options` — used by the retention cases |
/// | `bucket()` / `prefix()` | the defaults `make_engine()` uses |
/// | `seed(key, body)` | write an object directly, bypassing the engine and the request log |
/// | `keys()` / `has(key)` / `body(key)` | inspect the store |
/// | `request_log()` / `count_requests(verb)` / `clear_requests()` | the operation log, `"PUT "` / `"GET "` / `"DELETE "` / `"LIST "` prefixed |
/// | `fail_next_puts(n)` / `fail_next_gets(n)` / `fail_next_deletes(n)` / `fail_next_lists(n)` | injected transient failures |
/// | `fail_puts_for_key(key)` | a permanent failure, which the engine's single retry cannot ride out |
///
/// ### Additionally, for a **fenced** harness
///
/// | Member | Meaning |
/// |---|---|
/// | `static constexpr bool fenced = true` | the engine is instantiated with `fencing_mode::compare_and_swap`. Absent means unfenced, so no existing harness needs touching |
/// | `owner_id()` | the owner id `make_engine()` claims the prefix with |
/// | `make_engine_as(owner_id, takeover_epoch)` | a *different* writer over the same `{bucket, prefix}` — the takeover and other-owner cases |
// clang-format on
///
/// A harness that cannot inject a failure (a real bucket, say) instantiates the
/// subset macro `KYTHIRA_OBJECT_STORE_CONFORMANCE_NO_INJECTION` instead. A
/// fenced harness additionally instantiates
/// `KYTHIRA_OBJECT_STORE_CONFORMANCE_FENCING`.
///
/// ## Running the whole suite twice, fenced and unfenced
///
/// A fenced harness re-runs **every** case above, not only the fencing ones, and
/// that is the point: `compare_and_swap` rewrites the precondition on most of
/// the engine's writes, so the evidence that it did not break anything is the
/// same 60-odd cases passing with it on. It is also the negative control the
/// fencing cases need — "the stale write was rejected" says nothing about a fence
/// unless the correct write is *accepted*, and a suite where every legitimate
/// write is accepted is that control at full width. Three cases genuinely differ
/// under fencing (the owner object exists; a conditional write is not retried)
/// and branch on `Harness::fenced` explicitly rather than being weakened for
/// both modes.

#include <boost/test/unit_test.hpp>

#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kythira::conformance {

/// Whether a harness's engine is fenced. Detected rather than required, so the
/// harnesses that predate fencing — and every unfenced one after it — need no
/// member at all.
template<typename Harness> constexpr auto harness_is_fenced() -> bool {
    if constexpr (requires { Harness::fenced; }) {
        return Harness::fenced;
    } else {
        return false;
    }
}

/// @brief The shared bodies. Every case is a static function so the
///        registration macro is a one-liner per case.
template<typename Harness> struct cases {
    using engine_t = typename Harness::engine_t;
    using log_entry_t = log_entry<>;
    using snapshot_t = snapshot<>;

    /// True when this instantiation runs against a `compare_and_swap` engine.
    static constexpr bool fenced = harness_is_fenced<Harness>();

    static auto make_entry(std::uint64_t term, std::uint64_t index,
                           std::vector<std::byte> cmd = {std::byte{0xAB}}) -> log_entry_t {
        log_entry_t e;
        e._term = term;
        e._index = index;
        e._command = std::move(cmd);
        return e;
    }

    /// `snapshot`'s `_is_joint_consensus` has no default member initializer, so
    /// every snapshot in this suite is built through here — a
    /// default-constructed one would read an indeterminate bool.
    static auto make_snapshot(std::uint64_t index, std::uint64_t term,
                              std::vector<std::uint64_t> nodes = {1, 2, 3},
                              std::vector<std::byte> state = {}, bool joint = false,
                              std::optional<std::vector<std::uint64_t>> old_nodes = std::nullopt)
        -> snapshot_t {
        snapshot_t snap;
        snap._last_included_index = index;
        snap._last_included_term = term;
        snap._configuration =
            cluster_configuration<>{std::move(nodes), joint, std::move(old_nodes), {}};
        snap._state_machine_state = std::move(state);
        return snap;
    }

    /// The key a padded index lands at, computed independently of the engine —
    /// a test that asked the engine for the key it wrote would pin nothing.
    static auto padded(const Harness& h, std::uint64_t index) -> std::string {
        const std::string digits = std::to_string(index);
        return h.prefix() + "/log/" + std::string(20 - digits.size(), '0') + digits;
    }

    /// The retained-snapshot key for an index, likewise computed here rather
    /// than asked of the engine.
    static auto retained_key(const Harness& h, std::uint64_t index) -> std::string {
        const std::string digits = std::to_string(index);
        return h.prefix() + "/snapshots/" + std::string(20 - digits.size(), '0') + digits;
    }

    /// Every key under `<prefix>/snapshots/`, in the store's (lexicographic,
    /// therefore index) order.
    static auto retained_keys(const Harness& h) -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& key : h.keys()) {
            if (key.starts_with(h.prefix() + "/snapshots/")) {
                out.push_back(key);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static auto options_with_retention(std::size_t n) -> object_persistence_options {
        object_persistence_options opts;
        opts.snapshot_retention = n;
        return opts;
    }

    // ── Concept ──────────────────────────────────────────────────────────────

    static auto engine_satisfies_the_persistence_engine_concept() -> void {
        static_assert(persistence_engine<engine_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                         log_entry_t, snapshot_t>,
                      "the engine under test must satisfy persistence_engine");
        BOOST_TEST(true);
    }

    // ── Construction ─────────────────────────────────────────────────────────

    static auto empty_bucket_or_prefix_is_rejected() -> void {
        Harness h;
        BOOST_CHECK_THROW(h.make_engine("", h.prefix()), std::invalid_argument);
        BOOST_CHECK_THROW(h.make_engine(h.bucket(), ""), std::invalid_argument);
        // A prefix of nothing but slashes normalises to empty, and is rejected
        // the same way rather than silently writing to the bucket root.
        BOOST_CHECK_THROW(h.make_engine(h.bucket(), "//"), std::invalid_argument);
    }

    /// A cold start reads the (empty) listing and writes nothing — except, when
    /// fenced, the one owner object that is the whole construction-time cost of
    /// `compare_and_swap`.
    static auto empty_prefix_yields_empty_state_and_writes_nothing() -> void {
        Harness h;
        engine_t eng = h.make_engine();

        BOOST_TEST(eng.load_current_term() == 0U);
        BOOST_TEST(!eng.load_voted_for().has_value());
        BOOST_TEST(eng.get_last_log_index() == 0U);
        BOOST_TEST(!eng.load_snapshot().has_value());
        BOOST_TEST(!eng.get_log_entry(1).has_value());

        if constexpr (fenced) {
            const std::vector<std::string> expected{h.prefix() + "/owner"};
            BOOST_TEST(h.keys() == expected, boost::test_tools::per_element());
            BOOST_TEST(h.count_requests("PUT") == 1U);
        } else {
            BOOST_TEST(h.keys().empty());
            BOOST_TEST(h.count_requests("PUT") == 0U);
        }
        BOOST_TEST(h.count_requests("LIST") == 1U);
    }

    /// A trailing slash names the same objects as no trailing slash — otherwise
    /// a config typo would silently open a second, empty key space.
    static auto trailing_slash_in_prefix_is_normalised() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine(h.bucket(), h.prefix() + "/");
            eng.save_current_term(11);
        }
        BOOST_TEST(h.has(h.prefix() + "/term"));
        engine_t eng2 = h.make_engine();
        BOOST_TEST(eng2.load_current_term() == 11U);
    }

    // ── Layout ───────────────────────────────────────────────────────────────

    static auto keys_have_the_documented_shape() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        BOOST_TEST(eng.term_key() == h.prefix() + "/term");
        BOOST_TEST(eng.voted_for_key() == h.prefix() + "/voted_for");
        BOOST_TEST(eng.snapshot_key() == h.prefix() + "/snapshot");
        BOOST_TEST(eng.log_prefix() == h.prefix() + "/log/");
        BOOST_TEST(eng.log_key(7) == padded(h, 7));
        BOOST_TEST(eng.log_key(0).size() == eng.log_prefix().size() + 20);
    }

    /// The digit-boundary case: without the padding `log/10` sorts before
    /// `log/9`, and recovery reads the log out of order without any error.
    static auto zero_padding_keeps_lexicographic_order_equal_to_numeric_order() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            for (std::uint64_t i : {1U, 2U, 9U, 10U, 11U, 100U}) {
                eng.append_log_entry(make_entry(1, i));
            }
        }
        // The store lists in lexicographic order; that order must be numeric.
        std::vector<std::string> log_keys;
        for (const auto& key : h.keys()) {
            if (key.starts_with(h.prefix() + "/log/")) {
                log_keys.push_back(key);
            }
        }
        BOOST_TEST(log_keys.size() == 6U);
        BOOST_TEST(std::is_sorted(log_keys.begin(), log_keys.end()));
        const std::vector<std::string> expected{padded(h, 1),  padded(h, 2),  padded(h, 9),
                                                padded(h, 10), padded(h, 11), padded(h, 100)};
        BOOST_TEST(log_keys == expected, boost::test_tools::per_element());

        // …and a reloaded engine sees them in index order.
        engine_t eng2 = h.make_engine();
        auto entries = eng2.get_log_entries(0, 1000);
        BOOST_TEST(entries.size() == 6U);
        BOOST_TEST(entries.front().index() == 1U);
        BOOST_TEST(entries.back().index() == 100U);
    }

    /// Property 8: a default-configured engine's bucket holds exactly the keys
    /// the shipped format defines — no extra "just one small" object.
    static auto a_default_engine_writes_exactly_the_documented_key_set() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.save_current_term(4);
        eng.save_voted_for(2);
        eng.append_log_entry(make_entry(4, 1));
        eng.append_log_entry(make_entry(4, 2));
        eng.save_snapshot(make_snapshot(1, 4));

        std::vector<std::string> expected{
            h.prefix() + "/log/" + std::string(19, '0') + "1",
            h.prefix() + "/log/" + std::string(19, '0') + "2",
            h.prefix() + "/snapshot",
            h.prefix() + "/term",
            h.prefix() + "/voted_for",
        };
        if constexpr (fenced) {
            // The fence adds exactly one object, and it is named — a fencing
            // implementation that also left a scratch or lock object behind would
            // fail here rather than in production.
            expected.push_back(h.prefix() + "/owner");
            std::sort(expected.begin(), expected.end());
        }
        auto actual = h.keys();
        std::sort(actual.begin(), actual.end());
        BOOST_TEST(actual == expected, boost::test_tools::per_element());
    }

    // ── currentTerm ──────────────────────────────────────────────────────────

    static auto term_round_trip() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        BOOST_TEST(eng.load_current_term() == 0U);
        eng.save_current_term(42);
        BOOST_TEST(eng.load_current_term() == 42U);
        BOOST_TEST(h.body(h.prefix() + "/term") == "42");
    }

    static auto term_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.save_current_term(42);
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST(eng2.load_current_term() == 42U);
    }

    /// Every read answers from the mirror: after construction, no read method
    /// touches the store at all.
    static auto reads_never_touch_the_network() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.save_current_term(3);
        eng.save_voted_for(1);
        eng.append_log_entry(make_entry(3, 1));
        eng.save_snapshot(make_snapshot(1, 3));
        h.clear_requests();

        BOOST_TEST(eng.load_current_term() == 3U);
        BOOST_TEST(eng.load_voted_for().value() == 1U);
        BOOST_TEST(eng.get_log_entry(1).has_value());
        BOOST_TEST(eng.get_log_entries(1, 1).size() == 1U);
        BOOST_TEST(eng.get_last_log_index() == 1U);
        BOOST_TEST(eng.load_snapshot().has_value());

        BOOST_TEST(h.request_log().empty());
    }

    // ── votedFor ─────────────────────────────────────────────────────────────

    static auto voted_for_round_trip() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        BOOST_TEST(!eng.load_voted_for().has_value());
        eng.save_voted_for(7);
        BOOST_TEST(eng.load_voted_for().value() == 7U);
        BOOST_TEST(h.body(h.prefix() + "/voted_for") == "7");
    }

    static auto voted_for_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.save_voted_for(7);
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST(eng2.load_voted_for().value() == 7U);
    }

    static auto the_none_sentinel_loads_as_no_vote() -> void {
        Harness h;
        h.seed(h.prefix() + "/voted_for", "none");
        engine_t eng = h.make_engine();
        BOOST_TEST(!eng.load_voted_for().has_value());
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    static auto append_is_exactly_one_put() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        h.clear_requests();
        eng.append_log_entry(make_entry(1, 1));
        BOOST_TEST(h.count_requests("PUT") == 1U);
        BOOST_TEST(h.count_requests("GET") == 0U);
    }

    static auto append_and_get() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.append_log_entry(make_entry(1, 1));
        eng.append_log_entry(make_entry(1, 2));
        BOOST_TEST(eng.get_last_log_index() == 2U);
        auto entry = eng.get_log_entry(2);
        BOOST_TEST(entry.has_value());
        BOOST_TEST(entry->term() == 1U);
        BOOST_TEST(entry->index() == 2U);
        BOOST_TEST(!eng.get_log_entry(3).has_value());
    }

    static auto log_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.append_log_entry(make_entry(1, 1));
            eng.append_log_entry(make_entry(2, 2));
            eng.append_log_entry(make_entry(2, 3));
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST(eng2.get_last_log_index() == 3U);
        BOOST_TEST(eng2.get_log_entry(2)->term() == 2U);
        BOOST_TEST(eng2.get_log_entries(1, 3).size() == 3U);
    }

    static auto binary_command_survives_reload() -> void {
        Harness h;
        const std::vector<std::byte> cmd{std::byte{0x00}, std::byte{0xFF}, std::byte{0x10},
                                         std::byte{0x7F}, std::byte{0x80}};
        {
            engine_t eng = h.make_engine();
            eng.append_log_entry(make_entry(1, 1, cmd));
        }
        engine_t eng2 = h.make_engine();
        // `std::byte` has no `operator<<`, so the byte-vector comparisons use
        // BOOST_CHECK rather than BOOST_TEST's printing comparison.
        BOOST_CHECK(eng2.get_log_entry(1)->command() == cmd);
    }

    static auto embedded_nulls_and_high_bytes_survive() -> void {
        Harness h;
        std::vector<std::byte> cmd;
        for (int i = 0; i < 256; ++i) {
            cmd.push_back(static_cast<std::byte>(i));
        }
        {
            engine_t eng = h.make_engine();
            eng.append_log_entry(make_entry(1, 1, cmd));
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST(eng2.get_log_entry(1)->command().size() == 256U);
        // `std::byte` has no `operator<<`, so the byte-vector comparisons use
        // BOOST_CHECK rather than BOOST_TEST's printing comparison.
        BOOST_CHECK(eng2.get_log_entry(1)->command() == cmd);
    }

    static auto entry_type_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            auto entry = make_entry(1, 1);
            entry._type = entry_type::configuration;
            eng.append_log_entry(entry);
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST((eng2.get_log_entry(1)->type() == entry_type::configuration));
    }

    static auto log_entries_range() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        for (std::uint64_t i = 1; i <= 5; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        BOOST_TEST(eng.get_log_entries(2, 4).size() == 3U);
        BOOST_TEST(eng.get_log_entries(2, 4).front().index() == 2U);
        BOOST_TEST(eng.get_log_entries(2, 4).back().index() == 4U);
        BOOST_TEST(eng.get_log_entries(6, 9).empty());
    }

    /// Truncation is a bounded batch of DELETEs — one per affected object, and
    /// no others. A whole-log rewrite would pass a state assertion and fail
    /// this one.
    static auto truncate_deletes_exactly_the_affected_objects() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        for (std::uint64_t i = 1; i <= 4; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        h.clear_requests();
        eng.truncate_log(3);

        BOOST_TEST(h.count_requests("DELETE") == 2U);
        BOOST_TEST(h.count_requests("PUT") == 0U);
        BOOST_TEST(h.has(padded(h, 1)));
        BOOST_TEST(h.has(padded(h, 2)));
        BOOST_TEST(!h.has(padded(h, 3)));
        BOOST_TEST(!h.has(padded(h, 4)));
        BOOST_TEST(eng.get_last_log_index() == 2U);
    }

    static auto truncate_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            for (std::uint64_t i = 1; i <= 4; ++i) {
                eng.append_log_entry(make_entry(1, i));
            }
            eng.truncate_log(3);
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST(eng2.get_last_log_index() == 2U);
        BOOST_TEST(!eng2.get_log_entry(3).has_value());
    }

    static auto delete_log_entries_before_removes_only_older_entries() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        for (std::uint64_t i = 1; i <= 4; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        h.clear_requests();
        eng.delete_log_entries_before(3);

        BOOST_TEST(h.count_requests("DELETE") == 2U);
        BOOST_TEST(!h.has(padded(h, 1)));
        BOOST_TEST(!h.has(padded(h, 2)));
        BOOST_TEST(h.has(padded(h, 3)));
        BOOST_TEST(eng.get_last_log_index() == 4U);
        BOOST_TEST(!eng.get_log_entry(1).has_value());
    }

    static auto delete_before_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            for (std::uint64_t i = 1; i <= 4; ++i) {
                eng.append_log_entry(make_entry(1, i));
            }
            eng.delete_log_entries_before(3);
        }
        engine_t eng2 = h.make_engine();
        BOOST_TEST(!eng2.get_log_entry(2).has_value());
        BOOST_TEST(eng2.get_log_entry(3).has_value());
        BOOST_TEST(eng2.get_last_log_index() == 4U);
    }

    /// Property 9: the engine deletes only what it can prove it wrote. An
    /// operator's note under the prefix survives a full truncate/compact cycle.
    static auto a_foreign_object_survives_truncation_and_compaction() -> void {
        Harness h;
        h.seed(h.prefix() + "/NOTES", "left here by an operator");
        engine_t eng = h.make_engine();
        for (std::uint64_t i = 1; i <= 4; ++i) {
            eng.append_log_entry(make_entry(1, i));
        }
        eng.truncate_log(4);
        eng.delete_log_entries_before(3);
        BOOST_TEST(h.has(h.prefix() + "/NOTES"));
        BOOST_TEST(h.body(h.prefix() + "/NOTES") == "left here by an operator");
    }

    // ── Snapshot ─────────────────────────────────────────────────────────────

    static auto snapshot_round_trip() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        BOOST_TEST(!eng.load_snapshot().has_value());
        eng.save_snapshot(make_snapshot(10, 3, {1, 2, 3}, {std::byte{0x01}, std::byte{0x02}}));

        auto loaded = eng.load_snapshot();
        BOOST_TEST(loaded.has_value());
        BOOST_TEST(loaded->last_included_index() == 10U);
        BOOST_TEST(loaded->last_included_term() == 3U);
        BOOST_TEST(loaded->configuration().nodes().size() == 3U);
    }

    static auto snapshot_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.save_snapshot(make_snapshot(10, 3, {1, 2, 3}, {std::byte{0x01}, std::byte{0xFE}}));
        }
        engine_t eng2 = h.make_engine();
        auto loaded = eng2.load_snapshot();
        BOOST_TEST(loaded.has_value());
        BOOST_TEST(loaded->last_included_index() == 10U);
        BOOST_TEST(loaded->state_machine_state().size() == 2U);
        BOOST_TEST(std::to_integer<int>(loaded->state_machine_state()[1]) == 0xFE);
    }

    /// The single slot is overwritten, not accumulated: with default options no
    /// second snapshot object appears anywhere.
    static auto snapshot_overwrites_the_single_slot() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.save_snapshot(make_snapshot(5, 1));
        eng.save_snapshot(make_snapshot(9, 2));

        std::size_t snapshot_objects = 0;
        for (const auto& key : h.keys()) {
            if (key.starts_with(h.prefix() + "/snapshot")) {
                ++snapshot_objects;
            }
        }
        BOOST_TEST(snapshot_objects == 1U);
        BOOST_TEST(eng.load_snapshot()->last_included_index() == 9U);
    }

    static auto joint_consensus_configuration_survives_reload() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.save_snapshot(make_snapshot(4, 2, {1, 2, 3}, {}, /*joint=*/true,
                                            std::vector<std::uint64_t>{1, 2}));
        }
        engine_t eng2 = h.make_engine();
        auto loaded = eng2.load_snapshot();
        BOOST_TEST(loaded.has_value());
        BOOST_TEST(loaded->configuration().is_joint_consensus());
        BOOST_TEST(loaded->configuration().old_nodes().has_value());
        BOOST_TEST(loaded->configuration().old_nodes()->size() == 2U);
    }

    // ── Snapshot retention (Requirement 8) ───────────────────────────────────

    /// Requirement 8.3, and the sharpest form of Property 8: at the default
    /// retention the bucket is what shipped, **and so is the request pattern**.
    /// A retention feature that quietly added a LIST or a DELETE per snapshot
    /// would satisfy a key-set assertion and still have changed every existing
    /// deployment's cost.
    static auto retention_of_one_writes_no_retained_copy_and_costs_one_put() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.save_snapshot(make_snapshot(5, 1));
        h.clear_requests();
        eng.save_snapshot(make_snapshot(9, 2));
        eng.save_snapshot(make_snapshot(12, 2));

        BOOST_TEST(retained_keys(h).empty());
        BOOST_TEST(h.count_requests("PUT") == 2U);
        BOOST_TEST(h.count_requests("DELETE") == 0U);
        BOOST_TEST(h.count_requests("LIST") == 0U);
        BOOST_TEST(eng.load_snapshot()->last_included_index() == 12U);
    }

    /// `0` cannot mean "keep none" — that would delete the copy of the snapshot
    /// just written — so it is rejected where every other bad option is.
    static auto a_retention_of_zero_is_rejected() -> void {
        Harness h;
        BOOST_CHECK_THROW(h.make_engine(options_with_retention(0)), std::invalid_argument);
    }

    /// Retention counts generations including the live slot, so at 2 the store
    /// holds the current snapshot and exactly one predecessor.
    static auto retention_of_two_keeps_the_current_and_one_predecessor() -> void {
        Harness h;
        engine_t eng = h.make_engine(options_with_retention(2));
        eng.save_snapshot(make_snapshot(5, 1));
        eng.save_snapshot(make_snapshot(9, 2));
        eng.save_snapshot(make_snapshot(12, 3));

        const std::vector<std::string> expected{retained_key(h, 9), retained_key(h, 12)};
        BOOST_TEST(retained_keys(h) == expected, boost::test_tools::per_element());
        BOOST_TEST(eng.load_snapshot()->last_included_index() == 12U);
        BOOST_TEST(!eng.last_prune_error().has_value());
    }

    /// …and at N it prunes oldest first, one DELETE per save in steady state.
    static auto retention_of_n_prunes_oldest_first() -> void {
        Harness h;
        engine_t eng = h.make_engine(options_with_retention(3));
        for (std::uint64_t i = 1; i <= 6; ++i) {
            eng.save_snapshot(make_snapshot(i, 1));
        }
        const std::vector<std::string> expected{retained_key(h, 4), retained_key(h, 5),
                                                retained_key(h, 6)};
        BOOST_TEST(retained_keys(h) == expected, boost::test_tools::per_element());

        // Steady state: the sixth save costs two PUTs and exactly one DELETE.
        h.clear_requests();
        eng.save_snapshot(make_snapshot(7, 1));
        BOOST_TEST(h.count_requests("PUT") == 2U);
        BOOST_TEST(h.count_requests("DELETE") == 1U);
        BOOST_TEST(h.count_requests("LIST") == 0U);
    }

    /// The retained copy is the same bytes as the live slot, by the same codec —
    /// which is what makes it restorable by copying it over `<prefix>/snapshot`.
    static auto a_retained_copy_is_byte_identical_to_the_current_snapshot() -> void {
        Harness h;
        engine_t eng = h.make_engine(options_with_retention(2));
        eng.save_snapshot(make_snapshot(8, 3, {1, 2, 3}, {std::byte{0x01}, std::byte{0xFE}}));
        BOOST_TEST(h.body(retained_key(h, 8)) == h.body(h.prefix() + "/snapshot"));
    }

    /// Construction notes the retained copies from the listing it already
    /// performs, so a restarted engine prunes the previous run's copies rather
    /// than accumulating a second generation of them.
    static auto retained_copies_are_pruned_across_a_restart() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine(options_with_retention(2));
            eng.save_snapshot(make_snapshot(1, 1));
            eng.save_snapshot(make_snapshot(2, 1));
        }
        engine_t eng2 = h.make_engine(options_with_retention(2));
        h.clear_requests();
        eng2.save_snapshot(make_snapshot(3, 1));

        const std::vector<std::string> expected{retained_key(h, 2), retained_key(h, 3)};
        BOOST_TEST(retained_keys(h) == expected, boost::test_tools::per_element());
        // The prune needed no listing of its own.
        BOOST_TEST(h.count_requests("LIST") == 0U);
    }

    /// Requirement 8.4: the load path reads `<prefix>/snapshot` and nothing
    /// else, so a retained copy that is unreadable cannot keep a node from
    /// starting — the property that makes retention safe to turn on.
    static auto a_corrupt_retained_copy_does_not_break_startup() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine(options_with_retention(3));
            eng.save_snapshot(make_snapshot(4, 2));
            eng.save_snapshot(make_snapshot(6, 2));
        }
        h.seed(retained_key(h, 4), "{\"last_included_index\": \"ten\"");

        engine_t eng2 = h.make_engine(options_with_retention(3));
        BOOST_TEST(eng2.load_snapshot()->last_included_index() == 6U);
        // …and it was never even fetched.
        BOOST_TEST(
            std::none_of(h.request_log().begin(), h.request_log().end(),
                         [&h](const std::string& r) { return r == "GET " + retained_key(h, 4); }));
    }

    /// Requirement 8.5 / Property 9: pruning deletes only keys whose index
    /// suffix the engine recognizes. An operator's object under
    /// `<prefix>/snapshots/` survives, and — unlike the same shape under
    /// `<prefix>/log/` — it is not corruption either, because nothing reads it.
    static auto pruning_leaves_alone_a_key_it_cannot_prove_it_wrote() -> void {
        Harness h;
        h.seed(h.prefix() + "/snapshots/NOTES", "left here by an operator");
        h.seed(h.prefix() + "/snapshots/7", "an unpadded index");

        engine_t eng = h.make_engine(options_with_retention(2));
        for (std::uint64_t i = 1; i <= 5; ++i) {
            eng.save_snapshot(make_snapshot(i, 1));
        }

        BOOST_TEST(h.has(h.prefix() + "/snapshots/NOTES"));
        BOOST_TEST(h.body(h.prefix() + "/snapshots/NOTES") == "left here by an operator");
        BOOST_TEST(h.has(h.prefix() + "/snapshots/7"));
        // …while the copies it did write were pruned to the limit.
        const std::vector<std::string> expected{retained_key(h, 4), retained_key(h, 5)};
        std::vector<std::string> padded_only;
        for (const auto& key : retained_keys(h)) {
            if (key.size() == (h.prefix() + "/snapshots/").size() + 20) {
                padded_only.push_back(key);
            }
        }
        BOOST_TEST(padded_only == expected, boost::test_tools::per_element());
    }

    // ── Retention under injected failure (Requirement 8.2) ───────────────────

    /// Step (a). The retained copy is written first precisely so that its
    /// failure changes nothing: the live slot still holds the predecessor.
    static auto a_failure_writing_the_retained_copy_leaves_the_previous_snapshot() -> void {
        Harness h;
        engine_t eng = h.make_engine(options_with_retention(3));
        eng.save_snapshot(make_snapshot(5, 1));

        h.fail_puts_for_key(retained_key(h, 9));
        BOOST_CHECK_THROW(eng.save_snapshot(make_snapshot(9, 2)), std::runtime_error);
        h.fail_puts_for_key("");

        BOOST_TEST(eng.load_snapshot()->last_included_index() == 5U);
        BOOST_TEST(!h.has(retained_key(h, 9)));
        engine_t eng2 = h.make_engine(options_with_retention(3));
        BOOST_TEST(eng2.load_snapshot()->last_included_index() == 5U);
    }

    /// Step (b), the commit point, at retention 1, 2 and N: whichever snapshot
    /// was current before the failed save is still current and still readable
    /// after it, and a retained copy left behind by step (a) is inert.
    static auto a_failure_at_the_commit_point_leaves_the_previous_snapshot() -> void {
        for (const std::size_t retention : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            Harness h;
            engine_t eng = h.make_engine(options_with_retention(retention));
            eng.save_snapshot(make_snapshot(5, 1));

            h.fail_puts_for_key(h.prefix() + "/snapshot");
            BOOST_CHECK_THROW(eng.save_snapshot(make_snapshot(9, 2)), std::runtime_error);
            h.fail_puts_for_key("");

            BOOST_TEST(eng.load_snapshot()->last_included_index() == 5U);
            engine_t eng2 = h.make_engine(options_with_retention(retention));
            BOOST_TEST(eng2.load_snapshot()->last_included_index() == 5U);
            // The unreferenced retained copy of the snapshot that never
            // committed is inert, not an error.
            BOOST_TEST(h.has(retained_key(h, 9)) == (retention > 1));
        }
    }

    /// Step (c). Pruning runs after the commit point, so a failed DELETE must
    /// not be reported as a failed `save_snapshot` — it is recorded instead, and
    /// the next save retries it.
    static auto a_failure_pruning_leaves_the_new_snapshot_committed() -> void {
        Harness h;
        engine_t eng = h.make_engine(options_with_retention(2));
        eng.save_snapshot(make_snapshot(1, 1));
        eng.save_snapshot(make_snapshot(2, 1));

        h.fail_next_deletes(1);
        BOOST_CHECK_NO_THROW(eng.save_snapshot(make_snapshot(3, 1)));

        BOOST_TEST(eng.load_snapshot()->last_included_index() == 3U);
        BOOST_TEST(h.body(h.prefix() + "/snapshot") == h.body(retained_key(h, 3)));
        BOOST_TEST(eng.last_prune_error().has_value());
        BOOST_TEST(eng.last_prune_error()->find(retained_key(h, 1)) != std::string::npos);
        BOOST_TEST(h.has(retained_key(h, 1)));

        // Self-healing: the next save prunes what the failed one could not, and
        // clears the recorded failure.
        BOOST_CHECK_NO_THROW(eng.save_snapshot(make_snapshot(4, 1)));
        const std::vector<std::string> expected{retained_key(h, 3), retained_key(h, 4)};
        BOOST_TEST(retained_keys(h) == expected, boost::test_tools::per_element());
        BOOST_TEST(!eng.last_prune_error().has_value());
    }

    // ── Durability (Property 1) ──────────────────────────────────────────────

    /// The write is synchronous, not buffered: by the time the method returns,
    /// the store has already been asked and already holds the bytes.
    static auto a_returned_write_has_already_reached_the_store() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        h.clear_requests();
        eng.save_current_term(9);

        auto log = h.request_log();
        BOOST_TEST(!log.empty());
        BOOST_TEST(log.back() == "PUT " + h.prefix() + "/term");
        BOOST_TEST(h.body(h.prefix() + "/term") == "9");
    }

    /// The single-line mistake this pins: treating a non-2xx as success. The
    /// next read comes from the mirror, so nothing else would notice.
    static auto a_failed_put_throws_and_leaves_the_mirror_unchanged() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.save_current_term(2);
        eng.append_log_entry(make_entry(2, 1));

        h.fail_puts_for_key(h.prefix() + "/term");
        BOOST_CHECK_THROW(eng.save_current_term(3), std::runtime_error);
        h.fail_puts_for_key("");

        BOOST_TEST(eng.load_current_term() == 2U);
        BOOST_TEST(h.body(h.prefix() + "/term") == "2");

        h.fail_puts_for_key(padded(h, 2));
        BOOST_CHECK_THROW(eng.append_log_entry(make_entry(2, 2)), std::runtime_error);
        h.fail_puts_for_key("");

        BOOST_TEST(eng.get_last_log_index() == 1U);
        BOOST_TEST(!h.has(padded(h, 2)));
    }

    /// The engine's own retry: one, PUT-only, and it rides out a transient
    /// failure without the caller seeing it.
    ///
    /// Under `compare_and_swap` the term write is conditional and therefore
    /// deliberately *not* retried, so the retry is exercised on `save_snapshot`,
    /// which stays unconditional in both modes. The claim under test is the same
    /// one either way; what changes is which write still carries it.
    static auto a_transient_put_failure_is_retried_once_and_succeeds() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        h.clear_requests();
        h.fail_next_puts(1);

        if constexpr (fenced) {
            BOOST_CHECK_NO_THROW(eng.save_snapshot(make_snapshot(6, 2)));
            BOOST_TEST(eng.load_snapshot()->last_included_index() == 6U);
            BOOST_TEST(!h.body(h.prefix() + "/snapshot").empty());
        } else {
            BOOST_CHECK_NO_THROW(eng.save_current_term(5));
            BOOST_TEST(eng.load_current_term() == 5U);
            BOOST_TEST(h.body(h.prefix() + "/term") == "5");
        }
        BOOST_TEST(h.count_requests("PUT") == 2U);
    }

    /// A DELETE is not retried; the caller re-runs the whole idempotent
    /// truncation instead. The mirror still equals the store afterwards.
    static auto a_failed_delete_throws_and_is_not_retried() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.append_log_entry(make_entry(1, 1));
        eng.append_log_entry(make_entry(1, 2));
        h.clear_requests();

        h.fail_next_deletes(1);
        BOOST_CHECK_THROW(eng.truncate_log(1), std::runtime_error);
        BOOST_TEST(h.count_requests("DELETE") == 1U);
        BOOST_TEST(eng.get_last_log_index() == 2U);

        // Re-running the truncation is the recovery, and it converges.
        BOOST_CHECK_NO_THROW(eng.truncate_log(1));
        BOOST_TEST(eng.get_last_log_index() == 0U);
        BOOST_TEST(!h.has(padded(h, 1)));
        BOOST_TEST(!h.has(padded(h, 2)));
    }

    /// A store failure during the load path fails construction rather than
    /// producing an engine with invented state.
    static auto a_failed_load_fails_construction() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.save_current_term(5);
        }
        h.fail_next_lists(1);
        BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);

        h.fail_next_gets(1);
        BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);

        // …and it builds again once the injection is spent, proving the
        // failure came from the injection and not from the fixture.
        BOOST_CHECK_NO_THROW(h.make_engine());
    }

    // ── Corruption is fatal at load ──────────────────────────────────────────

    static auto a_corrupt_term_object_fails_construction_by_name() -> void {
        Harness h;
        h.seed(h.prefix() + "/term", "not-a-number");
        try {
            engine_t eng = h.make_engine();
            BOOST_FAIL("construction must not survive a corrupt term object");
        } catch (const std::runtime_error& e) {
            BOOST_TEST(std::string(e.what()).find(h.prefix() + "/term") != std::string::npos);
        }
    }

    /// `std::stoull` would read 12 out of this and start with a plausible term.
    static auto a_partially_numeric_term_is_corruption_not_a_prefix() -> void {
        Harness h;
        h.seed(h.prefix() + "/term", "12garbage");
        BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);
    }

    static auto a_corrupt_voted_for_object_fails_construction_by_name() -> void {
        Harness h;
        h.seed(h.prefix() + "/voted_for", "node-seven");
        try {
            engine_t eng = h.make_engine();
            BOOST_FAIL("construction must not survive a corrupt voted_for object");
        } catch (const std::runtime_error& e) {
            BOOST_TEST(std::string(e.what()).find(h.prefix() + "/voted_for") != std::string::npos);
        }
    }

    static auto a_truncated_log_record_fails_construction_by_name() -> void {
        Harness h;
        {
            engine_t eng = h.make_engine();
            eng.append_log_entry(make_entry(1, 1));
            eng.append_log_entry(make_entry(1, 2));
        }
        // A half-uploaded object: valid prefix, no closing brace.
        h.seed(padded(h, 2), "{\"term\":1,\"index\":2,\"comm");
        try {
            engine_t eng2 = h.make_engine();
            BOOST_FAIL("construction must not survive a truncated log record");
        } catch (const std::runtime_error& e) {
            BOOST_TEST(std::string(e.what()).find(padded(h, 2)) != std::string::npos);
        }
    }

    /// A record whose index disagrees with the key it is stored under means the
    /// log's ordering guarantee no longer holds.
    static auto a_log_record_disagreeing_with_its_key_fails_construction() -> void {
        Harness h;
        h.seed(padded(h, 4), "{\"term\":1,\"index\":9,\"command\":\"\",\"type\":0}");
        try {
            engine_t eng = h.make_engine();
            BOOST_FAIL("construction must not survive a mislabelled log record");
        } catch (const std::runtime_error& e) {
            BOOST_TEST(std::string(e.what()).find(padded(h, 4)) != std::string::npos);
        }
    }

    /// An unpadded key cannot be ordered against the padded ones, so it is
    /// corruption rather than a key to skip.
    static auto an_unpadded_log_key_fails_construction() -> void {
        Harness h;
        h.seed(h.prefix() + "/log/7", "{\"term\":1,\"index\":7,\"command\":\"\",\"type\":0}");
        BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);
    }

    static auto a_corrupt_snapshot_fails_construction_by_name() -> void {
        Harness h;
        h.seed(h.prefix() + "/snapshot", "{\"last_included_index\": \"ten\"}");
        try {
            engine_t eng = h.make_engine();
            BOOST_FAIL("construction must not survive a corrupt snapshot");
        } catch (const std::runtime_error& e) {
            BOOST_TEST(std::string(e.what()).find(h.prefix() + "/snapshot") != std::string::npos);
        }
    }

    /// A command that is not valid base64 would decode to *some* byte string
    /// under the file engine's skip-invalid-characters codec; here it is
    /// corruption.
    static auto a_non_base64_command_fails_construction() -> void {
        Harness h;
        h.seed(padded(h, 1), "{\"term\":1,\"index\":1,\"command\":\"!!not base64!!\",\"type\":0}");
        BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);
    }

    /// Objects that are not part of this format are none of the engine's
    /// business — neither parsed nor rejected.
    static auto a_foreign_object_under_the_prefix_is_ignored() -> void {
        Harness h;
        h.seed(h.prefix() + "/README", "left here by an operator");
        engine_t eng = h.make_engine();
        BOOST_TEST(eng.load_current_term() == 0U);
        BOOST_TEST(h.has(h.prefix() + "/README"));
    }

    // ── Fencing (Requirement 9) ──────────────────────────────────────────────
    //
    // Instantiated only for a fenced harness, by
    // KYTHIRA_OBJECT_STORE_CONFORMANCE_FENCING. Every case here carries its own
    // negative control: a fence that rejects everything is indistinguishable
    // from a broken client, so each of these shows a *correct* write being
    // accepted alongside the stale one being refused.

    static auto owner_key(const Harness& h) -> std::string { return h.prefix() + "/owner"; }

    /// Construction claims the prefix, and the claim is one create-only PUT of an
    /// object that names the owner.
    static auto construction_claims_the_prefix_with_an_owner_object() -> void {
        Harness h;
        engine_t eng = h.make_engine();

        BOOST_TEST(h.has(owner_key(h)));
        BOOST_TEST(eng.owner_epoch() == 1U);
        BOOST_TEST(!eng.is_fenced());

        const std::string body = h.body(owner_key(h));
        BOOST_TEST(body.find(h.owner_id()) != std::string::npos);
        BOOST_TEST(body.find("epoch") != std::string::npos);
        BOOST_TEST(body.find("started_at") != std::string::npos);

        // The negative control, and the reason every "rejected" below means
        // anything: the owner's own writes are accepted.
        BOOST_CHECK_NO_THROW(eng.save_current_term(1));
        BOOST_CHECK_NO_THROW(eng.save_voted_for(2));
        BOOST_CHECK_NO_THROW(eng.append_log_entry(make_entry(1, 1)));
        BOOST_TEST(eng.load_current_term() == 1U);
    }

    /// A restart by the recorded owner is allowed — a crash-restart and a
    /// duplicated deployment are indistinguishable at construction, and a restart
    /// must succeed — and it read-increments the epoch, leaving an audit trail.
    static auto a_restart_by_the_same_owner_advances_the_epoch() -> void {
        Harness h;
        {
            engine_t first = h.make_engine();
            BOOST_TEST(first.owner_epoch() == 1U);
            first.save_current_term(1);
        }
        engine_t second = h.make_engine();
        BOOST_TEST(second.owner_epoch() == 2U);
        BOOST_TEST(second.load_current_term() == 1U);
        // …and the restarted engine can write, which means it picked up the
        // version of the term object it inherited rather than assuming absence.
        BOOST_CHECK_NO_THROW(second.save_current_term(2));
        BOOST_TEST(h.body(h.prefix() + "/term") == "2");
    }

    /// Requirement 9.6: a different owner cannot open the prefix by accident.
    static auto construction_over_another_owners_prefix_throws_naming_the_owner() -> void {
        Harness h;
        {
            engine_t owner = h.make_engine();
            owner.save_current_term(3);
        }
        try {
            engine_t intruder = h.make_engine_as("some-other-node", std::nullopt);
            BOOST_FAIL("construction must not succeed over a prefix another owner holds");
        } catch (const persistence_fenced_error& e) {
            BOOST_TEST(e.key() == owner_key(h));
            const std::string what = e.what();
            BOOST_TEST(what.find(h.owner_id()) != std::string::npos);
            BOOST_TEST(what.find("takeover_epoch") != std::string::npos);
        }
        // A refused construction wrote nothing: the prefix still names its owner.
        BOOST_TEST(h.body(owner_key(h)).find(h.owner_id()) != std::string::npos);
        BOOST_TEST(h.body(h.prefix() + "/term") == "3");
    }

    /// …and with an epoch above the recorded one it succeeds, which is what makes
    /// a takeover an auditable act rather than a race.
    static auto a_takeover_epoch_above_the_recorded_epoch_takes_the_prefix() -> void {
        Harness h;
        {
            engine_t owner = h.make_engine();
            owner.save_current_term(3);
        }
        engine_t taker = h.make_engine_as("some-other-node", 5);
        BOOST_TEST(taker.owner_epoch() == 5U);
        BOOST_TEST(h.body(owner_key(h)).find("some-other-node") != std::string::npos);
        BOOST_TEST(taker.load_current_term() == 3U);
        BOOST_CHECK_NO_THROW(taker.save_current_term(4));
    }

    /// An epoch that does not advance is not a takeover, and is refused rather
    /// than ignored — an ignored takeover_epoch would put two writers on one
    /// prefix while both believed they had been granted it.
    static auto a_takeover_epoch_that_does_not_advance_is_rejected() -> void {
        Harness h;
        {
            engine_t owner = h.make_engine();  // epoch 1
        }
        BOOST_CHECK_THROW(h.make_engine_as("some-other-node", 1), std::invalid_argument);
        BOOST_CHECK_THROW(h.make_engine_as("some-other-node", 0), std::invalid_argument);
        BOOST_TEST(h.body(owner_key(h)).find(h.owner_id()) != std::string::npos);
        // …and the same owner is not exempt: the epoch is the record, not the name.
        BOOST_CHECK_THROW(h.make_engine_as(h.owner_id(), 1), std::invalid_argument);
    }

    /// A corrupt owner object must fail construction rather than read as
    /// "unowned", which would hand the prefix to a second writer at exactly the
    /// moment fencing was asked for.
    static auto a_corrupt_owner_object_fails_construction_by_name() -> void {
        Harness h;
        h.seed(owner_key(h), "{\"owner_id\":\"a\",\"epoch\":");
        try {
            engine_t eng = h.make_engine();
            BOOST_FAIL("construction must not survive a corrupt owner object");
        } catch (const std::runtime_error& e) {
            BOOST_TEST(std::string(e.what()).find(owner_key(h)) != std::string::npos);
        }
        // An owner object naming nobody is corruption too, not an empty claim.
        h.seed(owner_key(h), "{\"owner_id\":\"\",\"epoch\":1}");
        BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);
    }

    /// Requirement 9.9, the headline case: the genuine two-engine race, and the
    /// latch that follows it.
    static auto the_two_engine_race_latches_the_loser() -> void {
        Harness h;
        engine_t a = h.make_engine();
        a.save_current_term(1);
        a.save_voted_for(2);

        // A duplicated deployment: same owner id, same prefix, both processes
        // live. Construction cannot refuse this, which is the whole reason the
        // fence lives on the writes.
        engine_t b = h.make_engine();
        BOOST_CHECK_NO_THROW(b.save_current_term(2));

        h.clear_requests();
        BOOST_CHECK_THROW(a.save_current_term(3), persistence_fenced_error);
        BOOST_TEST(a.is_fenced());
        // One attempt: Requirement 9.4 — retrying a precondition failure is
        // exactly the overwrite the fence exists to prevent.
        BOOST_TEST(h.count_requests("PUT") == 1U);
        BOOST_TEST(h.body(h.prefix() + "/term") == "2");

        // Latched: every mutating call throws, and none of them reaches the store.
        h.clear_requests();
        BOOST_CHECK_THROW(a.save_current_term(4), persistence_fenced_error);
        BOOST_CHECK_THROW(a.save_voted_for(9), persistence_fenced_error);
        BOOST_CHECK_THROW(a.append_log_entry(make_entry(3, 1)), persistence_fenced_error);
        BOOST_CHECK_THROW(a.truncate_log(1), persistence_fenced_error);
        BOOST_CHECK_THROW(a.delete_log_entries_before(1), persistence_fenced_error);
        BOOST_CHECK_THROW(a.save_snapshot(make_snapshot(1, 1)), persistence_fenced_error);
        BOOST_TEST(h.request_log().empty());

        // Reads still answer from the mirror — they report what this node last
        // knew, and the raft layer meets the latch at its next write.
        BOOST_TEST(a.load_current_term() == 1U);
        BOOST_TEST(a.load_voted_for().value() == 2U);

        // The negative control: the rightful writer is unaffected throughout.
        BOOST_TEST(!b.is_fenced());
        BOOST_CHECK_NO_THROW(b.save_current_term(5));
        BOOST_TEST(h.body(h.prefix() + "/term") == "5");
    }

    /// The case the chokepoint argument does **not** cover, which is why it is
    /// tested separately rather than folded into the race above: a stale leader
    /// appends without ever writing term or vote. Create-only log PUTs are the
    /// only thing that catches it.
    static auto the_stale_appender_race_latches_one_writer() -> void {
        Harness h;
        engine_t a = h.make_engine();
        engine_t b = h.make_engine();

        BOOST_CHECK_NO_THROW(a.append_log_entry(make_entry(1, 1)));

        h.clear_requests();
        // Neither engine has touched term or voted_for at this point.
        BOOST_CHECK_THROW(b.append_log_entry(make_entry(1, 1)), persistence_fenced_error);
        BOOST_TEST(b.is_fenced());
        BOOST_TEST(h.count_requests("PUT") == 1U);
        BOOST_TEST(b.get_last_log_index() == 0U);

        // A is untouched, and — the control that matters for create-only —
        // legitimate re-use of an index is preceded by `truncate_log`'s DELETE, so
        // create-only never refuses a legal write.
        BOOST_TEST(!a.is_fenced());
        BOOST_CHECK_NO_THROW(a.append_log_entry(make_entry(1, 2)));
        BOOST_CHECK_NO_THROW(a.truncate_log(2));
        BOOST_CHECK_NO_THROW(a.append_log_entry(make_entry(2, 2)));
        BOOST_TEST(a.get_log_entry(2)->term() == 2U);
    }

    /// The **stated residual**, pinned so it cannot be believed away: snapshot
    /// writes and DELETEs stay unconditional, so a second writer whose only
    /// interaction with the prefix is a snapshot overwrite or a truncation is not
    /// detected. The chokepoint still catches it at its next term or vote.
    static auto a_second_writers_snapshot_and_truncation_are_not_detected() -> void {
        Harness h;
        engine_t a = h.make_engine();
        engine_t b = h.make_engine();

        BOOST_CHECK_NO_THROW(b.save_snapshot(make_snapshot(5, 1)));
        // Undetected, on purpose.
        BOOST_CHECK_NO_THROW(a.save_snapshot(make_snapshot(3, 1)));
        BOOST_TEST(!a.is_fenced());
        BOOST_TEST(a.load_snapshot()->last_included_index() == 3U);

        // So is a truncation by the losing writer.
        BOOST_CHECK_NO_THROW(a.append_log_entry(make_entry(1, 1)));
        BOOST_CHECK_NO_THROW(a.truncate_log(1));
        BOOST_TEST(!a.is_fenced());

        // …and then the chokepoint fires: A cannot write a term or a vote after
        // B has, which is the property the safety argument actually rests on.
        BOOST_CHECK_NO_THROW(b.save_current_term(7));
        BOOST_CHECK_THROW(a.save_current_term(8), persistence_fenced_error);
        BOOST_TEST(a.is_fenced());
    }

    /// Task 0.3's finding, at the engine level: only a precondition the service
    /// evaluated latches. An ordinary failure — a broken connection, or the
    /// benign conditional-request race S3 spells `409 ConditionalRequestConflict`
    /// — must not, because the latch is permanent and unclearable.
    static auto a_transient_failure_on_a_conditional_write_does_not_latch() -> void {
        Harness h;
        engine_t eng = h.make_engine();
        eng.save_current_term(1);
        h.clear_requests();

        h.fail_puts_for_key(h.prefix() + "/term");
        try {
            eng.save_current_term(2);
            BOOST_FAIL("a failed conditional write must throw");
        } catch (const persistence_fenced_error&) {
            BOOST_FAIL(
                "a transient failure must not be reported as a fence — mapping a bare "
                "status here would latch a healthy engine permanently");
        } catch (const std::runtime_error&) {
            BOOST_TEST(true);
        }
        h.fail_puts_for_key("");

        BOOST_TEST(!eng.is_fenced());
        BOOST_TEST(eng.load_current_term() == 1U);
        // Not retried either: the retry's precondition would be stale by
        // construction if the first attempt had landed.
        BOOST_TEST(h.count_requests("PUT") == 1U);

        // And the engine is still usable, which is the whole difference between
        // this and a fence.
        BOOST_CHECK_NO_THROW(eng.save_current_term(2));
        BOOST_TEST(h.body(h.prefix() + "/term") == "2");
    }
};

}  // namespace kythira::conformance

/// The cases that need no failure injection — the set a real bucket can run.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_OBJECT_STORE_CONFORMANCE_NO_INJECTION(harness_type, suite_prefix)                 \
    BOOST_AUTO_TEST_SUITE(suite_prefix##_conformance)                                             \
    KYTHIRA_OSC_CASE(harness_type, engine_satisfies_the_persistence_engine_concept)               \
    KYTHIRA_OSC_CASE(harness_type, empty_bucket_or_prefix_is_rejected)                            \
    KYTHIRA_OSC_CASE(harness_type, empty_prefix_yields_empty_state_and_writes_nothing)            \
    KYTHIRA_OSC_CASE(harness_type, trailing_slash_in_prefix_is_normalised)                        \
    KYTHIRA_OSC_CASE(harness_type, keys_have_the_documented_shape)                                \
    KYTHIRA_OSC_CASE(harness_type, zero_padding_keeps_lexicographic_order_equal_to_numeric_order) \
    KYTHIRA_OSC_CASE(harness_type, a_default_engine_writes_exactly_the_documented_key_set)        \
    KYTHIRA_OSC_CASE(harness_type, term_round_trip)                                               \
    KYTHIRA_OSC_CASE(harness_type, term_survives_reload)                                          \
    KYTHIRA_OSC_CASE(harness_type, reads_never_touch_the_network)                                 \
    KYTHIRA_OSC_CASE(harness_type, voted_for_round_trip)                                          \
    KYTHIRA_OSC_CASE(harness_type, voted_for_survives_reload)                                     \
    KYTHIRA_OSC_CASE(harness_type, the_none_sentinel_loads_as_no_vote)                            \
    KYTHIRA_OSC_CASE(harness_type, append_is_exactly_one_put)                                     \
    KYTHIRA_OSC_CASE(harness_type, append_and_get)                                                \
    KYTHIRA_OSC_CASE(harness_type, log_survives_reload)                                           \
    KYTHIRA_OSC_CASE(harness_type, binary_command_survives_reload)                                \
    KYTHIRA_OSC_CASE(harness_type, embedded_nulls_and_high_bytes_survive)                         \
    KYTHIRA_OSC_CASE(harness_type, entry_type_survives_reload)                                    \
    KYTHIRA_OSC_CASE(harness_type, log_entries_range)                                             \
    KYTHIRA_OSC_CASE(harness_type, truncate_deletes_exactly_the_affected_objects)                 \
    KYTHIRA_OSC_CASE(harness_type, truncate_survives_reload)                                      \
    KYTHIRA_OSC_CASE(harness_type, delete_log_entries_before_removes_only_older_entries)          \
    KYTHIRA_OSC_CASE(harness_type, delete_before_survives_reload)                                 \
    KYTHIRA_OSC_CASE(harness_type, a_foreign_object_survives_truncation_and_compaction)           \
    KYTHIRA_OSC_CASE(harness_type, snapshot_round_trip)                                           \
    KYTHIRA_OSC_CASE(harness_type, snapshot_survives_reload)                                      \
    KYTHIRA_OSC_CASE(harness_type, snapshot_overwrites_the_single_slot)                           \
    KYTHIRA_OSC_CASE(harness_type, joint_consensus_configuration_survives_reload)                 \
    KYTHIRA_OSC_CASE(harness_type, retention_of_one_writes_no_retained_copy_and_costs_one_put)    \
    KYTHIRA_OSC_CASE(harness_type, a_retention_of_zero_is_rejected)                               \
    KYTHIRA_OSC_CASE(harness_type, retention_of_two_keeps_the_current_and_one_predecessor)        \
    KYTHIRA_OSC_CASE(harness_type, retention_of_n_prunes_oldest_first)                            \
    KYTHIRA_OSC_CASE(harness_type, a_retained_copy_is_byte_identical_to_the_current_snapshot)     \
    KYTHIRA_OSC_CASE(harness_type, retained_copies_are_pruned_across_a_restart)                   \
    KYTHIRA_OSC_CASE(harness_type, a_corrupt_retained_copy_does_not_break_startup)                \
    KYTHIRA_OSC_CASE(harness_type, pruning_leaves_alone_a_key_it_cannot_prove_it_wrote)           \
    KYTHIRA_OSC_CASE(harness_type, a_returned_write_has_already_reached_the_store)                \
    KYTHIRA_OSC_CASE(harness_type, a_corrupt_term_object_fails_construction_by_name)              \
    KYTHIRA_OSC_CASE(harness_type, a_partially_numeric_term_is_corruption_not_a_prefix)           \
    KYTHIRA_OSC_CASE(harness_type, a_corrupt_voted_for_object_fails_construction_by_name)         \
    KYTHIRA_OSC_CASE(harness_type, a_truncated_log_record_fails_construction_by_name)             \
    KYTHIRA_OSC_CASE(harness_type, a_log_record_disagreeing_with_its_key_fails_construction)      \
    KYTHIRA_OSC_CASE(harness_type, an_unpadded_log_key_fails_construction)                        \
    KYTHIRA_OSC_CASE(harness_type, a_corrupt_snapshot_fails_construction_by_name)                 \
    KYTHIRA_OSC_CASE(harness_type, a_non_base64_command_fails_construction)                       \
    KYTHIRA_OSC_CASE(harness_type, a_foreign_object_under_the_prefix_is_ignored)                  \
    BOOST_AUTO_TEST_SUITE_END()

/// The full suite: the above plus the cases that inject store failures.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_OBJECT_STORE_CONFORMANCE(harness_type, suite_prefix)                           \
    KYTHIRA_OBJECT_STORE_CONFORMANCE_NO_INJECTION(harness_type, suite_prefix)                  \
    BOOST_AUTO_TEST_SUITE(suite_prefix##_conformance_injection)                                \
    KYTHIRA_OSC_CASE(harness_type, a_failed_put_throws_and_leaves_the_mirror_unchanged)        \
    KYTHIRA_OSC_CASE(harness_type, a_transient_put_failure_is_retried_once_and_succeeds)       \
    KYTHIRA_OSC_CASE(harness_type, a_failed_delete_throws_and_is_not_retried)                  \
    KYTHIRA_OSC_CASE(harness_type, a_failed_load_fails_construction)                           \
    KYTHIRA_OSC_CASE(harness_type,                                                             \
                     a_failure_writing_the_retained_copy_leaves_the_previous_snapshot)         \
    KYTHIRA_OSC_CASE(harness_type, a_failure_at_the_commit_point_leaves_the_previous_snapshot) \
    KYTHIRA_OSC_CASE(harness_type, a_failure_pruning_leaves_the_new_snapshot_committed)        \
    BOOST_AUTO_TEST_SUITE_END()

/// The fencing cases (Requirement 9). Instantiated **in addition** to the suites
/// above, by a harness whose engine is `fencing_mode::compare_and_swap` — the
/// suites above are the negative control, so this macro is never the whole story
/// for a substrate.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_OBJECT_STORE_CONFORMANCE_FENCING(harness_type, suite_prefix)                   \
    BOOST_AUTO_TEST_SUITE(suite_prefix##_conformance_fencing)                                  \
    KYTHIRA_OSC_CASE(harness_type, construction_claims_the_prefix_with_an_owner_object)        \
    KYTHIRA_OSC_CASE(harness_type, a_restart_by_the_same_owner_advances_the_epoch)             \
    KYTHIRA_OSC_CASE(harness_type,                                                             \
                     construction_over_another_owners_prefix_throws_naming_the_owner)          \
    KYTHIRA_OSC_CASE(harness_type, a_takeover_epoch_above_the_recorded_epoch_takes_the_prefix) \
    KYTHIRA_OSC_CASE(harness_type, a_takeover_epoch_that_does_not_advance_is_rejected)         \
    KYTHIRA_OSC_CASE(harness_type, a_corrupt_owner_object_fails_construction_by_name)          \
    KYTHIRA_OSC_CASE(harness_type, the_two_engine_race_latches_the_loser)                      \
    KYTHIRA_OSC_CASE(harness_type, the_stale_appender_race_latches_one_writer)                 \
    KYTHIRA_OSC_CASE(harness_type, a_second_writers_snapshot_and_truncation_are_not_detected)  \
    KYTHIRA_OSC_CASE(harness_type, a_transient_failure_on_a_conditional_write_does_not_latch)  \
    BOOST_AUTO_TEST_SUITE_END()

/// One case. Kept separate so a suite can be assembled case by case where a
/// substrate cannot run all of them.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTHIRA_OSC_CASE(harness_type, case_name)                     \
    BOOST_AUTO_TEST_CASE(case_name, *boost::unit_test::timeout(60)) { \
        kythira::conformance::cases<harness_type>::case_name();       \
    }
