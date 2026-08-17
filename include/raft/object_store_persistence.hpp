#pragma once

/// @file object_store_persistence.hpp
/// @brief Raft persistent state in a cloud key-object store — one object per
///        state item, synchronous writes, an in-memory read mirror; generic
///        over any `kythira::key_object_store`.
///
/// This is `alibaba_oss_persistence_engine`'s body, hoisted. That engine was
/// live-verified end to end against a real OSS bucket (August 14, 2026 — all
/// four real cases, including a fresh engine reading back another engine's
/// writes), and everything below — the layout, the mirror, the ordering, the
/// parse-or-throw load, the single idempotent-PUT retry — is that working
/// reference made generic, not a redesign. `alibaba_oss_persistence_engine` is
/// now an instantiation of this template over `alibaba_oss_client`.
///
/// ## Object layout
///
/// Everything one engine instance owns lives under one operator-supplied
/// `prefix` in one bucket:
///
///     <prefix>/term                        decimal text, e.g. "42"
///     <prefix>/voted_for                   decimal text or the literal "none"
///     <prefix>/log/<20-digit index>        one JSON record per log entry
///     <prefix>/snapshot                    one JSON record, single slot
///
/// The log-entry and snapshot records use **the same one-line JSON codec
/// `file_persistence_engine` writes** (`{term, index, command, type}` with the
/// command base64-encoded, and `{last_included_index, last_included_term,
/// state, nodes, is_joint_consensus, old_nodes?}` for a snapshot), so the two
/// engines' records are byte-comparable and a bucket can be diffed against a
/// data directory during a migration.
///
/// ### Why one object per log entry
///
/// The file engine keeps its whole log in a single file and rewrites that file
/// on every truncation. That is the right shape for a local filesystem, where a
/// rewrite is one `write`+`rename` and costs microseconds. It is the wrong shape
/// for object storage, where a rewrite is a full re-upload of the entire log on
/// every conflict resolution — O(log size) network bytes for an O(1) logical
/// change. Keying one object per entry instead makes:
///
///   * `append_log_entry`            → exactly one PUT,
///   * `truncate_log` / `delete_log_entries_before`
///                                   → a bounded batch of DELETEs, one per
///                                     affected entry and no others,
///   * recovery                      → one List plus one GET per live object.
///
/// ### Why 20 zero-padded digits
///
/// Every one of these stores lists keys in lexicographic order. `2^64 - 1` is
/// 20 digits, so padding every index to exactly 20 digits makes lexicographic
/// key order identical to numeric index order for every representable
/// `LogIndex` — recovery, range scans, and any human reading a bucket listing
/// all see the log in order. Without the padding, `.../log/10` would sort
/// before `.../log/9`. Recovery correctness rests on this, which is why a key
/// whose suffix is not exactly 20 digits is treated as corruption rather than
/// as a key to skip.
///
/// ## Durability contract, stated head-on
///
/// **Every mutating method issues its PUT or DELETE synchronously on the
/// calling thread and returns only after the store has answered 2xx.** There is
/// no write buffer, no background flusher, and no asynchronous path anywhere in
/// this engine — the call stack of `save_current_term` contains the socket
/// write and the socket read of the response.
///
/// That is the fsync-equivalence argument, and it is per provider: each store
/// documents what a successful write response means (replication scope, storage
/// class and account-redundancy caveats), and `doc/cloud_object_persistence.md`
/// carries that evidence table with its `documentation-derived` /
/// `live-verified` column intact. Alibaba OSS is the one row live-verified in
/// this repo. Where the claim holds, "the method returned" implies "the bytes
/// survive the loss of this instance", which is precisely what Raft's §5
/// durability requirement asks of `currentTerm` and `votedFor` before a node
/// acts on them.
///
/// This is **stronger** than `file_persistence_engine`, which writes to a
/// temporary file and renames it. That idiom gives atomicity — a reader never
/// sees a half-written file — but it does *not* give durability: neither the
/// data write nor the directory entry is fsynced, so a power loss can lose an
/// acknowledged term. The file engine is honest about being a single-host
/// development/chaos-node backend; this engine is the one you can actually lose
/// the instance under.
///
/// ### The honest cost
///
/// `save_current_term` and `save_voted_for` sit on the **election hot path** — a
/// candidate persists its term and self-vote before sending a single
/// RequestVote, and a follower persists a term bump before replying. With this
/// engine each of those is a **network round trip** rather than a
/// sub-millisecond local write, so one election round costs four sequential
/// durable writes plus an RPC round trip, giving the sizing rule
///
///     election_timeout_min  ≥  4 × p99(PUT) + rpc_rtt + margin
///
/// and the same quantity as a floor on the randomized election-timeout *range
/// width*, or nodes re-time-out inside each other's elections and livelock.
/// Size from the measured figure, not from an estimate: the real tier records
/// per-operation latency with its measurement position (in-region instance vs.
/// out-of-region host), and the operator documentation quotes it that way. The
/// only figure this project has measured so far — ~2–3 s per round trip — is a
/// cross-ocean upper bound and is not a production number.
///
/// Sustained append throughput per node cannot exceed roughly one entry per
/// store round trip, because `kythira::persistence_engine` has no batch
/// operation and this engine deliberately adds no latency-hiding mechanism:
/// no batching, no write coalescing, no write-behind, no asynchronous flush,
/// no relaxed-durability mode. Recovering append throughput requires widening
/// the concept every engine implements, which is a raft-layer change and a
/// recorded follow-on rather than something to smuggle in under a
/// configuration flag.
///
/// ## Read path: an in-memory mirror
///
/// All state is mirrored in memory behind a mutex and loaded exactly once, in
/// the constructor, with one List and one GET per live object. Every read
/// method (`load_current_term`, `get_log_entry`, `get_log_entries`,
/// `get_last_log_index`, `load_voted_for`, `load_snapshot`) answers from that
/// mirror and performs **no** network I/O — the Raft hot loop reads the log far
/// more often than it writes it, and a GET per read would be unusable.
///
/// The mirror looks like a performance optimization and is also a correctness
/// simplification: because reads happen once per process lifetime, the engine
/// is exposed to a provider's read consistency exactly once instead of
/// continuously.
///
/// The mirror is updated **after** the store call succeeds, never before. A
/// write that throws therefore leaves memory exactly equal to the store: the
/// caller treats the operation as failed, and a retry re-executes the full PUT.
///
/// ## Retry policy
///
/// A failed mutating call is retried **once, and only when it is a full-object
/// PUT** (`object_persistence_options::write_retries`). Every PUT here is a
/// deterministic, keyed, full-object overwrite — `<prefix>/term`,
/// `<prefix>/voted_for`, `<prefix>/snapshot`, and `<prefix>/log/<index>` all
/// name their object from the value being written — so re-sending it is
/// idempotent by construction: the second attempt writes the identical bytes to
/// the identical key. DELETEs are *not* retried here; the engine leaves that to
/// the caller, which is free to re-run the whole truncation (itself
/// idempotent). After the retry fails the method throws, and the raft layer's
/// existing treatment of persistence exceptions applies unchanged.
///
/// ## Corruption is fatal at load
///
/// If any object under the prefix fails to parse, **construction throws** and
/// the message names the offending key. This is a deliberate contrast with
/// `file_persistence_engine`, whose `load_all` wraps every parse in
/// `catch (...) {}` and silently drops what it cannot read — a limitation its
/// own header records. A truncated or half-uploaded object in a cloud store
/// must not degrade into silent state loss: a Raft node that comes back with a
/// silently shortened log can violate the Log Matching property. Failing to
/// start is recoverable by an operator; starting with invented state is not.
///
/// ## Single-writer requirement
///
/// **Exactly one process may own a `{bucket, prefix}` pair**, exactly as one
/// process owns a `file_persistence_engine`'s directory. Two nodes pointed at
/// the same prefix will corrupt each other's state. Conditional-PUT fencing —
/// which *detects* a second writer rather than coordinating with one — is
/// specified in `.kiro/specs/cloud-object-persistence/` Requirement 9 and is
/// not yet implemented here; until it is, single-writer is by assertion.
///
/// ## Provider independence
///
/// Nothing in this header names a provider, a vendor error code or a
/// `KYTHIRA_HAS_*` gate, and nothing in it behaves differently per provider.
/// Anything provider-specific belongs in the client or in
/// `object_persistence_options`. The fault points are named for the same
/// reason: `raft/objstore/{put,get,delete,list}_object`, at the store boundary,
/// provider-independent because the engine is.

#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <boost/json.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

/// @brief Engine-level options. Every default preserves the shipped engine's
///        behaviour exactly, so an operator who upgrades and changes nothing
///        sees a bucket with exactly the keys it had before.
///
/// The spec's full option set (snapshot retention, fencing mode and owner id,
/// checksum verification, a single-PUT size cap) is added field by field
/// **with** the behaviour that honours it — a field that is accepted and
/// ignored is worse than an absent one, especially for a fencing knob.
struct object_persistence_options {
    /// Same-call retries, PUT-only. `1` is the shipped behaviour: one retry,
    /// then throw.
    unsigned write_retries{1};
};

/// @brief Raft persistent state stored as individual objects in a key-object
///        store.
///
/// @tparam Store    Must satisfy `kythira::key_object_store`.
/// @tparam NodeId   Node identifier type; defaults to `std::uint64_t`.
/// @tparam TermId   Term number type; defaults to `std::uint64_t`.
/// @tparam LogIndex Log index type; defaults to `std::uint64_t`.
template<key_object_store Store, typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class object_store_persistence_engine {
public:
    using store_t = Store;
    using log_entry_t = log_entry<TermId, LogIndex>;
    using snapshot_t = snapshot<NodeId, TermId, LogIndex>;

    /// Number of digits every log index is zero-padded to: `2^64 - 1` is 20
    /// digits, so this is the width at which lexicographic key order and
    /// numeric index order coincide for every representable index.
    static constexpr std::size_t k_index_digits = 20;

    /// @brief Construct over `{store, bucket, prefix}` and load the mirror.
    ///
    /// @throws std::invalid_argument if a required field is empty or an option
    ///                               is out of range.
    /// @throws std::runtime_error    if the listing or any GET fails, or if any
    ///                               object under the prefix fails to parse —
    ///                               the message names the offending key.
    ///
    /// An empty prefix is the normal cold-start case: the standard empty state
    /// (term 0, no vote, empty log, no snapshot) is established and **no object
    /// is written** until the first mutation.
    object_store_persistence_engine(Store store, std::string bucket, std::string prefix,
                                    object_persistence_options opts = {})
        : _store(std::move(store)),
          _bucket(std::move(bucket)),
          _prefix(std::move(prefix)),
          _opts(opts) {
        if (_bucket.empty()) {
            throw std::invalid_argument("object_store_persistence_engine: bucket is empty");
        }
        // A trailing slash is accepted and normalised away so that
        // `"raft"` and `"raft/"` name the same objects rather than two
        // near-identical key spaces (`raft/term` vs `raft//term`).
        while (!_prefix.empty() && _prefix.back() == '/') {
            _prefix.pop_back();
        }
        if (_prefix.empty()) {
            throw std::invalid_argument("object_store_persistence_engine: prefix is empty");
        }
        load_all();
    }

    /// Move-only, and only safe before the engine is shared with the Raft loop —
    /// the same restriction, for the same reason, as `file_persistence_engine`.
    object_store_persistence_engine(object_store_persistence_engine&& other) noexcept
        : _store(std::move(other._store)),
          _bucket(std::move(other._bucket)),
          _prefix(std::move(other._prefix)),
          _opts(other._opts),
          _current_term(other._current_term),
          _voted_for(std::move(other._voted_for)),
          _log(std::move(other._log)),
          _snapshot(std::move(other._snapshot)) {}

    auto operator=(object_store_persistence_engine&&) -> object_store_persistence_engine& = delete;
    object_store_persistence_engine(const object_store_persistence_engine&) = delete;
    auto operator=(const object_store_persistence_engine&)
        -> object_store_persistence_engine& = delete;
    ~object_store_persistence_engine() = default;

    // ── Key layout ───────────────────────────────────────────────────────────

    /// @brief The object key holding `currentTerm`.
    [[nodiscard]] auto term_key() const -> std::string { return _prefix + "/term"; }

    /// @brief The object key holding `votedFor`.
    [[nodiscard]] auto voted_for_key() const -> std::string { return _prefix + "/voted_for"; }

    /// @brief The single snapshot slot's object key.
    [[nodiscard]] auto snapshot_key() const -> std::string { return _prefix + "/snapshot"; }

    /// @brief The object key holding the entry at `index`, zero-padded to 20
    ///        digits so lexicographic listing order is numeric index order.
    ///        Public because the padding is a documented, test-pinned part of
    ///        the on-bucket format.
    [[nodiscard]] auto log_key(LogIndex index) const -> std::string {
        return log_prefix() + pad_index(index);
    }

    /// @brief The common prefix of every log-entry key, trailing slash included.
    [[nodiscard]] auto log_prefix() const -> std::string { return _prefix + "/log/"; }

    /// @brief The store this engine writes through, for tests and for tooling
    ///        that needs the provider's name.
    [[nodiscard]] auto store() const -> const Store& { return _store; }

    // ── currentTerm ──────────────────────────────────────────────────────────

    auto save_current_term(TermId term) -> void {
        std::lock_guard lock(_mu);
        put_object(term_key(), std::to_string(static_cast<unsigned long long>(term)));
        _current_term = term;
    }

    auto load_current_term() -> TermId {
        std::lock_guard lock(_mu);
        return _current_term;
    }

    // ── votedFor ─────────────────────────────────────────────────────────────

    auto save_voted_for(NodeId node) -> void {
        std::lock_guard lock(_mu);
        if constexpr (std::is_same_v<NodeId, std::string>) {
            put_object(voted_for_key(), node);
        } else {
            put_object(voted_for_key(), std::to_string(static_cast<unsigned long long>(node)));
        }
        _voted_for = std::move(node);
    }

    auto load_voted_for() -> std::optional<NodeId> {
        std::lock_guard lock(_mu);
        return _voted_for;
    }

    // ── Log ──────────────────────────────────────────────────────────────────

    auto append_log_entry(const log_entry_t& entry) -> void {
        std::lock_guard lock(_mu);
        put_object(log_key(entry.index()), entry_to_json(entry));
        _log[entry.index()] = entry;
    }

    auto get_log_entry(LogIndex index) -> std::optional<log_entry_t> {
        std::lock_guard lock(_mu);
        auto it = _log.find(index);
        if (it == _log.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto get_log_entries(LogIndex start, LogIndex end) -> std::vector<log_entry_t> {
        std::lock_guard lock(_mu);
        std::vector<log_entry_t> result;
        for (auto it = _log.lower_bound(start); it != _log.end() && it->first <= end; ++it) {
            result.push_back(it->second);
        }
        return result;
    }

    auto get_last_log_index() -> LogIndex {
        std::lock_guard lock(_mu);
        if (_log.empty()) {
            return LogIndex{0};
        }
        // `_log` is ordered, so the greatest index is the last element.
        return _log.rbegin()->first;
    }

    /// @brief Delete every entry with index ≥ `index` — a bounded batch of
    ///        DELETEs, one per affected object.
    ///
    /// Each object is erased from the mirror only after its DELETE is
    /// acknowledged, so a failure part-way through leaves the mirror equal to
    /// the store and the caller free to re-run the (idempotent) truncation.
    auto truncate_log(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        auto it = _log.lower_bound(index);
        while (it != _log.end()) {
            delete_object(log_key(it->first));
            it = _log.erase(it);
        }
    }

    /// @brief Delete every entry with index &lt; `index` (compaction after a
    ///        snapshot), with the same delete-then-forget ordering.
    auto delete_log_entries_before(LogIndex index) -> void {
        std::lock_guard lock(_mu);
        auto it = _log.begin();
        while (it != _log.end() && it->first < index) {
            delete_object(log_key(it->first));
            it = _log.erase(it);
        }
    }

    // ── Snapshot ─────────────────────────────────────────────────────────────

    /// @brief Overwrite the single `<prefix>/snapshot` object.
    auto save_snapshot(const snapshot_t& snap) -> void {
        std::lock_guard lock(_mu);
        put_object(snapshot_key(), snapshot_to_json(snap));
        _snapshot = snap;
    }

    auto load_snapshot() -> std::optional<snapshot_t> {
        std::lock_guard lock(_mu);
        return _snapshot;
    }

private:
    // ── Store boundary ───────────────────────────────────────────────────────
    //
    // Every store call is wrapped by exactly one libfiu fault point, following
    // `memory_persistence_engine`'s `raft/persistence/...` precedent but placed
    // at the store boundary so a chaos run can fail a single storage operation
    // rather than a whole persistence method. The names carry no provider:
    // a chaos configuration written against this engine works for every store
    // it is instantiated over.

    /// PUT with the single idempotent-overwrite retry.
    auto put_object(const std::string& key, std::string_view bytes) -> void {
        fiu_do_on("raft/objstore/put_object",
                  throw std::runtime_error("chaos: raft/objstore/put_object " + key););
        unsigned attempts_left = _opts.write_retries;
        std::string first_what;
        while (true) {
            try {
                _store.put_object(_bucket, key, bytes);
                return;
            } catch (const std::exception& e) {
                if (attempts_left == 0) {
                    if (first_what.empty()) {
                        throw std::runtime_error("object_store_persistence_engine: PUT " + key +
                                                 " failed: " + e.what());
                    }
                    throw std::runtime_error("object_store_persistence_engine: PUT " + key +
                                             " failed twice — first: " + first_what +
                                             "; retry: " + e.what());
                }
                // Safe to re-send: this is a full-object overwrite at a
                // deterministic key, so the retry writes the identical bytes to
                // the identical place whether or not the first attempt landed.
                if (first_what.empty()) {
                    first_what = e.what();
                }
                --attempts_left;
            }
        }
    }

    /// DELETE, no retry: only full-overwrite PUTs are retried here. These stores
    /// answer success for a key that was never there, so a caller-level re-run
    /// of the whole truncation is the idempotent recovery.
    auto delete_object(const std::string& key) -> void {
        fiu_do_on("raft/objstore/delete_object",
                  throw std::runtime_error("chaos: raft/objstore/delete_object " + key););
        _store.delete_object(_bucket, key);
    }

    auto get_object(const std::string& key) -> std::optional<std::string> {
        fiu_do_on("raft/objstore/get_object",
                  throw std::runtime_error("chaos: raft/objstore/get_object " + key););
        auto result = _store.get_object(_bucket, key);
        if (!result) {
            return std::nullopt;
        }
        return std::move(result->body);
    }

    auto list_keys(const std::string& prefix) -> std::vector<std::string> {
        fiu_do_on("raft/objstore/list_object",
                  throw std::runtime_error("chaos: raft/objstore/list_object " + prefix););
        return _store.list_keys(_bucket, prefix);
    }

    // ── Initialisation ───────────────────────────────────────────────────────

    /// One List plus one GET per live object. Anything that will not parse
    /// throws with its key named.
    auto load_all() -> void {
        const std::string term = term_key();
        const std::string vote = voted_for_key();
        const std::string snap = snapshot_key();
        const std::string log_pfx = log_prefix();

        for (const auto& key : list_keys(_prefix + "/")) {
            // A key that vanished between the List and the GET is a
            // concurrent-writer symptom (there is not supposed to be one) —
            // but it is also what a not-yet-consistent listing would look
            // like, so treat absent as absent rather than inventing state
            // for it.
            if (key == term) {
                auto body = get_object(key);
                if (body) {
                    _current_term = static_cast<TermId>(parse_unsigned(*body, key));
                }
            } else if (key == vote) {
                auto body = get_object(key);
                if (body && *body != "none" && !body->empty()) {
                    if constexpr (std::is_same_v<NodeId, std::string>) {
                        _voted_for = *body;
                    } else {
                        _voted_for = static_cast<NodeId>(parse_unsigned(*body, key));
                    }
                }
            } else if (key == snap) {
                auto body = get_object(key);
                if (body && !body->empty()) {
                    _snapshot =
                        wrap_parse<snapshot_t>(key, [&body] { return json_to_snapshot(*body); });
                }
            } else if (key.starts_with(log_pfx)) {
                const LogIndex index =
                    index_from_key(std::string_view(key).substr(log_pfx.size()), key);
                auto body = get_object(key);
                if (!body) {
                    continue;
                }
                auto entry = wrap_parse<log_entry_t>(key, [&body] { return json_to_entry(*body); });
                if (entry.index() != index) {
                    // The key *is* the index; a record that disagrees with its
                    // own key means the log's ordering guarantee no longer
                    // holds, which is exactly the silent-loss case this engine
                    // exists to prevent.
                    throw std::runtime_error(
                        "object_store_persistence_engine: corrupt object " + key +
                        ": record carries index " +
                        std::to_string(static_cast<unsigned long long>(entry.index())) +
                        " but its key names index " +
                        std::to_string(static_cast<unsigned long long>(index)));
                }
                _log[index] = std::move(entry);
            }
            // Any other key under the prefix belongs to something else (an
            // operator's note, a future format's object). It is not this
            // engine's state, so it is neither read nor written — only keys
            // this format defines are parsed, and those must parse.
        }
    }

    // ── Parsing helpers, all of which name the key they choked on ────────────

    template<typename T, typename Fn>
    static auto wrap_parse(const std::string& key, Fn&& parse) -> T {
        try {
            return parse();
        } catch (const std::exception& e) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                     ": " + e.what());
        }
    }

    /// Strict decimal parse: the *whole* body must be digits. `std::stoull`
    /// would happily read "12" out of "12garbage"; a persistence engine that
    /// does that turns a truncated upload into a plausible-looking term.
    static auto parse_unsigned(std::string_view body, const std::string& key)
        -> unsigned long long {
        // Tolerate a single trailing newline: an operator repairing state with
        // a shell writes one whether they mean to or not.
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
            body.remove_suffix(1);
        }
        unsigned long long value = 0;
        const char* first = body.data();
        const char* last = body.data() + body.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last || body.empty()) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                     ": expected a decimal number, got \"" + std::string(body) +
                                     "\"");
        }
        return value;
    }

    /// The inverse of `log_key`'s padding, and strict about it: a log key whose
    /// suffix is not exactly `k_index_digits` digits cannot be ordered against
    /// the others, so it is corruption rather than a key to skip.
    static auto index_from_key(std::string_view suffix, const std::string& key) -> LogIndex {
        if (suffix.size() != k_index_digits ||
            suffix.find_first_not_of("0123456789") != std::string_view::npos) {
            throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                     ": log key suffix must be exactly " +
                                     std::to_string(k_index_digits) + " decimal digits");
        }
        return static_cast<LogIndex>(parse_unsigned(suffix, key));
    }

    static auto pad_index(LogIndex index) -> std::string {
        std::string digits = std::to_string(static_cast<unsigned long long>(index));
        if (digits.size() >= k_index_digits) {
            return digits;
        }
        return std::string(k_index_digits - digits.size(), '0') + digits;
    }

    // ── Serialisation — byte-identical to file_persistence_engine's codec ────

    static auto entry_to_json(const log_entry_t& e) -> std::string {
        boost::json::object obj;
        obj["term"] = e.term();
        obj["index"] = e.index();
        obj["command"] = bytes_to_base64(e.command());
        obj["type"] = static_cast<int>(e.type());
        return boost::json::serialize(obj);
    }

    static auto json_to_entry(const std::string& s) -> log_entry_t {
        auto obj = boost::json::parse(s).as_object();
        log_entry_t e;
        e._term = static_cast<TermId>(obj.at("term").as_int64());
        e._index = static_cast<LogIndex>(obj.at("index").as_int64());
        e._command = base64_to_bytes(std::string(obj.at("command").as_string()));
        e._type = obj.contains("type") ? static_cast<entry_type>(obj.at("type").as_int64())
                                       : entry_type::normal;
        return e;
    }

    static auto snapshot_to_json(const snapshot_t& snap) -> std::string {
        boost::json::object obj;
        obj["last_included_index"] = snap.last_included_index();
        obj["last_included_term"] = snap.last_included_term();
        obj["state"] = bytes_to_base64(snap.state_machine_state());
        boost::json::array nodes;
        for (const auto& n : snap.configuration().nodes()) {
            if constexpr (std::is_same_v<NodeId, std::string>) {
                nodes.push_back(boost::json::string(n));
            } else {
                nodes.push_back(static_cast<std::uint64_t>(n));
            }
        }
        obj["nodes"] = nodes;
        obj["is_joint_consensus"] = snap.configuration().is_joint_consensus();
        if (snap.configuration().is_joint_consensus() && snap.configuration().old_nodes()) {
            boost::json::array old_nodes;
            for (const auto& n : *snap.configuration().old_nodes()) {
                if constexpr (std::is_same_v<NodeId, std::string>) {
                    old_nodes.push_back(boost::json::string(n));
                } else {
                    old_nodes.push_back(static_cast<std::uint64_t>(n));
                }
            }
            obj["old_nodes"] = old_nodes;
        }
        return boost::json::serialize(obj);
    }

    static auto json_to_snapshot(const std::string& s) -> snapshot_t {
        auto obj = boost::json::parse(s).as_object();
        snapshot_t snap;
        snap._last_included_index = static_cast<LogIndex>(obj.at("last_included_index").as_int64());
        snap._last_included_term = static_cast<TermId>(obj.at("last_included_term").as_int64());
        snap._state_machine_state = base64_to_bytes(std::string(obj.at("state").as_string()));
        for (const auto& n : obj.at("nodes").as_array()) {
            if constexpr (std::is_same_v<NodeId, std::string>) {
                snap._configuration._nodes.emplace_back(n.as_string());
            } else {
                snap._configuration._nodes.push_back(static_cast<NodeId>(n.as_int64()));
            }
        }
        snap._configuration._is_joint_consensus =
            obj.contains("is_joint_consensus") && obj.at("is_joint_consensus").as_bool();
        if (snap._configuration._is_joint_consensus && obj.contains("old_nodes")) {
            std::vector<NodeId> old_nodes;
            for (const auto& n : obj.at("old_nodes").as_array()) {
                if constexpr (std::is_same_v<NodeId, std::string>) {
                    old_nodes.emplace_back(n.as_string());
                } else {
                    old_nodes.push_back(static_cast<NodeId>(n.as_int64()));
                }
            }
            snap._configuration._old_nodes = std::move(old_nodes);
        }
        return snap;
    }

    // ── Base64 (same codec and alphabet as file_persistence_engine) ──────────

    static constexpr std::string_view k_b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    static auto bytes_to_base64(const std::vector<std::byte>& in) -> std::string {
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        for (std::size_t i = 0; i < in.size(); i += 3) {
            std::uint32_t v = static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << 16U;
            if (i + 1 < in.size()) {
                v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 1])) << 8U;
            }
            if (i + 2 < in.size()) {
                v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i + 2]));
            }
            out += k_b64[(v >> 18U) & 0x3FU];
            out += k_b64[(v >> 12U) & 0x3FU];
            out += (i + 1 < in.size()) ? k_b64[(v >> 6U) & 0x3FU] : '=';
            out += (i + 2 < in.size()) ? k_b64[v & 0x3FU] : '=';
        }
        return out;
    }

    /// Decodes strictly: a character outside the alphabet throws rather than
    /// being skipped the way the file engine skips it. Skipping is how a
    /// corrupted command silently decodes to *some* byte string, which the
    /// state machine would then apply.
    static auto base64_to_bytes(const std::string& in) -> std::vector<std::byte> {
        static const auto tbl = [] {
            std::array<std::int8_t, 256> t{};
            t.fill(-1);
            for (int i = 0; i < 64; ++i) {
                t[static_cast<std::uint8_t>(k_b64[static_cast<std::size_t>(i)])] =
                    static_cast<std::int8_t>(i);
            }
            return t;
        }();
        std::vector<std::byte> out;
        out.reserve(in.size() * 3 / 4);
        std::uint32_t v = 0;
        int bits = 0;
        for (const char c : in) {
            if (c == '=') {
                break;
            }
            const std::int8_t b = tbl[static_cast<std::uint8_t>(c)];
            if (b < 0) {
                throw std::runtime_error("base64: invalid character in encoded command");
            }
            v = (v << 6U) | static_cast<std::uint32_t>(b);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(bits)) & 0xFFU));
            }
        }
        return out;
    }

    Store _store;
    std::string _bucket;
    std::string _prefix;
    object_persistence_options _opts;

    mutable std::mutex _mu;
    TermId _current_term{0};
    std::optional<NodeId> _voted_for;
    /// Ordered, not hashed: `get_log_entries`, `truncate_log`,
    /// `delete_log_entries_before`, and `get_last_log_index` are all range
    /// operations, and the order is the same one the key padding gives the
    /// bucket.
    std::map<LogIndex, log_entry_t> _log;
    std::optional<snapshot_t> _snapshot;
};

namespace object_store_persistence_detail {

/// The smallest thing that satisfies `key_object_store`, so the concept
/// assertion below can be made at file scope without naming a provider. It is
/// never instantiated — only its signatures are read.
struct concept_check_store {
    auto put_object(const std::string&, const std::string&, std::string_view) const -> put_result;
    auto get_object(const std::string&, const std::string&) const -> std::optional<get_result>;
    auto delete_object(const std::string&, const std::string&) const -> void;
    auto list_keys(const std::string&, const std::string&) const -> std::vector<std::string>;
    auto provider_name() const -> std::string_view;
};

static_assert(key_object_store<concept_check_store>);

}  // namespace object_store_persistence_detail

/// The concept is checked here, at file scope, so a signature drift is a
/// compile error in every translation unit that includes this header — not just
/// in whichever test happens to instantiate it.
static_assert(
    persistence_engine<
        object_store_persistence_engine<object_store_persistence_detail::concept_check_store>,
        std::uint64_t, std::uint64_t, std::uint64_t, log_entry<>, snapshot<>>,
    "object_store_persistence_engine must satisfy the persistence_engine concept");

}  // namespace kythira
