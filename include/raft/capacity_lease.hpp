// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file capacity_lease.hpp
/// @brief Single-writer control for elastic shard capacity: the
///        `capacity_lease` concept, the shipped Raft-leadership lease, and the
///        single-process lease for embedded use.
///
/// See `.kiro/specs/elastic-shard-capacity/` design §7 and Requirement 7. An
/// operator paying for machines wants exactly one party able to provision, so
/// that a partition cannot produce N copies of a fleet. The lease is how the
/// controller proves it is that party *before every step* — not once at
/// start-up.

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>

namespace kythira {

/// @brief Proof, checked per step, that this controller may act.
///
/// Two rules every implementation keeps:
///
/// 1. **`held()` is false whenever the answer is unknown** (Requirement 7.4).
///    A lease that cannot tell — an exception from its source, a probe it
///    cannot reach — is not held. Acting on "probably" is how two controllers
///    each provision a machine for the same shortfall.
/// 2. **`fencing_token()` never decreases across holders.** A successor's
///    token is strictly greater than every predecessor's, so a write stamped
///    with an old token is recognisably stale — which is how the controller
///    *detects* a second controller where the mechanism allows it
///    (Requirement 7.6): an intent in the ledger carrying a token above its
///    own means someone with a newer lease has acted.
template<typename L>
concept capacity_lease = requires(L& l) {
    { l.held() } -> std::same_as<bool>;
    { l.fencing_token() } -> std::same_as<std::uint64_t>;
};

/// @brief The shipped lease: Raft leadership of a nominated coordination group.
///
/// The mechanism this repository already trusts for exactly this decision —
/// the single-group quorum loop starts on `become_leader` and stops on any
/// transition away (`raft.hpp`'s `run_quorum_assessment` gating). The
/// discipline is the same here: held while, and only while, the group's local
/// replica reports itself leader; the fencing token is the term it leads in,
/// which Raft makes strictly increasing across leaders.
///
/// Built from two hooks rather than a node type, so this header names no
/// consensus type and the same lease serves a standalone `node<Types>` or one
/// group of a `multi_raft` host — see `make_node_leadership_lease` and
/// `make_group_leadership_lease`.
class raft_leadership_lease {
public:
    raft_leadership_lease(std::function<bool()> is_leader, std::function<std::uint64_t()> term)
        : _is_leader(std::move(is_leader)), _term(std::move(term)) {}

    // The atomics delete the implicit copy and move; a lease is a value the
    // controller owns, so carry the observed terms across explicitly.
    raft_leadership_lease(const raft_leadership_lease& other)
        : _is_leader(other._is_leader),
          _term(other._term),
          _highest(other._highest.load()),
          _token(other._token.load()) {}
    raft_leadership_lease(raft_leadership_lease&& other) noexcept
        : _is_leader(std::move(other._is_leader)),
          _term(std::move(other._term)),
          _highest(other._highest.load()),
          _token(other._token.load()) {}
    auto operator=(const raft_leadership_lease& other) -> raft_leadership_lease& {
        if (this != &other) {
            _is_leader = other._is_leader;
            _term = other._term;
            _highest.store(other._highest.load());
            _token.store(other._token.load());
        }
        return *this;
    }
    auto operator=(raft_leadership_lease&& other) noexcept -> raft_leadership_lease& {
        if (this != &other) {
            _is_leader = std::move(other._is_leader);
            _term = std::move(other._term);
            _highest.store(other._highest.load());
            _token.store(other._token.load());
        }
        return *this;
    }
    ~raft_leadership_lease() = default;

    /// @brief Leader right now, in a term we could read.
    ///
    /// A throw from either hook is "unknown", which is "not held". So is a
    /// term that went *backwards* — impossible for a correct Raft node, and
    /// precisely the kind of impossible thing not to act through.
    [[nodiscard]] auto held() -> bool {
        try {
            if (!_is_leader || !_term || !_is_leader()) {
                return false;
            }
            const auto term = _term();
            auto seen = _highest.load();
            if (term < seen) {
                return false;
            }
            while (term > seen && !_highest.compare_exchange_weak(seen, term)) {
            }
            _token.store(term);
            return true;
        } catch (...) {
            return false;
        }
    }

    /// @brief The term this lease was last held in.
    [[nodiscard]] auto fencing_token() -> std::uint64_t { return _token.load(); }

private:
    std::function<bool()> _is_leader;
    std::function<std::uint64_t()> _term;
    std::atomic<std::uint64_t> _highest{0};
    std::atomic<std::uint64_t> _token{0};
};

/// @brief A `raft_leadership_lease` over a standalone `node<Types>`.
///
/// `node` must outlive the lease.
template<typename Node>
[[nodiscard]] auto make_node_leadership_lease(Node& node) -> raft_leadership_lease {
    return raft_leadership_lease{
        [&node] { return node.is_leader(); },
        [&node] { return static_cast<std::uint64_t>(node.get_current_term()); }};
}

/// @brief A `raft_leadership_lease` over one group of a `multi_raft` host.
///
/// Looked up on every call: the group's node can be destroyed and re-created
/// (a merge, a tombstone, a lazily created replica), and a lease that cached
/// the pointer would keep answering for a node that no longer exists. A group
/// that is not local is "not leader".
template<typename Host, typename GroupId>
[[nodiscard]] auto make_group_leadership_lease(Host& host, GroupId group) -> raft_leadership_lease {
    return raft_leadership_lease{[&host, group] {
                                     auto* n = host.group_node(group);
                                     return n != nullptr && n->is_leader();
                                 },
                                 [&host, group] {
                                     auto* n = host.group_node(group);
                                     if (n == nullptr) {
                                         throw std::runtime_error("coordination group not local");
                                     }
                                     return static_cast<std::uint64_t>(n->get_current_term());
                                 }};
}

/// @brief The lease for an embedded deployment with exactly one controller.
///
/// **It checks nothing, and says so.** It exists for a process that is, by
/// construction, the only thing that could provision — an embedded system, a
/// test, a single-binary appliance — and it documents that assumption instead
/// of pretending to verify it. Two processes each holding one of these against
/// the same cluster is an unsupported misconfiguration this lease cannot
/// detect (Requirement 7.6); use `raft_leadership_lease` wherever two
/// controllers are possible.
///
/// The fencing token is fixed at construction: pass a larger one to a
/// replacement so ledgers written by its predecessor read as older.
class single_process_capacity_lease {
public:
    explicit single_process_capacity_lease(std::uint64_t fencing_token = 1)
        : _token(fencing_token) {}

    [[nodiscard]] auto held() -> bool { return true; }
    [[nodiscard]] auto fencing_token() -> std::uint64_t { return _token; }

private:
    std::uint64_t _token;
};

static_assert(capacity_lease<raft_leadership_lease>);
static_assert(capacity_lease<single_process_capacity_lease>);

}  // namespace kythira
