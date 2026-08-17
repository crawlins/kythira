#pragma once

/// @file mock_object_store.hpp
/// @brief An in-memory `kythira::key_object_store` (and
///        `conditional_key_object_store`) with a request log and injectable
///        failures — the substrate the conformance suite runs against in the
///        unit tier.
///
/// This is deliberately *not* a mock HTTP server. The per-provider mock servers
/// in this tree (`alibaba_mock_server.hpp`, `oci_mock_server.hpp`) exist to
/// prove a **client** speaks its provider's wire protocol, signatures included.
/// This one exists to prove the **engine**, which is provider-independent, so
/// it implements the concept directly: no sockets, no signing, no XML, and
/// therefore no way for a transport detail to be mistaken for engine behaviour.
///
/// The state lives behind a `shared_ptr`, so a copy of the handle handed to an
/// engine and the copy kept by the test observe the same bucket. That matters
/// because `object_store_persistence_engine` takes its store **by value**: a
/// test must be able to inspect what the engine wrote after handing the store
/// over, and to construct a second engine over the same bucket to prove state
/// survived a "restart".

#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

/// @brief One stored object: its bytes and the version the store assigned.
struct mock_stored_object {
    std::string body;
    object_version version;
};

/// @brief The shared bucket behind every copy of a `mock_object_store` handle.
struct mock_object_store_state {
    mutable std::mutex mu;

    /// Ordered, like every one of these services' listings.
    std::map<std::string, mock_stored_object> objects;

    /// Every operation, in order: `"PUT <key>"`, `"GET <key>"`,
    /// `"DELETE <key>"`, `"LIST <prefix>"`. The durability-ordering assertions
    /// read this to prove a write reached the store *before* its method
    /// returned.
    std::vector<std::string> requests;

    /// Monotonic version source. A real store's token is an ETag or a
    /// generation; all that matters here is that it changes on every write.
    std::size_t version_counter{0};

    // ── Injection knobs ──────────────────────────────────────────────────────

    /// Fail this many further PUTs, then go back to storing — the transient
    /// failure the engine's single retry is supposed to ride out.
    int fail_next_puts{0};
    /// Fail this many further GETs / DELETEs / LISTs.
    int fail_next_gets{0};
    int fail_next_deletes{0};
    int fail_next_lists{0};
    /// Fail every PUT whose key is exactly this, however many times it is sent.
    std::string fail_puts_for_key;
    /// Keys the listing pretends not to see yet — a **lagging index**, the one
    /// consistency failure the engine cannot detect at runtime (it recovers a
    /// silently short log). Nothing else about them changes: a GET still
    /// returns them.
    std::set<std::string> hidden_from_listing;

    /// Report a version that is *not* the digest of what was stored, for this
    /// many further PUTs — a corrupted transfer, on a store whose version is
    /// supposed to be the content MD5. Inert on the plain store, whose version
    /// carries no such promise.
    int wrong_version_for_next_puts{0};
};

/// @brief A copyable handle onto an in-memory bucket, satisfying both store
///        concepts.
///
/// @tparam VersionIsContentMd5 mint the version as the content MD5, quoted and
///         uppercased, the way S3's and OSS's ETag behave — and declare
///         `version_is_content_md5` so the engine performs Requirement 7.2's
///         local verification. `false` mints an opaque counter instead, like
///         Azure, GCS and OCI. Two types rather than a runtime flag because the
///         declaration is a compile-time property of the store, which is what
///         lets the engine compute no digest at all where there is nothing to
///         compare against.
template<bool VersionIsContentMd5> class basic_mock_object_store {
public:
    /// Read by `kythira::content_md5_versioned_store`. Present on both
    /// instantiations, `false` on one — a store may equally well say so
    /// explicitly.
    static constexpr bool version_is_content_md5 = VersionIsContentMd5;

    basic_mock_object_store() : _state(std::make_shared<mock_object_store_state>()) {}

    /// Share an existing bucket — this is how a test keeps its own handle after
    /// handing one to an engine, and how two engines are pointed at one bucket.
    explicit basic_mock_object_store(std::shared_ptr<mock_object_store_state> state)
        : _state(std::move(state)) {}

    [[nodiscard]] auto state() const -> const std::shared_ptr<mock_object_store_state>& {
        return _state;
    }

    // ── key_object_store ─────────────────────────────────────────────────────

    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> put_result {
        const std::lock_guard lock(_state->mu);
        _state->requests.push_back("PUT " + key);
        require_bucket(bucket);
        if (_state->fail_next_puts > 0) {
            --_state->fail_next_puts;
            throw std::runtime_error("mock_object_store: injected PUT failure for " + key);
        }
        if (!_state->fail_puts_for_key.empty() && _state->fail_puts_for_key == key) {
            throw std::runtime_error("mock_object_store: injected PUT failure for " + key);
        }
        return {store_locked(key, std::string(bytes))};
    }

    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        const std::lock_guard lock(_state->mu);
        _state->requests.push_back("GET " + key);
        require_bucket(bucket);
        if (_state->fail_next_gets > 0) {
            --_state->fail_next_gets;
            throw std::runtime_error("mock_object_store: injected GET failure for " + key);
        }
        auto it = _state->objects.find(key);
        if (it == _state->objects.end()) {
            return std::nullopt;
        }
        return get_result{it->second.body, it->second.version};
    }

    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->requests.push_back("DELETE " + key);
        require_bucket(bucket);
        if (_state->fail_next_deletes > 0) {
            --_state->fail_next_deletes;
            throw std::runtime_error("mock_object_store: injected DELETE failure for " + key);
        }
        // Deleting an absent key succeeds, as it does on all five services.
        _state->objects.erase(key);
    }

    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        const std::lock_guard lock(_state->mu);
        _state->requests.push_back("LIST " + prefix);
        require_bucket(bucket);
        if (_state->fail_next_lists > 0) {
            --_state->fail_next_lists;
            throw std::runtime_error("mock_object_store: injected LIST failure for " + prefix);
        }
        std::vector<std::string> keys;
        for (const auto& [key, _] : _state->objects) {
            if (key.starts_with(prefix) && !_state->hidden_from_listing.contains(key)) {
                keys.push_back(key);
            }
        }
        return keys;
    }

    [[nodiscard]] static auto provider_name() -> std::string_view { return "mock"; }

    // ── conditional_key_object_store ─────────────────────────────────────────

    auto put_object_if(const std::string& bucket, const std::string& key, std::string_view bytes,
                       const precondition& pre) const -> put_result {
        const std::lock_guard lock(_state->mu);
        _state->requests.push_back("PUT " + key);
        require_bucket(bucket);
        // The precondition is checked first, exactly as a service does: a request
        // whose precondition fails never reaches the storage path, so an injected
        // transport failure cannot mask a fence (or be mistaken for one).
        check_precondition_locked(key, pre);
        if (_state->fail_next_puts > 0) {
            --_state->fail_next_puts;
            throw std::runtime_error("mock_object_store: injected PUT failure for " + key);
        }
        if (!_state->fail_puts_for_key.empty() && _state->fail_puts_for_key == key) {
            // A plain `runtime_error`, deliberately: this is the shape a client
            // gives a transient failure or a benign conditional-request race
            // (S3's 409), and the engine must not latch on it.
            throw std::runtime_error("mock_object_store: injected PUT failure for " + key);
        }
        return {store_locked(key, std::string(bytes))};
    }

    auto delete_object_if(const std::string& bucket, const std::string& key,
                          const precondition& pre) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->requests.push_back("DELETE " + key);
        require_bucket(bucket);
        check_precondition_locked(key, pre);
        _state->objects.erase(key);
    }

    // ── Inspection and injection, from the test thread ───────────────────────

    /// Write directly, bypassing the request log — how a test seeds a corrupt
    /// or foreign object into the bucket.
    auto seed(const std::string& key, std::string_view body) const -> void {
        const std::lock_guard lock(_state->mu);
        store_locked(key, std::string(body));
    }

    [[nodiscard]] auto keys() const -> std::vector<std::string> {
        const std::lock_guard lock(_state->mu);
        std::vector<std::string> out;
        out.reserve(_state->objects.size());
        for (const auto& [key, _] : _state->objects) {
            out.push_back(key);
        }
        return out;
    }

    [[nodiscard]] auto has(const std::string& key) const -> bool {
        const std::lock_guard lock(_state->mu);
        return _state->objects.contains(key);
    }

    [[nodiscard]] auto body(const std::string& key) const -> std::string {
        const std::lock_guard lock(_state->mu);
        auto it = _state->objects.find(key);
        return it == _state->objects.end() ? std::string{} : it->second.body;
    }

    [[nodiscard]] auto version_of(const std::string& key) const -> object_version {
        const std::lock_guard lock(_state->mu);
        auto it = _state->objects.find(key);
        return it == _state->objects.end() ? object_version{} : it->second.version;
    }

    [[nodiscard]] auto request_log() const -> std::vector<std::string> {
        const std::lock_guard lock(_state->mu);
        return _state->requests;
    }

    [[nodiscard]] auto count_requests(std::string_view verb) const -> std::size_t {
        const std::lock_guard lock(_state->mu);
        return static_cast<std::size_t>(
            std::count_if(_state->requests.begin(), _state->requests.end(),
                          [verb](const std::string& r) { return r.starts_with(verb); }));
    }

    auto clear_requests() const -> void {
        const std::lock_guard lock(_state->mu);
        _state->requests.clear();
    }

    auto fail_next_puts(int n) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->fail_next_puts = n;
    }

    auto fail_puts_for_key(std::string key) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->fail_puts_for_key = std::move(key);
    }

    auto fail_next_gets(int n) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->fail_next_gets = n;
    }

    auto fail_next_deletes(int n) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->fail_next_deletes = n;
    }

    auto fail_next_lists(int n) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->fail_next_lists = n;
    }

    /// Make the listing lag behind an acknowledged write, without touching what
    /// a GET returns.
    auto hide_from_listing(std::string key) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->hidden_from_listing.insert(std::move(key));
    }

    /// Report a version that does not match what was stored, for the next `n`
    /// PUTs — a corrupted transfer. The object is still stored, exactly as a
    /// service would store what actually arrived; it is the *reported digest*
    /// that disagrees, which is the whole point of an end-to-end checksum.
    auto wrong_version_for_next_puts(int n) const -> void {
        const std::lock_guard lock(_state->mu);
        _state->wrong_version_for_next_puts = n;
    }

private:
    /// Callers hold `_state->mu`.
    auto store_locked(const std::string& key, std::string body) const -> object_version {
        ++_state->version_counter;
        object_version version;
        if constexpr (VersionIsContentMd5) {
            // Quoted and uppercase, like OSS's ETag — which is also how the
            // engine's comparison gets held to being quote- and case-insensitive
            // rather than only claiming to be.
            version = '"' + to_upper(kythira::object_store_persistence_detail::md5_hex(body)) + '"';
        } else {
            version = "v" + std::to_string(_state->version_counter);
        }
        if (_state->wrong_version_for_next_puts > 0) {
            --_state->wrong_version_for_next_puts;
            // The digest of something else entirely: what a corrupted transfer
            // looks like from the caller's side.
            version = '"' +
                      to_upper(kythira::object_store_persistence_detail::md5_hex(
                          "not what the caller sent")) +
                      '"';
        }
        _state->objects[key] = mock_stored_object{std::move(body), version};
        return version;
    }

    static auto to_upper(std::string s) -> std::string {
        for (char& c : s) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return s;
    }

    /// Callers hold `_state->mu`. A rejected precondition throws
    /// `object_precondition_failed` and nothing else does — the distinction the
    /// engine's fencing rests on.
    auto check_precondition_locked(const std::string& key, const precondition& pre) const -> void {
        auto it = _state->objects.find(key);
        if (std::holds_alternative<if_absent>(pre)) {
            if (it != _state->objects.end()) {
                throw object_precondition_failed(std::string(provider_name()), key, {},
                                                 "object already exists");
            }
            return;
        }
        const auto& expected = std::get<if_version>(pre).expected;
        if (it == _state->objects.end()) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             "object does not exist");
        }
        if (it->second.version != expected) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             "object is at version " + it->second.version);
        }
    }

    auto require_bucket(const std::string& bucket) const -> void {
        if (bucket.empty()) {
            throw std::invalid_argument("mock_object_store: empty bucket");
        }
    }

    std::shared_ptr<mock_object_store_state> _state;
};

/// The default substrate: an opaque version, like Azure, GCS and OCI.
using mock_object_store = basic_mock_object_store<false>;

/// The substrate for Requirement 7.2's local verification: the version **is** the
/// content MD5, like S3's and OSS's ETag.
using md5_versioned_mock_object_store = basic_mock_object_store<true>;

static_assert(key_object_store<mock_object_store>,
              "mock_object_store must satisfy key_object_store");
static_assert(conditional_key_object_store<mock_object_store>,
              "mock_object_store must satisfy conditional_key_object_store");
static_assert(!content_md5_versioned_store<mock_object_store>,
              "the plain mock store's version must stay opaque — it is what pins that the engine "
              "computes no digest where there is nothing to compare against");
static_assert(conditional_key_object_store<md5_versioned_mock_object_store>);
static_assert(content_md5_versioned_store<md5_versioned_mock_object_store>,
              "md5_versioned_mock_object_store must declare its version is the content MD5");

}  // namespace kythira
