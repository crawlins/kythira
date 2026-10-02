// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Duplicate detection for CoAP exchanges (.kiro/specs/coap-transport-multi-raft/
// Requirement 5), shared by the libcoap client and server and by the cantcoap
// backend, which each carried their own copy of the same check keyed on the
// bare 16-bit Message ID.
//
// A Message ID identifies an exchange only together with the endpoint that
// chose it: RFC 7252 Section 4.4 scopes Message ID uniqueness to one source
// endpoint, and every peer numbers its own messages independently. Keying on
// the Message ID alone therefore declared one peer's message a duplicate of a
// different peer's whenever their counters met, which in a cluster of three or
// more nodes is a matter of when, not whether; the server then answered with a
// bare 2.03 and the real request was never delivered.
//
// The key is (peer endpoint, Message ID), and a match additionally requires
// the same token. RFC 7252 forbids reusing a Message ID within
// EXCHANGE_LIFETIME, but a peer that leads many Raft groups over one client
// emits ~40 Message IDs per second per group and laps the 16-bit space well
// inside that horizon. A retransmission repeats its token; a new request on a
// wrapped counter does not. Comparing the token is what lets a wrapped counter
// through without weakening the check for a genuine retransmission.
//
// Every component of the key narrows the old check, never widens it: an entry
// is a duplicate only when peer, Message ID and token all match. The change can
// turn a wrongly-dropped message into a delivered one and never the reverse.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kythira {

/// RFC 7252 Section 4.8.2: EXCHANGE_LIFETIME = MAX_TRANSMIT_SPAN (45 s)
/// + 2 * MAX_LATENCY (100 s) + PROCESSING_DELAY (2 s) = 247 s, computed from
/// the Section 4.8 defaults. The time a sender must wait before reusing a
/// Message ID, and therefore exactly how long a receiver has to remember one to
/// recognise a retransmission of it. It replaces an arbitrary five minutes.
inline constexpr std::chrono::seconds coap_exchange_lifetime{247};

/// Remembers recently seen (peer, Message ID, token) triples for
/// coap_exchange_lifetime. Not synchronised: every owner already serialises
/// access under its own mutex.
///
/// Bounded per peer by the Message ID space rather than by traffic rate: a
/// Message ID that recurs overwrites its slot, so a peer holds at most 65,536
/// entries however fast it sends. Expired entries are swept in batches whose
/// cost is amortised over the records that triggered them, rather than by
/// walking the whole table on every record as the per-transport copies did.
class coap_exchange_table {
public:
    using clock = std::chrono::steady_clock;

    explicit coap_exchange_table(clock::duration lifetime = coap_exchange_lifetime)
        : _lifetime{lifetime} {}

    /// True when this exact exchange (peer, Message ID and token) was recorded
    /// within the lifetime: a retransmission, to be suppressed.
    [[nodiscard]] auto is_duplicate(std::string_view peer, std::uint16_t message_id,
                                    std::string_view token,
                                    clock::time_point now = clock::now()) const -> bool {
        const auto peer_it = _peers.find(peer);
        if (peer_it == _peers.end()) {
            return false;
        }
        const auto slot_it = peer_it->second.find(message_id);
        if (slot_it == peer_it->second.end()) {
            return false;
        }
        const auto& slot = slot_it->second;
        return now - slot.recorded < _lifetime && slot.token_hash == hash_token(token);
    }

    /// Records an exchange as seen, replacing whatever that peer's Message ID
    /// slot held before: on a wrapped counter, the newer exchange is the only
    /// one a retransmission can still be of.
    auto record(std::string_view peer, std::uint16_t message_id, std::string_view token,
                clock::time_point now = clock::now()) -> void {
        auto peer_it = _peers.find(peer);
        if (peer_it == _peers.end()) {
            peer_it = _peers.emplace(std::string{peer}, peer_window{}).first;
        }
        auto [slot_it, inserted] =
            peer_it->second.insert_or_assign(message_id, slot{now, hash_token(token)});
        (void)slot_it;
        if (inserted) {
            ++_entries;
        }
        if (++_records_since_sweep >= sweep_threshold()) {
            sweep(now);
        }
    }

    /// Checks and records in one step: false for a new exchange (now recorded),
    /// true for a retransmission of one already seen.
    auto check_and_record(std::string_view peer, std::uint16_t message_id, std::string_view token,
                          clock::time_point now = clock::now()) -> bool {
        if (is_duplicate(peer, message_id, token, now)) {
            return true;
        }
        record(peer, message_id, token, now);
        return false;
    }

    /// Drops every expired entry, and every peer left with none.
    auto sweep(clock::time_point now = clock::now()) -> void {
        for (auto peer_it = _peers.begin(); peer_it != _peers.end();) {
            auto& window = peer_it->second;
            for (auto slot_it = window.begin(); slot_it != window.end();) {
                if (now - slot_it->second.recorded >= _lifetime) {
                    slot_it = window.erase(slot_it);
                    --_entries;
                } else {
                    ++slot_it;
                }
            }
            peer_it = window.empty() ? _peers.erase(peer_it) : std::next(peer_it);
        }
        _records_since_sweep = 0;
    }

    auto clear() -> void {
        _peers.clear();
        _entries = 0;
        _records_since_sweep = 0;
    }

    /// Live and not-yet-swept entries across every peer.
    [[nodiscard]] auto size() const -> std::size_t { return _entries; }
    [[nodiscard]] auto peer_count() const -> std::size_t { return _peers.size(); }
    [[nodiscard]] auto lifetime() const -> clock::duration { return _lifetime; }

private:
    struct slot {
        clock::time_point recorded;
        std::size_t token_hash;
    };
    using peer_window = std::unordered_map<std::uint16_t, slot>;

    // Heterogeneous lookup, so the per-message check never allocates a
    // std::string for the peer it is looking up.
    struct string_hash {
        using is_transparent = void;
        auto operator()(std::string_view s) const -> std::size_t {
            return std::hash<std::string_view>{}(s);
        }
    };

    [[nodiscard]] static auto hash_token(std::string_view token) -> std::size_t {
        return std::hash<std::string_view>{}(token);
    }

    // A full sweep costs O(entries); running one only after that many new
    // records keeps it O(1) amortised per record. The floor stops a nearly
    // empty table from sweeping on every message.
    [[nodiscard]] auto sweep_threshold() const -> std::size_t {
        constexpr std::size_t minimum = 1024;
        return _entries > minimum ? _entries : minimum;
    }

    clock::duration _lifetime;
    std::unordered_map<std::string, peer_window, string_hash, std::equal_to<>> _peers;
    std::size_t _entries{0};
    std::size_t _records_since_sweep{0};
};

}  // namespace kythira
