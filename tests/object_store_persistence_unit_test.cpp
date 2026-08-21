// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

/// The same harness with the engine fenced. It inherits every pass-through and
/// overrides only construction, so the two instantiations of the conformance
/// suite differ in exactly one thing: the fencing mode.
struct fenced_mock_object_store_harness : mock_object_store_harness {
    using store_t = ::store_t;
    using engine_t = kythira::fenced_object_store_persistence_engine<store_t>;

    /// Read by the conformance suite to branch the three cases that genuinely
    /// differ under fencing.
    static constexpr bool fenced = true;

    [[nodiscard]] static auto owner_id() -> std::string { return "node-a"; }

    auto make_engine() const -> engine_t { return make_engine(bucket(), prefix()); }

    auto make_engine(std::string bucket_name, std::string prefix_name) const -> engine_t {
        return engine_t{store, std::move(bucket_name), std::move(prefix_name),
                        with_owner(kythira::object_persistence_options{})};
    }

    auto make_engine(kythira::object_persistence_options opts) const -> engine_t {
        return engine_t{store, bucket(), prefix(), with_owner(std::move(opts))};
    }

    /// A *different* writer over the same `{bucket, prefix}` — the takeover and
    /// other-owner cases. `takeover_epoch` is passed through exactly as given,
    /// including a value that does not advance, because rejecting that is one of
    /// the behaviours under test.
    auto make_engine_as(std::string owner, std::optional<std::uint64_t> takeover_epoch) const
        -> engine_t {
        kythira::object_persistence_options opts;
        opts.owner_id = std::move(owner);
        opts.takeover_epoch = takeover_epoch;
        return engine_t{store, bucket(), prefix(), std::move(opts)};
    }

private:
    /// The suite's cases build options without knowing about fencing, so the
    /// harness fills in the identity the mode requires.
    [[nodiscard]] static auto with_owner(kythira::object_persistence_options opts)
        -> kythira::object_persistence_options {
        opts.owner_id = owner_id();
        return opts;
    }
};

/// The harness for a store whose version **is** the content MD5, like S3's and
/// OSS's ETag. Only the store type and the one injection knob differ; everything
/// else is the same pass-through, which is what keeps the checksum suite about
/// the engine rather than about a second fixture.
struct md5_versioned_mock_object_store_harness {
    using store_t = kythira::md5_versioned_mock_object_store;
    using engine_t = kythira::object_store_persistence_engine<store_t>;

    store_t store;

    [[nodiscard]] static auto bucket() -> std::string { return "kythira"; }
    [[nodiscard]] static auto prefix() -> std::string { return "raft"; }

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
    auto wrong_version_for_next_puts(int n) const -> void { store.wrong_version_for_next_puts(n); }
};

using engine_t = mock_object_store_harness::engine_t;
using fenced_engine_t = fenced_mock_object_store_harness::engine_t;
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

// …and again with fencing on. Running the *whole* suite a second time is the
// evidence that `compare_and_swap` — which rewrites the precondition on most of
// the engine's writes — broke nothing, and it is the negative control the fencing
// cases need: "the stale write was rejected" says nothing about a fence unless
// every legitimate write is still accepted.
KYTHIRA_OBJECT_STORE_CONFORMANCE(fenced_mock_object_store_harness, fenced_mock_object_store)
KYTHIRA_OBJECT_STORE_CONFORMANCE_FENCING(fenced_mock_object_store_harness, fenced_mock_object_store)

// …and Requirement 7.2's local verification, over the one substrate that can
// carry it: a store whose version *is* the content digest. The full suite runs
// here too, because "the engine verifies digests" is only worth having if every
// legitimate write still passes the verification.
KYTHIRA_OBJECT_STORE_CONFORMANCE(md5_versioned_mock_object_store_harness, md5_mock_object_store)
KYTHIRA_OBJECT_STORE_CONFORMANCE_CHECKSUM(md5_versioned_mock_object_store_harness,
                                          md5_mock_object_store)

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

// Requirement 9.8, as a negative compile test rather than a promise: a store that
// cannot express a precondition has no fenced engine to name. This is the
// assertion that fails if somebody "makes it work" by relaxing the constraint and
// letting `compare_and_swap` fall through to unconditional writes — which is the
// single most likely well-meaning change that would destroy the feature.
BOOST_AUTO_TEST_CASE(compare_and_swap_is_unavailable_for_a_store_without_conditional_writes) {
    struct versionless_store {
        auto put_object(const std::string&, const std::string&, std::string_view) const
            -> kythira::put_result;
        auto get_object(const std::string&, const std::string&) const
            -> std::optional<kythira::get_result>;
        auto delete_object(const std::string&, const std::string&) const -> void;
        auto list_keys(const std::string&, const std::string&) const -> std::vector<std::string>;
        auto provider_name() const -> std::string_view;
    };

    // Naming the specialization is the whole test: the constraint makes it
    // ill-formed for a store lacking the refinement, and well-formed for one that
    // has it. (The detection has to be a dependent requires-expression — hence
    // the concept from the engine header — because a non-dependent one naming an
    // ill-formed specialization is a hard error rather than `false`.)
    using kythira::object_store_persistence_detail::fenced_engine_instantiable;
    static_assert(!fenced_engine_instantiable<versionless_store>,
                  "a store without conditional writes must not have a fenced engine");
    static_assert(fenced_engine_instantiable<store_t>,
                  "…and a store with them must, or the test above proves nothing");

    // The unfenced engine over the same store is unaffected: losing the fencing
    // *option* is the only consequence of not satisfying the refinement.
    static_assert(
        kythira::persistence_engine<kythira::object_store_persistence_engine<versionless_store>,
                                    std::uint64_t, std::uint64_t, std::uint64_t, log_entry_t,
                                    snapshot_t>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ── The content digest (Requirement 7.2's local half) ────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_persistence_md5)

// The engine carries its own MD5 so it compiles with every cloud gate off, the
// same reason it carries its own base64. A hand-rolled digest with no
// known-answer test is worse than no digest, so this is RFC 1321 §A.5's full
// test suite verbatim.
BOOST_AUTO_TEST_CASE(md5_matches_the_rfc_1321_test_suite) {
    using kythira::object_store_persistence_detail::md5_hex;
    BOOST_TEST(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    BOOST_TEST(md5_hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    BOOST_TEST(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    BOOST_TEST(md5_hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    BOOST_TEST(md5_hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    BOOST_TEST(md5_hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
               "d174ab98d277d9f5a5611c2c9f419d9f");
    // RFC 1321's last vector is "1234567890" eight times. Built rather than
    // transcribed: an 80-character run of digits is a transcription error waiting
    // to happen, and the first attempt at this line was one — it hashed cleanly
    // to the wrong answer, which is exactly why a known-answer test is worth
    // having.
    std::string eighty;
    for (int i = 0; i < 8; ++i) {
        eighty += "1234567890";
    }
    BOOST_TEST(eighty.size() == 80U);
    BOOST_TEST(md5_hex(eighty) == "57edf4a22be3c955ac49da2e2107b67a");
}

// The padding boundaries are where an implementation of this shape actually goes
// wrong: at 56 bytes the length no longer fits in the first block, so a second
// block appears, and at 64 the message is already block-aligned.
BOOST_AUTO_TEST_CASE(md5_is_right_at_the_padding_boundaries) {
    using kythira::object_store_persistence_detail::md5_hex;
    BOOST_TEST(md5_hex(std::string(55, 'a')) == "ef1772b6dff9a122358552954ad0df65");
    BOOST_TEST(md5_hex(std::string(56, 'a')) == "3b0c8ac703f828b04c6c197006d17218");
    BOOST_TEST(md5_hex(std::string(63, 'a')) == "b06521f39153d618550606be297466d5");
    BOOST_TEST(md5_hex(std::string(64, 'a')) == "014842d480b571495a4a0363793f7367");
    BOOST_TEST(md5_hex(std::string(65, 'a')) == "c743a45e0d2e6a95cb859adae0248435");
    BOOST_TEST(md5_hex(std::string(1000, 'a')) == "cabe45dcc9ae5b66ba86600cca6b8ba8");
}

// Binary content, including embedded nulls and every high byte — a log entry's
// command is arbitrary bytes, so a digest that stopped at the first null would
// pass every text-based test above and verify nothing in practice.
BOOST_AUTO_TEST_CASE(md5_covers_binary_content_including_nulls) {
    using kythira::object_store_persistence_detail::md5_hex;
    std::string all_bytes;
    for (int i = 0; i < 256; ++i) {
        all_bytes += static_cast<char>(i);
    }
    BOOST_TEST(md5_hex(all_bytes) == "e2c865db4162bed963bfaa9ef6ac18f0");
    BOOST_TEST(md5_hex(std::string("a\0b", 3)) != md5_hex("a"));
}

// The comparison the engine performs, not the digest: S3 spells the hex
// lowercase and OSS uppercase, and OSS's client carries the ETag through with its
// quotes because it is an opaque token that goes back to the service unmodified.
// Normalising at the comparison is what keeps those two facts from having to
// agree.
BOOST_AUTO_TEST_CASE(the_digest_comparison_ignores_quotes_and_case) {
    using kythira::object_store_persistence_detail::normalise_content_digest;
    const std::string lower = "900150983cd24fb0d6963f7d28e17f72";
    BOOST_TEST(normalise_content_digest(lower) == lower);
    BOOST_TEST(normalise_content_digest("900150983CD24FB0D6963F7D28E17F72") == lower);
    BOOST_TEST(normalise_content_digest("\"900150983CD24FB0D6963F7D28E17F72\"") == lower);
    BOOST_TEST(normalise_content_digest("\"" + lower + "\"") == lower);
    // An opaque token is left alone rather than mangled into something that might
    // accidentally match.
    BOOST_TEST(normalise_content_digest("0x8DD1234ABCD") == "0x8dd1234abcd");
    BOOST_TEST(normalise_content_digest("") == "");
    BOOST_TEST(normalise_content_digest("\"") == "\"");
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

// The fencing options are rejected on an engine that is not fencing — the mirror
// image of "no field that is accepted and ignored". An owner_id silently ignored
// reads, to whoever set it, exactly like fencing being on.
BOOST_AUTO_TEST_CASE(fencing_options_on_an_unfenced_engine_are_rejected) {
    mock_object_store_harness h;
    kythira::object_persistence_options with_owner;
    with_owner.owner_id = "node-a";
    BOOST_CHECK_THROW((engine_t{h.store, mock_object_store_harness::bucket(),
                                mock_object_store_harness::prefix(), with_owner}),
                      std::invalid_argument);

    kythira::object_persistence_options with_epoch;
    with_epoch.takeover_epoch = 7;
    BOOST_CHECK_THROW((engine_t{h.store, mock_object_store_harness::bucket(),
                                mock_object_store_harness::prefix(), with_epoch}),
                      std::invalid_argument);

    // …and nothing was written on the way to either rejection.
    BOOST_TEST(h.keys().empty());
}

// A fenced engine with no owner_id would write an owner object naming nobody,
// which is worse than not fencing: it claims the prefix without saying for whom.
BOOST_AUTO_TEST_CASE(a_fenced_engine_requires_an_owner_id) {
    fenced_mock_object_store_harness h;
    BOOST_CHECK_THROW((fenced_engine_t{h.store, fenced_mock_object_store_harness::bucket(),
                                       fenced_mock_object_store_harness::prefix(),
                                       kythira::object_persistence_options{}}),
                      std::invalid_argument);
    BOOST_TEST(h.keys().empty());
}

// The mode is visible on the type, which is what lets a caller assert it rather
// than infer it from behaviour.
BOOST_AUTO_TEST_CASE(the_fencing_mode_is_part_of_the_engine_type) {
    static_assert(engine_t::fencing == kythira::fencing_mode::none);
    static_assert(fenced_engine_t::fencing == kythira::fencing_mode::compare_and_swap);

    mock_object_store_harness unfenced;
    engine_t eng = unfenced.make_engine();
    // An unfenced engine never latches, and says so without being asked twice.
    BOOST_TEST(!eng.is_fenced());
    BOOST_TEST(eng.owner_epoch() == 0U);
}

// `fencing_mode::none` writes no owner object even when one is already there:
// under `none` that key is a foreign object like any other, which is what "no
// extra request cost" means and what keeps an unfenced engine from stamping its
// name over a fenced deployment's claim.
BOOST_AUTO_TEST_CASE(an_unfenced_engine_neither_reads_nor_writes_the_owner_object) {
    mock_object_store_harness h;
    h.seed(mock_object_store_harness::prefix() + "/owner",
           "{\"owner_id\":\"node-a\",\"epoch\":4,\"started_at\":\"2026-08-17T00:00:00Z\"}");
    h.clear_requests();

    engine_t eng = h.make_engine();
    BOOST_CHECK_NO_THROW(eng.save_current_term(1));
    BOOST_TEST(h.body(mock_object_store_harness::prefix() + "/owner").find("\"epoch\":4") !=
               std::string::npos);
    BOOST_TEST(h.count_requests("GET") == 0U);
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
