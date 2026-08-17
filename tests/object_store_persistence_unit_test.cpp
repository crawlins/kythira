#define BOOST_TEST_MODULE object_store_persistence_unit_test
#include <boost/test/unit_test.hpp>

#include "mock_object_store.hpp"
#include "object_store_conformance.hpp"

#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#endif

#include <cstdint>
#include <string>
#include <utility>

// Unit coverage for the generic engine (spec Requirements 2.1–2.6, 17.1–17.3).
//
// The conformance suite is the substance — one test body, proved here against
// `mock_object_store` and instantiated per provider elsewhere. This file adds
// only what is specific to *this* substrate: the harness, the concept
// satisfaction checks, options validation, and the provider-independent fault
// points.
//
// Nothing here speaks HTTP. The per-provider mock servers exist to prove a
// client's wire protocol; the engine has no wire protocol, so testing it
// through one would only add a way for a transport detail to be mistaken for
// engine behaviour.

namespace {

using store_t = kythira::mock_object_store;

/// The conformance harness for the in-memory store. Everything is a
/// pass-through: the harness exists so the suite can be written once against an
/// interface every substrate can present.
struct mock_object_store_harness {
    using store_t = ::store_t;
    using engine_t = kythira::object_store_persistence_engine<store_t>;

    store_t store;

    [[nodiscard]] static auto bucket() -> std::string { return "kythira"; }
    [[nodiscard]] static auto prefix() -> std::string { return "raft"; }

    // Deliberately not [[nodiscard]]: the construction-failure cases call this
    // inside BOOST_CHECK_THROW, where discarding the result is the point.
    auto make_engine() const -> engine_t { return make_engine(bucket(), prefix()); }

    auto make_engine(std::string bucket_name, std::string prefix_name) const -> engine_t {
        return engine_t{store, std::move(bucket_name), std::move(prefix_name)};
    }

    auto make_engine(kythira::object_persistence_options opts) const -> engine_t {
        return engine_t{store, bucket(), prefix(), opts};
    }

    auto seed(const std::string& key, std::string_view body) const -> void {
        store.seed(key, body);
    }
    [[nodiscard]] auto keys() const -> std::vector<std::string> { return store.keys(); }
    [[nodiscard]] auto has(const std::string& key) const -> bool { return store.has(key); }
    [[nodiscard]] auto body(const std::string& key) const -> std::string { return store.body(key); }
    [[nodiscard]] auto request_log() const -> std::vector<std::string> {
        return store.request_log();
    }
    [[nodiscard]] auto count_requests(std::string_view verb) const -> std::size_t {
        return store.count_requests(verb);
    }
    auto clear_requests() const -> void { store.clear_requests(); }
    auto fail_next_puts(int n) const -> void { store.fail_next_puts(n); }
    auto fail_puts_for_key(std::string key) const -> void {
        store.fail_puts_for_key(std::move(key));
    }
    auto fail_next_gets(int n) const -> void { store.fail_next_gets(n); }
    auto fail_next_deletes(int n) const -> void { store.fail_next_deletes(n); }
    auto fail_next_lists(int n) const -> void { store.fail_next_lists(n); }
};

using engine_t = mock_object_store_harness::engine_t;
using log_entry_t = kythira::log_entry<>;
using snapshot_t = kythira::snapshot<>;

auto make_entry(std::uint64_t term, std::uint64_t index) -> log_entry_t {
    log_entry_t e;
    e._term = term;
    e._index = index;
    e._command = {std::byte{0xAB}};
    return e;
}

#ifdef FIU_ENABLE
struct FiuInitFixture {
    FiuInitFixture() { fiu_init(0); }
};
BOOST_GLOBAL_FIXTURE(FiuInitFixture);
#endif

}  // namespace

// ── The conformance suite, instantiated. One line, which is the point. ───────

KYTHIRA_OBJECT_STORE_CONFORMANCE(mock_object_store_harness, mock_object_store)

// ── Concepts ─────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_persistence_concept)

BOOST_AUTO_TEST_CASE(the_mock_store_satisfies_both_store_concepts) {
    static_assert(kythira::key_object_store<store_t>);
    static_assert(kythira::conditional_key_object_store<store_t>);
    BOOST_TEST(true);
}

// The header carries a file-scope static_assert over a synthetic store too;
// this one keeps the failure legible in the test output, and pins it for a
// *real* store rather than the synthetic one.
BOOST_AUTO_TEST_CASE(the_engine_satisfies_the_persistence_engine_concept) {
    static_assert(kythira::persistence_engine<engine_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                              log_entry_t, snapshot_t>,
                  "object_store_persistence_engine must satisfy persistence_engine");
    BOOST_TEST(true);
}

// A store that reports no version is still a `key_object_store` — it is only
// disqualified from Requirement 9's CAS fencing, and from nothing else. This
// pins that the base concept does not accidentally require the refinement.
BOOST_AUTO_TEST_CASE(a_store_without_conditional_writes_still_satisfies_the_base_concept) {
    struct versionless_store {
        auto put_object(const std::string&, const std::string&, std::string_view) const
            -> kythira::put_result;
        auto get_object(const std::string&, const std::string&) const
            -> std::optional<kythira::get_result>;
        auto delete_object(const std::string&, const std::string&) const -> void;
        auto list_keys(const std::string&, const std::string&) const -> std::vector<std::string>;
        auto provider_name() const -> std::string_view;
    };
    static_assert(kythira::key_object_store<versionless_store>);
    static_assert(!kythira::conditional_key_object_store<versionless_store>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Options ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_persistence_options_suite)

// The default is the shipped behaviour: one retry, PUT-only, single snapshot
// slot.
BOOST_AUTO_TEST_CASE(the_default_options_are_the_shipped_behaviour) {
    const kythira::object_persistence_options opts;
    BOOST_TEST(opts.write_retries == 1U);
    BOOST_TEST(opts.snapshot_retention == 1U);
}

// With retries disabled the first failure is the last word, and the message
// says so rather than claiming a retry that never happened.
BOOST_AUTO_TEST_CASE(zero_retries_means_one_attempt) {
    mock_object_store_harness h;
    engine_t eng{h.store, mock_object_store_harness::bucket(), mock_object_store_harness::prefix(),
                 kythira::object_persistence_options{.write_retries = 0}};
    h.clear_requests();
    h.fail_next_puts(1);

    BOOST_CHECK_THROW(eng.save_current_term(4), std::runtime_error);
    BOOST_TEST(h.count_requests("PUT") == 1U);
    BOOST_TEST(eng.load_current_term() == 0U);
}

// …and more retries than one are honoured, so the knob is a knob.
BOOST_AUTO_TEST_CASE(extra_retries_are_honoured) {
    mock_object_store_harness h;
    engine_t eng{h.store, mock_object_store_harness::bucket(), mock_object_store_harness::prefix(),
                 kythira::object_persistence_options{.write_retries = 3}};
    h.clear_requests();
    h.fail_next_puts(3);

    BOOST_CHECK_NO_THROW(eng.save_current_term(4));
    BOOST_TEST(h.count_requests("PUT") == 4U);
    BOOST_TEST(eng.load_current_term() == 4U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── The store handle is shared, so "reload" means what the tests think ───────

BOOST_AUTO_TEST_SUITE(object_store_persistence_engine_lifecycle)

// Two engines over one bucket is the *restart* case, not the concurrent-writer
// case: the second sees everything the first acknowledged. (Detecting a genuine
// second writer is Requirement 9's fencing, which is not implemented yet.)
BOOST_AUTO_TEST_CASE(a_fresh_engine_sees_a_previous_engines_acknowledged_writes) {
    mock_object_store_harness h;
    {
        engine_t writer = h.make_engine();
        writer.save_current_term(7);
        writer.save_voted_for(3);
        writer.append_log_entry(make_entry(7, 1));
        writer.append_log_entry(make_entry(7, 2));
    }
    engine_t reader = h.make_engine();
    BOOST_TEST(reader.load_current_term() == 7U);
    BOOST_TEST(reader.load_voted_for().value() == 3U);
    BOOST_TEST(reader.get_last_log_index() == 2U);
}

// The engine names its store, so an error message and a backup manifest can
// say which service they came from without the engine knowing anything else
// about it.
BOOST_AUTO_TEST_CASE(the_engine_exposes_its_stores_provider_name) {
    mock_object_store_harness h;
    engine_t eng = h.make_engine();
    BOOST_TEST(eng.store().provider_name() == std::string_view{"mock"});
}

// A lagging listing is the one consistency failure the engine cannot detect:
// the recovered log is silently short. Pinned here so the failure mode is
// *documented in a test* rather than only in prose — closing it is a provider
// obligation (spec Requirement 5.4), not something the engine can do.
BOOST_AUTO_TEST_CASE(a_lagging_listing_silently_shortens_the_recovered_log) {
    mock_object_store_harness h;
    {
        engine_t writer = h.make_engine();
        writer.append_log_entry(make_entry(1, 1));
        writer.append_log_entry(make_entry(1, 2));
        writer.append_log_entry(make_entry(1, 3));
    }
    h.store.hide_from_listing(mock_object_store_harness::prefix() + "/log/" + std::string(19, '0') +
                              "3");

    engine_t reader = h.make_engine();
    BOOST_TEST(reader.get_last_log_index() == 2U);
    // The object is still there — nothing was lost in the store, only in the
    // listing, which is exactly why no error is raised.
    BOOST_TEST(h.has(mock_object_store_harness::prefix() + "/log/" + std::string(19, '0') + "3"));
}

BOOST_AUTO_TEST_SUITE_END()

// ── Fault points (Requirement 2.5) ───────────────────────────────────────────

#ifdef FIU_ENABLE

BOOST_AUTO_TEST_SUITE(object_store_persistence_fault_injection)

// The names carry no provider: a chaos configuration written against this
// engine works for every store it is instantiated over.
BOOST_AUTO_TEST_CASE(the_put_object_fault_fails_the_write_without_touching_the_store) {
    mock_object_store_harness h;
    engine_t eng = h.make_engine();
    eng.save_current_term(2);
    h.clear_requests();

    fiu_enable("raft/objstore/put_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(eng.save_current_term(3), std::runtime_error);
    fiu_disable("raft/objstore/put_object");

    BOOST_TEST(h.count_requests("PUT") == 0U);
    BOOST_TEST(eng.load_current_term() == 2U);
    BOOST_TEST(h.body(mock_object_store_harness::prefix() + "/term") == "2");
}

BOOST_AUTO_TEST_CASE(the_delete_object_fault_fails_truncation) {
    mock_object_store_harness h;
    engine_t eng = h.make_engine();
    eng.append_log_entry(make_entry(1, 1));
    h.clear_requests();

    fiu_enable("raft/objstore/delete_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(eng.truncate_log(1), std::runtime_error);
    fiu_disable("raft/objstore/delete_object");

    BOOST_TEST(h.count_requests("DELETE") == 0U);
    BOOST_TEST(eng.get_last_log_index() == 1U);
}

BOOST_AUTO_TEST_CASE(the_get_object_fault_fails_construction) {
    mock_object_store_harness h;
    {
        engine_t eng = h.make_engine();
        eng.save_current_term(5);
    }
    fiu_enable("raft/objstore/get_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);
    fiu_disable("raft/objstore/get_object");
}

BOOST_AUTO_TEST_CASE(the_list_object_fault_fails_construction) {
    mock_object_store_harness h;
    fiu_enable("raft/objstore/list_object", 1, nullptr, 0);
    BOOST_CHECK_THROW(h.make_engine(), std::runtime_error);
    fiu_disable("raft/objstore/list_object");

    BOOST_CHECK_NO_THROW(h.make_engine());
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // FIU_ENABLE
