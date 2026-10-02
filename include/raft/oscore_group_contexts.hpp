// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One OSCORE Security Context per (peer, Raft group)
// (.kiro/specs/coap-transport-multi-raft/ Requirement 4, design §4).
//
// Why per group. A single context per peer carries every group's traffic
// through one 64-entry replay window (RFC 8613 Section 7.4). Groups are
// pipelined and reordered independently, so at multi-shard rates a Partial IV
// 64 places behind the highest seen is ordinary, and legitimate traffic fails
// verification. The window must not be widened (Requirement 4.12), and a
// Sender Sequence Number per group on the shared context is catastrophic: the
// nonce is a function of the Common IV, the Sender ID and the Partial IV, so
// two groups issuing Partial IV 1 under one Common IV reuse an AES-CCM nonce.
// A per-group *context* gives each group its own keys, Common IV, counter and
// window, which is the only form of per-group counter that is safe.
//
// How. RFC 8613 Section 3.2's key schedule already mixes an ID Context into
// the Sender Key, the Recipient Key and the Common IV; security_context
// derives with oscore_credentials::id_context. This registry sets it to
//
//     id_context = group_id (8 bytes, big-endian) || boot_nonce
//
// from the one master secret already established with each peer, so N groups
// cost N HKDF derivations and exactly one bootstrap (Requirement 4.4), not N
// EDHOC handshakes.
//
// Group ids are never reused. The placement driver allocates shard ids from a
// range it never reissues (shard_placement_driver.hpp), and this design
// depends on it: a recycled group id under a surviving boot nonce would
// re-derive a key already used, with a counter that has restarted
// (Requirement 4.10).

#include <raft/coap_security.hpp>
#include <raft/oscore.hpp>

#include <openssl/rand.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::oscore {

/// Bytes of process-incarnation randomness in every group ID Context.
///
/// This is the load-bearing half of the ID Context, and the constant that looks
/// most like it could be dropped. The Sender Sequence Number lives in memory
/// and restarts at zero, so a node that crashes and comes back would issue
/// Partial IV 0, 1, 2... again under keys it has already used: nonce reuse
/// across an incarnation boundary. RFC 8613 Section 7.2.1 permits exactly two
/// remedies, persisting the SSN or establishing a new Security Context.
/// Persisting one counter per group per peer, durably, on the path of every
/// heartbeat is not viable at Raft rates; a fresh boot nonce makes every
/// restart a new context for one HKDF per group per peer, at first use.
///
/// The shard epoch is NOT a substitute (Requirement 4.6): it changes on split
/// and merge, and a restart, the case that reuses nonces, leaves it untouched.
inline constexpr std::size_t boot_nonce_length = 8;

/// Fresh boot-nonce bytes from the OpenSSL CSPRNG. Exposed so tests can
/// simulate a restart; production code uses process_boot_nonce().
[[nodiscard]] inline auto generate_boot_nonce() -> std::vector<std::byte> {
    std::vector<std::byte> nonce(boot_nonce_length);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()),
                   static_cast<int>(nonce.size())) != 1) {
        throw coap_security_error("OSCORE: no randomness available for the boot nonce");
    }
    return nonce;
}

/// This process's boot nonce: generated once, on first use, and constant until
/// exit.
[[nodiscard]] inline auto process_boot_nonce() -> const std::vector<std::byte>& {
    static const std::vector<std::byte> nonce = generate_boot_nonce();
    return nonce;
}

/// `group_id (8 bytes, big-endian) || boot_nonce`.
[[nodiscard]] inline auto make_group_id_context(std::uint64_t group_id,
                                                std::span<const std::byte> boot_nonce)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(8 + boot_nonce.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((group_id >> shift) & 0xFFU));
    }
    out.insert(out.end(), boot_nonce.begin(), boot_nonce.end());
    return out;
}

/// The group an ID Context names, or nullopt when it is not one this scheme
/// produced (too short to hold a group id and a full boot nonce).
[[nodiscard]] inline auto group_of_id_context(std::span<const std::byte> id_context)
    -> std::optional<std::uint64_t> {
    if (id_context.size() < 8 + boot_nonce_length) {
        return std::nullopt;
    }
    std::uint64_t group = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        group = (group << 8U) | static_cast<std::uint64_t>(id_context[i]);
    }
    return group;
}

/// Bounds on recipient-side, on-demand derivation (Requirement 4.8).
struct group_context_limits {
    /// Live recipient contexts per peer. An adversarial bound, not a capacity
    /// one: at about a hundred bytes of key and window state per context, a
    /// thousand groups against a handful of peers is a few hundred kilobytes.
    std::size_t max_recipient_contexts_per_peer{4096};
    /// A recipient context unused for this long is dropped on the next sweep.
    /// Long enough that a peer restarting under a new boot nonce does not lose
    /// its in-flight messages under the old one the moment the new appears.
    std::chrono::seconds idle_ttl{std::chrono::minutes{10}};
};

/// Cumulative counters. Every on-demand derivation is counted, so an attacker
/// forcing HKDF work with invented `kid context` values is visible, not merely
/// expensive (Requirement 4.8.3).
struct group_context_counters {
    std::uint64_t bootstraps{0};             ///< base credentials fetched, one per peer
    std::uint64_t sender_derivations{0};     ///< contexts for our own requests
    std::uint64_t recipient_derivations{0};  ///< on-demand, from a peer's kid context
    std::uint64_t rejected_unhosted{0};      ///< kid context naming a group not hosted here
    std::uint64_t rejected_malformed{0};     ///< kid context absent or not group || nonce
    std::uint64_t evictions{0};              ///< recipient contexts dropped at the cap
    std::uint64_t expirations{0};            ///< recipient contexts dropped by idle_ttl
    std::uint64_t groups_forgotten{0};       ///< forget_group() calls that wiped something
};

/// Per-(peer, group) OSCORE Security Contexts over one master secret per peer.
///
/// Two maps, because the two directions use different ID Contexts. Requests
/// this node sends to a peer in a group derive with *this* process's boot
/// nonce; requests a peer sends derive with *its* boot nonce, which this node
/// learns only from the `kid context` of the first such request. A context is
/// therefore keyed by (peer, ID Context) and serves one request direction plus
/// the responses to it.
///
/// Responses reuse the request's nonce (security_context::protect_response's
/// default), so a recipient context never issues Sender Sequence Numbers of its
/// own under a peer's boot nonce. Observe, which needs its own Partial IVs, is
/// not used for Raft traffic.
///
/// Thread-safe.
class group_context_registry {
public:
    using clock = std::chrono::steady_clock;
    using context_ptr = std::shared_ptr<security_context>;
    /// Returns the single set of credentials established with a peer:
    /// statically provisioned, or the output of one EDHOC run. Called at most
    /// once per peer, however many groups are in use.
    using bootstrap_fn = std::function<oscore_credentials(const std::string& peer)>;
    using hosts_group_fn = std::function<bool(std::uint64_t group_id)>;

    group_context_registry(bootstrap_fn bootstrap, hosts_group_fn hosts_group,
                           group_context_limits limits = {},
                           std::vector<std::byte> boot_nonce = process_boot_nonce())
        : _bootstrap{std::move(bootstrap)},
          _hosts_group{std::move(hosts_group)},
          _limits{limits},
          _boot_nonce{std::move(boot_nonce)} {
        if (!_bootstrap || !_hosts_group) {
            throw coap_security_config_error(
                "OSCORE group contexts need a bootstrap and a hosts_group function");
        }
        if (_boot_nonce.size() < boot_nonce_length) {
            throw coap_security_config_error("OSCORE: the boot nonce must be at least 8 bytes");
        }
        if (_limits.max_recipient_contexts_per_peer == 0) {
            throw coap_security_config_error(
                "OSCORE: max_recipient_contexts_per_peer must be at least 1");
        }
    }

    group_context_registry(const group_context_registry&) = delete;
    auto operator=(const group_context_registry&) -> group_context_registry& = delete;

    ~group_context_registry() {
        const std::lock_guard lock(_mutex);
        for (auto& [peer, state] : _peers) {
            wipe_all(state);
        }
    }

    /// The context for requests this node sends to `peer` in `group_id`,
    /// derived on first use. Split creates a group, and therefore its context,
    /// lazily here at its first message (Requirement 4.9).
    [[nodiscard]] auto sender_context(const std::string& peer, std::uint64_t group_id)
        -> context_ptr {
        const std::lock_guard lock(_mutex);
        auto& state = peer_state(peer);
        const auto found = state.senders.find(group_id);
        if (found != state.senders.end()) {
            return found->second;
        }
        auto creds = *state.base;
        creds.id_context = make_group_id_context(group_id, _boot_nonce);
        auto context = std::make_shared<security_context>(creds);
        ++_counters.sender_derivations;
        state.senders.emplace(group_id, context);
        return context;
    }

    /// The context that verifies `request` from `peer`, selected by the pair
    /// (`kid`, `kid context`) and derived on first sight of a new `kid
    /// context` (Requirement 4.8). Throws verification_error, deriving
    /// nothing, when the `kid context` is absent, malformed, or names a group
    /// this node does not host.
    [[nodiscard]] auto recipient_context(const std::string& peer, const coap_message& request,
                                         clock::time_point now = clock::now()) -> context_ptr {
        // An absent kid context decodes as empty, which the overload below
        // counts and refuses as malformed.
        const auto fields = option_of(request);
        return recipient_context(peer, fields.kid, fields.kid_context, now);
    }

    /// As above, from the option fields directly.
    [[nodiscard]] auto recipient_context(const std::string& peer, std::span<const std::byte> kid,
                                         std::span<const std::byte> kid_context,
                                         clock::time_point now = clock::now()) -> context_ptr {
        const std::lock_guard lock(_mutex);
        const auto group = group_of_id_context(kid_context);
        if (!group) {
            ++_counters.rejected_malformed;
            throw verification_error("kid context is not a group ID Context");
        }
        // Checked before the map is even consulted, so an unhosted group can
        // never be served from a stale entry either.
        if (!_hosts_group(*group)) {
            ++_counters.rejected_unhosted;
            throw verification_error("kid context names a group this node does not host");
        }
        auto& state = peer_state(peer);
        if (std::vector<std::byte>(kid.begin(), kid.end()) != state.base->recipient_id) {
            ++_counters.rejected_malformed;
            throw verification_error("no Recipient Context matches the request's kid");
        }
        const std::vector<std::byte> key(kid_context.begin(), kid_context.end());
        expire(state, now);

        if (const auto found = state.recipients.find(key); found != state.recipients.end()) {
            // Most recently used goes to the back of the LRU list.
            state.lru.splice(state.lru.end(), state.lru, found->second.lru_position);
            found->second.last_used = now;
            return found->second.context;
        }

        if (state.recipients.size() >= _limits.max_recipient_contexts_per_peer) {
            const auto& oldest = state.lru.front();
            auto victim = state.recipients.find(oldest);
            victim->second.context->wipe();
            state.recipients.erase(victim);
            state.lru.pop_front();
            ++_counters.evictions;
        }

        auto creds = *state.base;
        creds.id_context = key;
        auto context = std::make_shared<security_context>(creds);
        ++_counters.recipient_derivations;
        state.lru.push_back(key);
        state.recipients.emplace(key, recipient_entry{context, now, std::prev(state.lru.end())});
        return context;
    }

    /// Destroys every context of `group_id`, both directions and every peer,
    /// zeroing their key material (Requirement 4.9). Called when the group's
    /// local replica is destroyed: merged away, tombstoned, or shut down.
    /// Anyone still holding one of the contexts finds it unusable.
    auto forget_group(std::uint64_t group_id) -> void {
        const std::lock_guard lock(_mutex);
        bool wiped = false;
        for (auto& [peer, state] : _peers) {
            if (const auto found = state.senders.find(group_id); found != state.senders.end()) {
                found->second->wipe();
                state.senders.erase(found);
                wiped = true;
            }
            for (auto it = state.recipients.begin(); it != state.recipients.end();) {
                if (group_of_id_context(it->first) == group_id) {
                    it->second.context->wipe();
                    state.lru.erase(it->second.lru_position);
                    it = state.recipients.erase(it);
                    wiped = true;
                } else {
                    ++it;
                }
            }
        }
        if (wiped) {
            ++_counters.groups_forgotten;
        }
    }

    /// Drops recipient contexts idle past `idle_ttl`. Also done lazily on every
    /// recipient lookup for that peer.
    auto sweep(clock::time_point now = clock::now()) -> void {
        const std::lock_guard lock(_mutex);
        for (auto& [peer, state] : _peers) {
            expire(state, now);
        }
    }

    [[nodiscard]] auto counters() const -> group_context_counters {
        const std::lock_guard lock(_mutex);
        return _counters;
    }

    [[nodiscard]] auto recipient_count(const std::string& peer) const -> std::size_t {
        const std::lock_guard lock(_mutex);
        const auto found = _peers.find(peer);
        return found == _peers.end() ? 0 : found->second.recipients.size();
    }

    [[nodiscard]] auto sender_count(const std::string& peer) const -> std::size_t {
        const std::lock_guard lock(_mutex);
        const auto found = _peers.find(peer);
        return found == _peers.end() ? 0 : found->second.senders.size();
    }

    [[nodiscard]] auto boot_nonce() const -> const std::vector<std::byte>& { return _boot_nonce; }

private:
    struct recipient_entry {
        context_ptr context;
        clock::time_point last_used;
        std::list<std::vector<std::byte>>::iterator lru_position;
    };

    struct peer_entry {
        std::optional<oscore_credentials> base;
        std::unordered_map<std::uint64_t, context_ptr> senders;
        std::map<std::vector<std::byte>, recipient_entry> recipients;
        /// Recipient keys, least recently used first.
        std::list<std::vector<std::byte>> lru;
    };

    [[nodiscard]] static auto option_of(const coap_message& message) -> option_fields {
        for (const auto& option : message.options) {
            if (option.number == coap_option_oscore) {
                return decode_option(option.value);
            }
        }
        throw verification_error("message carries no OSCORE option");
    }

    /// The peer's state, bootstrapping it on first use. Caller holds _mutex.
    auto peer_state(const std::string& peer) -> peer_entry& {
        auto& state = _peers[peer];
        if (!state.base) {
            auto base = _bootstrap(peer);
            if (!base.id_context.empty()) {
                throw coap_security_config_error(
                    "OSCORE: bootstrap credentials must leave id_context empty; the group "
                    "registry sets it");
            }
            state.base = std::move(base);
            ++_counters.bootstraps;
        }
        return state;
    }

    auto expire(peer_entry& state, clock::time_point now) -> void {
        while (!state.lru.empty()) {
            auto oldest = state.recipients.find(state.lru.front());
            if (now - oldest->second.last_used < _limits.idle_ttl) {
                // The LRU list is ordered by last use, so nothing behind the
                // front is older.
                return;
            }
            oldest->second.context->wipe();
            state.recipients.erase(oldest);
            state.lru.pop_front();
            ++_counters.expirations;
        }
    }

    static auto wipe_all(peer_entry& state) -> void {
        for (auto& [group, context] : state.senders) {
            context->wipe();
        }
        for (auto& [key, entry] : state.recipients) {
            entry.context->wipe();
        }
        if (state.base) {
            secure_zero(state.base->master_secret);
            secure_zero(state.base->master_salt);
        }
    }

    bootstrap_fn _bootstrap;
    hosts_group_fn _hosts_group;
    group_context_limits _limits;
    std::vector<std::byte> _boot_nonce;

    mutable std::mutex _mutex;
    std::unordered_map<std::string, peer_entry> _peers;
    group_context_counters _counters;
};

}  // namespace kythira::oscore
