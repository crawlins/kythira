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
//
// An owner that builds its own message layer can also keep the reply it sent
// for each exchange (.kiro/specs/coap-cantcoap-duplicate-replay/), so a
// retransmitted request is answered again instead of dropped: RFC 7252 Section
// 4.5 asks for each duplicate confirmable message to be acknowledged. The
// reply rides in the same slot as the record, under the same key, lifetime and
// sweep, and is bounded by its own byte budget and shorter retention. The
// default limits store nothing, so owners that never attach a reply (libcoap,
// which replays inside the library) pay nothing for it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira {

/// RFC 7252 Section 4.8.2: EXCHANGE_LIFETIME = MAX_TRANSMIT_SPAN (45 s)
/// + 2 * MAX_LATENCY (100 s) + PROCESSING_DELAY (2 s) = 247 s, computed from
/// the Section 4.8 defaults. The time a sender must wait before reusing a
/// Message ID, and therefore exactly how long a receiver has to remember one to
/// recognise a retransmission of it. It replaces an arbitrary five minutes.
inline constexpr std::chrono::seconds coap_exchange_lifetime{247};

/// RFC 7252 Section 4.8.2: MAX_TRANSMIT_WAIT = ACK_TIMEOUT * (2^(MAX_RETRANSMIT
/// + 1) - 1) * ACK_RANDOM_FACTOR = 93 s with the Section 4.8 defaults. The
/// longest a client following those defaults waits for a reply, so a stored
/// reply older than this can no longer reach anyone who is waiting for it.
inline constexpr std::chrono::seconds coap_max_transmit_wait{93};

/// How much of the replies it sent a coap_exchange_table keeps. The default
/// keeps nothing.
struct coap_reply_cache_limits {
    /// Cap on the total bytes of stored replies; the oldest are evicted first
    /// to make room. 0 never stores a reply.
    std::size_t budget_bytes{0};
    /// How long a stored reply can be replayed. Shorter than the record's own
    /// lifetime: once the reply expires, a late copy of the request is still a
    /// duplicate, and is dropped rather than processed again.
    std::chrono::steady_clock::duration retention{coap_max_transmit_wait};
};

/// What an owner that keeps replies does with an incoming message.
enum class coap_duplicate_kind {
    fresh,   ///< A new exchange, now recorded: process it.
    replay,  ///< A duplicate of an exchange whose reply is stored: send it again.
    drop,    ///< A duplicate with no reply to send (none yet, expired or evicted).
};

struct coap_duplicate_lookup {
    coap_duplicate_kind kind;
    /// The stored reply; set only for coap_duplicate_kind::replay. Shared so
    /// the caller can send it after releasing its lock, even if a concurrent
    /// attach evicts it from the table in the meantime.
    std::shared_ptr<const std::vector<std::byte>> reply;
};

/// Remembers recently seen (peer, Message ID, token) triples for
/// coap_exchange_lifetime, and optionally the reply sent for each. Not
/// synchronised: every owner already serialises access under its own mutex.
///
/// Bounded per peer by the Message ID space rather than by traffic rate: a
/// Message ID that recurs overwrites its slot, so a peer holds at most 65,536
/// entries however fast it sends. Expired entries are swept in batches whose
/// cost is amortised over the records that triggered them, rather than by
/// walking the whole table on every record as the per-transport copies did.
class coap_exchange_table {
public:
    using clock = std::chrono::steady_clock;

    explicit coap_exchange_table(clock::duration lifetime = coap_exchange_lifetime,
                                 coap_reply_cache_limits limits = {})
        : _lifetime{lifetime}, _limits{limits} {}

    /// True when this exact exchange (peer, Message ID and token) was recorded
    /// within the lifetime: a retransmission, to be suppressed.
    [[nodiscard]] auto is_duplicate(std::string_view peer, std::uint16_t message_id,
                                    std::string_view token,
                                    clock::time_point now = clock::now()) const -> bool {
        return live_slot(peer, message_id, token, now) != nullptr;
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
        auto [slot_it, inserted] = peer_it->second.try_emplace(message_id);
        if (inserted) {
            ++_entries;
        } else {
            release_reply(slot_it->second);
        }
        slot_it->second.recorded = now;
        slot_it->second.token_hash = hash_token(token);
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

    /// For owners that keep replies: classifies a message and, when it is
    /// new, records it. A duplicate is a replay when its reply is stored and
    /// still within the retention, and a drop otherwise; a reply past its
    /// retention is released here, while its record stays for the lifetime.
    auto classify(std::string_view peer, std::uint16_t message_id, std::string_view token,
                  clock::time_point now = clock::now()) -> coap_duplicate_lookup {
        expire_replies(now);
        if (auto* found = live_slot(peer, message_id, token, now)) {
            if (found->reply && now - found->replied < _limits.retention) {
                return {coap_duplicate_kind::replay, found->reply};
            }
            release_reply(*found);
            return {coap_duplicate_kind::drop, nullptr};
        }
        record(peer, message_id, token, now);
        return {coap_duplicate_kind::fresh, nullptr};
    }

    /// Stores the reply sent for a recorded exchange, replacing any stored
    /// before, and evicts the oldest stored replies until the budget holds it.
    /// A no-op when the budget is 0, the exchange is not recorded (or has
    /// expired, or its slot now holds another token), or the reply alone is
    /// larger than the whole budget.
    auto attach_reply(std::string_view peer, std::uint16_t message_id, std::string_view token,
                      std::vector<std::byte> reply, clock::time_point now = clock::now()) -> void {
        if (_limits.budget_bytes == 0 || reply.size() > _limits.budget_bytes) {
            return;
        }
        expire_replies(now);
        auto* found = live_slot(peer, message_id, token, now);
        if (found == nullptr) {
            return;
        }
        release_reply(*found);
        while (!_reply_order.empty() && _reply_bytes + reply.size() > _limits.budget_bytes) {
            if (release_oldest_reply()) {
                ++_replies_evicted;
            }
        }
        _reply_bytes += reply.size();
        ++_reply_count;
        found->reply = std::make_shared<const std::vector<std::byte>>(std::move(reply));
        found->replied = now;
        found->reply_serial = ++_next_reply_serial;
        _reply_order.push_back({std::string{peer}, message_id, now, found->reply_serial});
    }

    struct reply_stats {
        /// Stored replies evicted to make room under the budget, since
        /// construction (clear() does not reset it).
        std::uint64_t evicted;
        /// Bytes and number of replies stored right now.
        std::size_t bytes;
        std::size_t count;
    };

    [[nodiscard]] auto replies() const -> reply_stats {
        return {_replies_evicted, _reply_bytes, _reply_count};
    }

    /// Drops every expired entry, and every peer left with none.
    auto sweep(clock::time_point now = clock::now()) -> void {
        expire_replies(now);
        for (auto peer_it = _peers.begin(); peer_it != _peers.end();) {
            auto& window = peer_it->second;
            for (auto slot_it = window.begin(); slot_it != window.end();) {
                if (now - slot_it->second.recorded >= _lifetime) {
                    release_reply(slot_it->second);
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
        _reply_order.clear();
        _reply_bytes = 0;
        _reply_count = 0;
    }

    /// Live and not-yet-swept entries across every peer.
    [[nodiscard]] auto size() const -> std::size_t { return _entries; }
    [[nodiscard]] auto peer_count() const -> std::size_t { return _peers.size(); }
    [[nodiscard]] auto lifetime() const -> clock::duration { return _lifetime; }
    [[nodiscard]] auto reply_limits() const -> coap_reply_cache_limits { return _limits; }

private:
    struct slot {
        clock::time_point recorded{};
        std::size_t token_hash{0};
        /// The reply sent for this exchange, when the owner stores one.
        std::shared_ptr<const std::vector<std::byte>> reply;
        clock::time_point replied{};
        /// Which _reply_order entry owns `reply`. A wrapped Message ID can put
        /// a newer exchange's reply in this slot after an older one was
        /// queued; the serial stops that older entry from releasing it.
        std::uint64_t reply_serial{0};
    };
    using peer_window = std::unordered_map<std::uint16_t, slot>;

    /// One stored reply, in the order replies were attached: the eviction and
    /// retention queue.
    struct reply_entry {
        std::string peer;
        std::uint16_t message_id;
        clock::time_point replied;
        std::uint64_t serial;
    };

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

    /// The slot recording exactly this exchange within the lifetime, or null.
    [[nodiscard]] auto live_slot(std::string_view peer, std::uint16_t message_id,
                                 std::string_view token, clock::time_point now) const
        -> const slot* {
        const auto peer_it = _peers.find(peer);
        if (peer_it == _peers.end()) {
            return nullptr;
        }
        const auto slot_it = peer_it->second.find(message_id);
        if (slot_it == peer_it->second.end()) {
            return nullptr;
        }
        const auto& found = slot_it->second;
        if (now - found.recorded >= _lifetime || found.token_hash != hash_token(token)) {
            return nullptr;
        }
        return &found;
    }

    [[nodiscard]] auto live_slot(std::string_view peer, std::uint16_t message_id,
                                 std::string_view token, clock::time_point now) -> slot* {
        return const_cast<slot*>(std::as_const(*this).live_slot(peer, message_id, token, now));
    }

    /// Forgets a slot's reply, if it has one. Its queue entry stays behind and
    /// is discarded when it reaches the front, because the serials no longer
    /// match.
    auto release_reply(slot& target) -> void {
        if (!target.reply) {
            return;
        }
        _reply_bytes -= target.reply->size();
        --_reply_count;
        target.reply.reset();
        target.reply_serial = 0;
    }

    /// Pops the oldest queue entry and releases the reply it names, if that
    /// reply is still stored. Returns whether one was.
    auto release_oldest_reply() -> bool {
        const auto entry = std::move(_reply_order.front());
        _reply_order.pop_front();
        const auto peer_it = _peers.find(entry.peer);
        if (peer_it == _peers.end()) {
            return false;
        }
        const auto slot_it = peer_it->second.find(entry.message_id);
        if (slot_it == peer_it->second.end() || !slot_it->second.reply ||
            slot_it->second.reply_serial != entry.serial) {
            return false;
        }
        release_reply(slot_it->second);
        return true;
    }

    /// Releases every reply past the retention. The queue is in attach order,
    /// so this only ever looks at its front, and it also discards the stale
    /// entries released replies left behind: the queue never holds more than
    /// one retention's worth of attaches.
    auto expire_replies(clock::time_point now) -> void {
        while (!_reply_order.empty() && now - _reply_order.front().replied >= _limits.retention) {
            (void)release_oldest_reply();
        }
    }

    // A full sweep costs O(entries); running one only after that many new
    // records keeps it O(1) amortised per record. The floor stops a nearly
    // empty table from sweeping on every message.
    [[nodiscard]] auto sweep_threshold() const -> std::size_t {
        constexpr std::size_t minimum = 1024;
        return _entries > minimum ? _entries : minimum;
    }

    clock::duration _lifetime;
    coap_reply_cache_limits _limits;
    std::unordered_map<std::string, peer_window, string_hash, std::equal_to<>> _peers;
    std::size_t _entries{0};
    std::size_t _records_since_sweep{0};

    std::deque<reply_entry> _reply_order;
    std::size_t _reply_bytes{0};
    std::size_t _reply_count{0};
    std::uint64_t _replies_evicted{0};
    std::uint64_t _next_reply_serial{0};
};

}  // namespace kythira
