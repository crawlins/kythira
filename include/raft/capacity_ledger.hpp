// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file capacity_ledger.hpp
/// @brief The provisioning ledger of elastic shard capacity: the intent record,
///        its state machine, the `capacity_ledger` concept, and three
///        implementations — volatile, file-backed, and a replicated state
///        machine for a Raft coordination group.
///
/// See `.kiro/specs/elastic-shard-capacity/` design §4 and §7. The ledger is
/// the thing that makes a provider call idempotent across a controller
/// failover: every intent is written down *before* the call that enacts it,
/// with an idempotency key the provider can carry, so a successor that finds a
/// machine can tell which decision created it — and a successor that finds a
/// `requested` intent with no machine knows a call may or may not have gone
/// out, and resolves it by reconciliation rather than by guessing.
///
/// ### Writes are proposals
///
/// Every mutation returns a **ticket**, and nothing acts on a mutation until
/// its ticket reads `committed`. For the volatile and file ledgers that is
/// immediate. For the replicated ledger it is a Raft commit on the coordination
/// group, which the controller must never wait for on the heartbeat path — the
/// group may be ticked by the very thread that is asking. So the controller
/// polls tickets the way it polls provider futures: once per step, never
/// blocking.
///
/// ### Record before act, by API shape
///
/// A provider call needs a `capacity_intent_token`, and the only way to obtain
/// one is `token(ticket)` on a *committed* `record` ticket. A token cannot be
/// constructed anywhere else, so code that calls `provision_node` without
/// having durably recorded the intent does not compile, rather than merely
/// being wrong.

#include <raft/capacity_policy.hpp>

#include <boost/json.hpp>

#include <cstring>
#include <memory>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// The intent and its states
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One machine added, or one removed.
enum class capacity_intent_kind : std::uint8_t {
    scale_out = 0,
    scale_in = 1,
};

[[nodiscard]] inline auto to_string(capacity_intent_kind k) -> const char* {
    return k == capacity_intent_kind::scale_out ? "scale_out" : "scale_in";
}

/// @brief The intent state machine of design §4.
///
/// Scale-out: `requested → provisioning → provisioned → admitting → completed`,
/// with `reaping → orphaned` for a machine that never joined. Scale-in:
/// `draining → drained → decommissioning → completed`, with `abandoned` for a
/// drain that ran out of time. `decided` and `refused` exist for the log
/// record: a decision is `decided` in memory and either `refused` by a bound
/// (never written to the ledger) or recorded straight into `requested` /
/// `draining`.
enum class capacity_intent_state : std::uint8_t {
    decided = 0,
    requested = 1,     ///< Recorded; the provider call may or may not have gone out.
    provisioning = 2,  ///< `provision_node` is in flight.
    provisioned = 3,   ///< The provider returned a machine; it has not joined.
    admitting = 4,     ///< Joined (heartbeating); shards are moving onto it.
    reaping = 5,       ///< Never joined in time; `decommission_node` is in flight.
    draining = 6,      ///< Scale-in: leadership and replicas moving off.
    drained = 7,       ///< Scale-in: holds nothing.
    decommissioning = 8,
    // ── terminal ─────────────────────────────────────────────────────────────
    completed = 9,
    refused = 10,    ///< A bound or the kill switch said no. Log-only.
    abandoned = 11,  ///< Stopped on purpose: lease lost before the call, or a deadline.
    orphaned = 12,   ///< Created, never joined, reaped.
    failed = 13,     ///< The provider said no, or the machine cannot be found.
};

[[nodiscard]] inline auto to_string(capacity_intent_state s) -> const char* {
    switch (s) {
        case capacity_intent_state::decided:
            return "decided";
        case capacity_intent_state::requested:
            return "requested";
        case capacity_intent_state::provisioning:
            return "provisioning";
        case capacity_intent_state::provisioned:
            return "provisioned";
        case capacity_intent_state::admitting:
            return "admitting";
        case capacity_intent_state::reaping:
            return "reaping";
        case capacity_intent_state::draining:
            return "draining";
        case capacity_intent_state::drained:
            return "drained";
        case capacity_intent_state::decommissioning:
            return "decommissioning";
        case capacity_intent_state::completed:
            return "completed";
        case capacity_intent_state::refused:
            return "refused";
        case capacity_intent_state::abandoned:
            return "abandoned";
        case capacity_intent_state::orphaned:
            return "orphaned";
        case capacity_intent_state::failed:
            return "failed";
        default:
            return "unknown";
    }
}

[[nodiscard]] constexpr auto is_terminal(capacity_intent_state s) -> bool {
    return s >= capacity_intent_state::completed;
}

/// @brief The transition table, explicit (task 5).
///
/// A transition not listed here is refused by every ledger — not just by the
/// controller — so a bug in the controller cannot write a state the recovery
/// logic was never designed to read.
[[nodiscard]] constexpr auto capacity_intent_transition_allowed(capacity_intent_kind kind,
                                                                capacity_intent_state from,
                                                                capacity_intent_state to) -> bool {
    using S = capacity_intent_state;
    if (is_terminal(from)) {
        return false;
    }
    if (kind == capacity_intent_kind::scale_out) {
        switch (from) {
            case S::decided:
                return to == S::requested || to == S::refused;
            case S::requested:
                // The call goes out; or the lease was lost before it did; or
                // reconciliation finds the machine joined, created-but-not-
                // joined, or nowhere.
                return to == S::provisioning || to == S::abandoned || to == S::admitting ||
                       to == S::completed || to == S::reaping || to == S::failed;
            case S::provisioning:
                return to == S::provisioned || to == S::admitting || to == S::reaping ||
                       to == S::failed || to == S::completed;
            case S::provisioned:
                return to == S::admitting || to == S::reaping || to == S::completed;
            case S::admitting:
                return to == S::completed || to == S::abandoned;
            case S::reaping:
                return to == S::orphaned || to == S::failed;
            default:
                return false;
        }
    }
    switch (from) {
        case S::decided:
            return to == S::draining || to == S::refused;
        case S::draining:
            return to == S::drained || to == S::abandoned;
        case S::drained:
            return to == S::decommissioning || to == S::abandoned;
        case S::decommissioning:
            return to == S::completed || to == S::failed;
        default:
            return false;
    }
}

/// @brief Where every non-terminal state goes when its deadline expires.
///
/// Total over non-terminal states: nothing waits forever (design §4 rule 3).
/// - `requested`: the call never started (the executor never ran it) →
///   `abandoned`. A call that *did* start is in `provisioning`.
/// - `provisioning`: `provision_deadline` passed with no answer; a machine may
///   exist → `reaping`, so it is decommissioned rather than billed forever.
/// - `provisioned`: `join_deadline` passed and it never heartbeated → `reaping`.
/// - `admitting`: `admit_deadline` → `abandoned`. The machine stays, as
///   capacity; only the moves stop.
/// - `reaping`: decommission keeps failing → `failed`, counted, for a human.
/// - `draining` / `drained`: `drain_deadline` → `abandoned`; the machine is
///   returned to service and never decommissioned half-drained.
/// - `decommissioning`: → `failed`. The machine is empty, so this costs money,
///   not data.
[[nodiscard]] constexpr auto capacity_intent_on_deadline(capacity_intent_state s)
    -> capacity_intent_state {
    using S = capacity_intent_state;
    switch (s) {
        case S::decided:
            return S::refused;
        case S::requested:
            return S::abandoned;
        case S::provisioning:
        case S::provisioned:
            return S::reaping;
        case S::admitting:
            return S::abandoned;
        case S::reaping:
            return S::failed;
        case S::draining:
        case S::drained:
            return S::abandoned;
        case S::decommissioning:
            return S::failed;
        default:
            return s;
    }
}

/// @brief A placement group the controller tried, and what the provider said.
///
/// Requirement 9.3: "we grew in the wrong AZ" is a question an operator will
/// ask later, and the answer is this list.
template<typename PlacementGroupId> struct capacity_placement_attempt {
    PlacementGroupId _group{};
    std::string _outcome;

    [[nodiscard]] auto operator==(const capacity_placement_attempt&) const -> bool = default;
};

/// @brief One recorded decision to add or remove one machine.
///
/// Carries everything Requirement 8.1 asks for — the idempotency key, the
/// decision and its reason and evidence, the target placement group, the
/// deadline, and the observed cluster size that justified it — so that a
/// successor controller, or a person, can read the decision without the
/// process that made it.
///
/// Times are `system_clock`: a ledger outlives the process, and a steady
/// clock's epoch does not.
template<typename NodeId, typename PlacementGroupId> struct capacity_intent {
    using time_point = std::chrono::system_clock::time_point;

    /// Unique per intent; also the provider metadata value used to match a
    /// machine to the intent that created it.
    std::string _key;
    capacity_intent_kind _kind{capacity_intent_kind::scale_out};
    capacity_intent_state _state{capacity_intent_state::decided};
    capacity_reason _reason{capacity_reason::manual};
    capacity_evidence _evidence{};
    PlacementGroupId _group{};
    /// The machine: the one the provider returned (scale-out) or the one being
    /// drained (scale-in). Unset until known.
    std::optional<NodeId> _node{};
    /// Its address as the provider reported it, rendered as text.
    std::string _address;
    /// The lease's fencing token when the intent was recorded.
    std::uint64_t _fencing_token{0};
    /// Cluster size when decided.
    std::size_t _observed_cluster_size{0};
    time_point _created_at{};
    time_point _updated_at{};
    /// When the current state expires; see `capacity_intent_on_deadline`.
    time_point _deadline{};
    std::vector<capacity_placement_attempt<PlacementGroupId>> _attempts;
    /// The last thing that happened to it, in words.
    std::string _note;

    [[nodiscard]] auto terminal() const -> bool { return is_terminal(_state); }
    [[nodiscard]] auto operator==(const capacity_intent&) const -> bool = default;
};

/// @brief The fields a transition may change, besides the state itself.
template<typename NodeId, typename PlacementGroupId> struct capacity_intent_patch {
    std::optional<NodeId> _node{};
    std::optional<std::string> _address{};
    std::optional<PlacementGroupId> _group{};
    std::optional<std::chrono::system_clock::time_point> _deadline{};
    std::optional<std::string> _note{};
    std::optional<capacity_placement_attempt<PlacementGroupId>> _attempt{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Tickets and tokens
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A handle on one proposed ledger write.
struct capacity_ledger_ticket {
    std::uint64_t _id{0};
    [[nodiscard]] auto operator==(const capacity_ledger_ticket&) const -> bool = default;
};

enum class capacity_ledger_write_status : std::uint8_t {
    pending = 0,    ///< Proposed, not yet durable. Do not act on it.
    committed = 1,  ///< Durable. Act on it.
    rejected = 2,   ///< Refused (illegal transition, duplicate key, lost proposal).
};

template<typename NodeId, typename PlacementGroupId> class capacity_ledger_core;

/// @brief Proof that an intent is durably recorded. Required by every
///        provider call the controller makes.
///
/// Not constructible outside a ledger: see the file comment.
class capacity_intent_token {
public:
    [[nodiscard]] auto key() const -> const std::string& { return _key; }
    [[nodiscard]] auto fencing_token() const -> std::uint64_t { return _fencing; }

private:
    template<typename, typename> friend class capacity_ledger_core;
    capacity_intent_token(std::string key, std::uint64_t fencing)
        : _key(std::move(key)), _fencing(fencing) {}
    std::string _key;
    std::uint64_t _fencing{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// The concept
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A durable record of provisioning intents.
///
/// Small on purpose (Requirement 8.7): a deployment that already has a durable
/// store can implement seven methods rather than adopting a new one.
///
/// - `record(intent)` proposes a new intent. It must be new (unique key) and in
///   its first recorded state (`requested` for scale-out, `draining` for
///   scale-in).
/// - `transition(key, to, patch, at)` proposes one state change, which the
///   ledger refuses unless `capacity_intent_transition_allowed` permits it.
/// - `compact(before)` proposes dropping terminal intents last updated before
///   `before` (Requirement 8.8).
/// - `status(ticket)` / `token(ticket)` read a proposal's fate; `token` is set
///   only for a committed `record`. `forget(ticket)` releases it.
/// - `intents()` / `find(key)` read the **committed** state only.
template<typename L, typename NodeId, typename PlacementGroupId>
concept capacity_ledger =
    requires(L& l, const L& cl, const capacity_intent<NodeId, PlacementGroupId>& intent,
             const std::string& key, capacity_intent_state to,
             const capacity_intent_patch<NodeId, PlacementGroupId>& patch,
             std::chrono::system_clock::time_point at, capacity_ledger_ticket t) {
        { l.record(intent) } -> std::same_as<capacity_ledger_ticket>;
        { l.transition(key, to, patch, at) } -> std::same_as<capacity_ledger_ticket>;
        { l.compact(at) } -> std::same_as<capacity_ledger_ticket>;
        { l.status(t) } -> std::same_as<capacity_ledger_write_status>;
        { l.token(t) } -> std::same_as<std::optional<capacity_intent_token>>;
        { l.forget(t) } -> std::same_as<void>;
        { cl.intents() } -> std::same_as<std::vector<capacity_intent<NodeId, PlacementGroupId>>>;
        { cl.find(key) } -> std::same_as<std::optional<capacity_intent<NodeId, PlacementGroupId>>>;
    };

// ─────────────────────────────────────────────────────────────────────────────
// Encoding
// ─────────────────────────────────────────────────────────────────────────────

/// @brief How a node or placement-group id is written into a ledger.
///
/// Integral types and anything convertible to and from `std::string` work out
/// of the box. Anything else specialises this.
template<typename T> struct capacity_ledger_codec {
    static auto encode(const T& v) -> boost::json::value {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            return boost::json::value(static_cast<std::int64_t>(v));
        } else if constexpr (std::is_integral_v<T>) {
            return boost::json::value(static_cast<std::uint64_t>(v));
        } else if constexpr (std::is_convertible_v<T, std::string>) {
            return boost::json::value(std::string(v));
        } else {
            static_assert(sizeof(T) == 0,
                          "specialise kythira::capacity_ledger_codec<T> for this id type");
        }
    }
    static auto decode(const boost::json::value& v) -> T {
        if constexpr (std::is_integral_v<T>) {
            return boost::json::value_to<T>(v);
        } else {
            return T(std::string(v.as_string()));
        }
    }
};

namespace detail::ledger {

inline auto encode_time(std::chrono::system_clock::time_point t) -> boost::json::value {
    return {static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count())};
}

inline auto decode_time(const boost::json::value& v) -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{boost::json::value_to<std::int64_t>(v)}};
}

inline auto encode_evidence(const capacity_evidence& e) -> boost::json::object {
    boost::json::object o;
    boost::json::array signals;
    for (const auto& s : e._signals) {
        signals.push_back(boost::json::object{
            {"name", s._name}, {"value", s._value}, {"threshold", s._threshold}});
    }
    o["signals"] = std::move(signals);
    o["capacity_refusals"] = static_cast<std::uint64_t>(e._capacity_refusals);
    o["note"] = e._note;
    if (e._projection) {
        const auto& p = *e._projection;
        o["projection"] = boost::json::object{
            {"current", p._current_shards_per_node},
            {"rate_per_minute", p._split_rate_per_minute},
            {"rate_window_ms", static_cast<std::int64_t>(p._rate_window.count())},
            {"horizon_minutes", p._horizon_minutes},
            {"live_nodes", static_cast<std::uint64_t>(p._live_node_count)},
            {"projected", p._projected_shards_per_node}};
    }
    return o;
}

inline auto decode_evidence(const boost::json::object& o) -> capacity_evidence {
    capacity_evidence e;
    for (const auto& s : o.at("signals").as_array()) {
        const auto& so = s.as_object();
        e._signals.push_back(capacity_signal{._name = std::string(so.at("name").as_string()),
                                             ._value = so.at("value").to_number<double>(),
                                             ._threshold = so.at("threshold").to_number<double>()});
    }
    e._capacity_refusals =
        static_cast<std::size_t>(boost::json::value_to<std::uint64_t>(o.at("capacity_refusals")));
    e._note = std::string(o.at("note").as_string());
    if (const auto* p = o.if_contains("projection")) {
        const auto& po = p->as_object();
        capacity_projection proj;
        proj._current_shards_per_node = po.at("current").to_number<double>();
        proj._split_rate_per_minute = po.at("rate_per_minute").to_number<double>();
        proj._rate_window =
            std::chrono::milliseconds{boost::json::value_to<std::int64_t>(po.at("rate_window_ms"))};
        proj._horizon_minutes = po.at("horizon_minutes").to_number<double>();
        proj._live_node_count =
            static_cast<std::size_t>(boost::json::value_to<std::uint64_t>(po.at("live_nodes")));
        proj._projected_shards_per_node = po.at("projected").to_number<double>();
        e._projection = proj;
    }
    return e;
}

}  // namespace detail::ledger

/// @brief One intent as JSON, and back.
template<typename NodeId, typename PlacementGroupId>
auto encode_capacity_intent(const capacity_intent<NodeId, PlacementGroupId>& i)
    -> boost::json::object {
    using node_codec = capacity_ledger_codec<NodeId>;
    using group_codec = capacity_ledger_codec<PlacementGroupId>;
    boost::json::object o;
    o["key"] = i._key;
    o["kind"] = static_cast<std::uint64_t>(i._kind);
    o["state"] = static_cast<std::uint64_t>(i._state);
    o["reason"] = static_cast<std::uint64_t>(i._reason);
    o["evidence"] = detail::ledger::encode_evidence(i._evidence);
    o["group"] = group_codec::encode(i._group);
    if (i._node) {
        o["node"] = node_codec::encode(*i._node);
    }
    o["address"] = i._address;
    o["fencing_token"] = i._fencing_token;
    o["observed_cluster_size"] = static_cast<std::uint64_t>(i._observed_cluster_size);
    o["created_at"] = detail::ledger::encode_time(i._created_at);
    o["updated_at"] = detail::ledger::encode_time(i._updated_at);
    o["deadline"] = detail::ledger::encode_time(i._deadline);
    boost::json::array attempts;
    for (const auto& a : i._attempts) {
        attempts.push_back(
            boost::json::object{{"group", group_codec::encode(a._group)}, {"outcome", a._outcome}});
    }
    o["attempts"] = std::move(attempts);
    o["note"] = i._note;
    return o;
}

template<typename NodeId, typename PlacementGroupId>
auto decode_capacity_intent(const boost::json::object& o)
    -> capacity_intent<NodeId, PlacementGroupId> {
    using node_codec = capacity_ledger_codec<NodeId>;
    using group_codec = capacity_ledger_codec<PlacementGroupId>;
    capacity_intent<NodeId, PlacementGroupId> i;
    i._key = std::string(o.at("key").as_string());
    i._kind = static_cast<capacity_intent_kind>(boost::json::value_to<std::uint64_t>(o.at("kind")));
    i._state =
        static_cast<capacity_intent_state>(boost::json::value_to<std::uint64_t>(o.at("state")));
    i._reason = static_cast<capacity_reason>(boost::json::value_to<std::uint64_t>(o.at("reason")));
    i._evidence = detail::ledger::decode_evidence(o.at("evidence").as_object());
    i._group = group_codec::decode(o.at("group"));
    if (const auto* n = o.if_contains("node")) {
        i._node = node_codec::decode(*n);
    }
    i._address = std::string(o.at("address").as_string());
    i._fencing_token = boost::json::value_to<std::uint64_t>(o.at("fencing_token"));
    i._observed_cluster_size = static_cast<std::size_t>(
        boost::json::value_to<std::uint64_t>(o.at("observed_cluster_size")));
    i._created_at = detail::ledger::decode_time(o.at("created_at"));
    i._updated_at = detail::ledger::decode_time(o.at("updated_at"));
    i._deadline = detail::ledger::decode_time(o.at("deadline"));
    for (const auto& a : o.at("attempts").as_array()) {
        const auto& ao = a.as_object();
        i._attempts.push_back(capacity_placement_attempt<PlacementGroupId>{
            ._group = group_codec::decode(ao.at("group")),
            ._outcome = std::string(ao.at("outcome").as_string())});
    }
    i._note = std::string(o.at("note").as_string());
    return i;
}

// ─────────────────────────────────────────────────────────────────────────────
// The core every shipped ledger is built on
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The committed state and its validation rules, with no durability.
///
/// Deterministic: the same commands in the same order produce the same state,
/// which is what lets the replicated ledger apply it on every replica.
template<typename NodeId, typename PlacementGroupId> class capacity_ledger_core {
public:
    using intent_type = capacity_intent<NodeId, PlacementGroupId>;
    using patch_type = capacity_intent_patch<NodeId, PlacementGroupId>;
    using time_point = std::chrono::system_clock::time_point;

    /// @brief Add a new intent. False for a duplicate key or a wrong first state.
    auto apply_record(const intent_type& intent) -> bool {
        const auto first = intent._kind == capacity_intent_kind::scale_out
                               ? capacity_intent_state::requested
                               : capacity_intent_state::draining;
        if (intent._key.empty() || intent._state != first || _intents.contains(intent._key)) {
            return false;
        }
        _intents.emplace(intent._key, intent);
        return true;
    }

    /// @brief Apply one transition. False if the table forbids it or the key
    ///        is unknown; the state is then unchanged.
    auto apply_transition(const std::string& key, capacity_intent_state to, const patch_type& patch,
                          time_point at) -> bool {
        auto it = _intents.find(key);
        if (it == _intents.end()) {
            return false;
        }
        auto& i = it->second;
        if (!capacity_intent_transition_allowed(i._kind, i._state, to)) {
            return false;
        }
        i._state = to;
        i._updated_at = at;
        if (patch._node) {
            i._node = patch._node;
        }
        if (patch._address) {
            i._address = *patch._address;
        }
        if (patch._group) {
            i._group = *patch._group;
        }
        if (patch._deadline) {
            i._deadline = *patch._deadline;
        }
        if (patch._note) {
            i._note = *patch._note;
        }
        if (patch._attempt) {
            i._attempts.push_back(*patch._attempt);
        }
        return true;
    }

    /// @brief Drop terminal intents last updated before `before`. Open intents
    ///        are never dropped, however old.
    auto apply_compact(time_point before) -> std::size_t {
        std::size_t dropped = 0;
        for (auto it = _intents.begin(); it != _intents.end();) {
            if (it->second.terminal() && it->second._updated_at < before) {
                it = _intents.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        return dropped;
    }

    [[nodiscard]] auto intents() const -> std::vector<intent_type> {
        std::vector<intent_type> out;
        out.reserve(_intents.size());
        for (const auto& [_, i] : _intents) {
            out.push_back(i);
        }
        return out;
    }

    [[nodiscard]] auto find(const std::string& key) const -> std::optional<intent_type> {
        auto it = _intents.find(key);
        if (it == _intents.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] auto size() const -> std::size_t { return _intents.size(); }

    /// @brief Replace the state with already-committed intents, unvalidated.
    ///        For loading a view of state some other core already validated.
    auto load(const std::vector<intent_type>& intents) -> void {
        _intents.clear();
        for (const auto& i : intents) {
            _intents.emplace(i._key, i);
        }
    }

    /// @brief The token for a recorded intent. Only ledgers call this, and only
    ///        once the record is durable.
    [[nodiscard]] auto mint_token(const std::string& key) const
        -> std::optional<capacity_intent_token> {
        auto it = _intents.find(key);
        if (it == _intents.end()) {
            return std::nullopt;
        }
        return capacity_intent_token{key, it->second._fencing_token};
    }

    // ── serialisation ───────────────────────────────────────────────────────

    [[nodiscard]] auto to_json() const -> boost::json::value {
        boost::json::array arr;
        for (const auto& [_, i] : _intents) {
            arr.push_back(encode_capacity_intent(i));
        }
        return boost::json::object{{"version", 1}, {"intents", std::move(arr)}};
    }

    auto from_json(const boost::json::value& v) -> void {
        _intents.clear();
        for (const auto& i : v.as_object().at("intents").as_array()) {
            auto intent = decode_capacity_intent<NodeId, PlacementGroupId>(i.as_object());
            auto key = intent._key;
            _intents.emplace(std::move(key), std::move(intent));
        }
    }

    // ── commands, for a replicated log ──────────────────────────────────────

    [[nodiscard]] static auto encode_record(const intent_type& intent) -> std::vector<std::byte> {
        return to_bytes(
            boost::json::object{{"op", "record"}, {"intent", encode_capacity_intent(intent)}});
    }

    [[nodiscard]] static auto encode_transition(const std::string& key, capacity_intent_state to,
                                                const patch_type& patch, time_point at)
        -> std::vector<std::byte> {
        using node_codec = capacity_ledger_codec<NodeId>;
        using group_codec = capacity_ledger_codec<PlacementGroupId>;
        boost::json::object p;
        if (patch._node) {
            p["node"] = node_codec::encode(*patch._node);
        }
        if (patch._address) {
            p["address"] = *patch._address;
        }
        if (patch._group) {
            p["group"] = group_codec::encode(*patch._group);
        }
        if (patch._deadline) {
            p["deadline"] = detail::ledger::encode_time(*patch._deadline);
        }
        if (patch._note) {
            p["note"] = *patch._note;
        }
        if (patch._attempt) {
            p["attempt"] =
                boost::json::object{{"group", group_codec::encode(patch._attempt->_group)},
                                    {"outcome", patch._attempt->_outcome}};
        }
        return to_bytes(boost::json::object{{"op", "transition"},
                                            {"key", key},
                                            {"to", static_cast<std::uint64_t>(to)},
                                            {"at", detail::ledger::encode_time(at)},
                                            {"patch", std::move(p)}});
    }

    [[nodiscard]] static auto encode_compact(time_point before) -> std::vector<std::byte> {
        return to_bytes(boost::json::object{{"op", "compact"},
                                            {"before", detail::ledger::encode_time(before)}});
    }

    /// @brief Apply one encoded command. True if it changed (or was allowed to
    ///        change) the state; a malformed command is false, never a throw —
    ///        a replica must not crash on an entry every other replica applied.
    auto apply_command(const std::vector<std::byte>& bytes) -> bool {
        try {
            const auto v = boost::json::parse(
                std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            const auto& o = v.as_object();
            const auto op = std::string(o.at("op").as_string());
            if (op == "record") {
                return apply_record(
                    decode_capacity_intent<NodeId, PlacementGroupId>(o.at("intent").as_object()));
            }
            if (op == "transition") {
                using node_codec = capacity_ledger_codec<NodeId>;
                using group_codec = capacity_ledger_codec<PlacementGroupId>;
                const auto& po = o.at("patch").as_object();
                patch_type patch;
                if (const auto* n = po.if_contains("node")) {
                    patch._node = node_codec::decode(*n);
                }
                if (const auto* a = po.if_contains("address")) {
                    patch._address = std::string(a->as_string());
                }
                if (const auto* g = po.if_contains("group")) {
                    patch._group = group_codec::decode(*g);
                }
                if (const auto* d = po.if_contains("deadline")) {
                    patch._deadline = detail::ledger::decode_time(*d);
                }
                if (const auto* n = po.if_contains("note")) {
                    patch._note = std::string(n->as_string());
                }
                if (const auto* a = po.if_contains("attempt")) {
                    const auto& ao = a->as_object();
                    patch._attempt = capacity_placement_attempt<PlacementGroupId>{
                        ._group = group_codec::decode(ao.at("group")),
                        ._outcome = std::string(ao.at("outcome").as_string())};
                }
                return apply_transition(std::string(o.at("key").as_string()),
                                        static_cast<capacity_intent_state>(
                                            boost::json::value_to<std::uint64_t>(o.at("to"))),
                                        patch, detail::ledger::decode_time(o.at("at")));
            }
            if (op == "compact") {
                apply_compact(detail::ledger::decode_time(o.at("before")));
                return true;
            }
        } catch (const std::exception&) {
        }
        return false;
    }

    [[nodiscard]] static auto to_bytes(const boost::json::value& v) -> std::vector<std::byte> {
        const auto s = boost::json::serialize(v);
        std::vector<std::byte> out(s.size());
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }

private:
    std::map<std::string, intent_type> _intents;
};

// ─────────────────────────────────────────────────────────────────────────────
// Synchronous ledgers: every write is committed or rejected on the spot
// ─────────────────────────────────────────────────────────────────────────────

namespace detail::ledger {

/// @brief Ticket bookkeeping shared by the ledgers whose writes resolve at once.
template<typename NodeId, typename PlacementGroupId> class immediate_ledger_base {
public:
    using intent_type = capacity_intent<NodeId, PlacementGroupId>;
    using patch_type = capacity_intent_patch<NodeId, PlacementGroupId>;

    [[nodiscard]] auto status(capacity_ledger_ticket t) -> capacity_ledger_write_status {
        std::lock_guard lock(_mutex);
        auto it = _tickets.find(t._id);
        return it == _tickets.end() ? capacity_ledger_write_status::rejected : it->second._status;
    }

    [[nodiscard]] auto token(capacity_ledger_ticket t) -> std::optional<capacity_intent_token> {
        std::lock_guard lock(_mutex);
        auto it = _tickets.find(t._id);
        if (it == _tickets.end() || it->second._status != capacity_ledger_write_status::committed ||
            it->second._record_key.empty()) {
            return std::nullopt;
        }
        return _core.mint_token(it->second._record_key);
    }

    auto forget(capacity_ledger_ticket t) -> void {
        std::lock_guard lock(_mutex);
        _tickets.erase(t._id);
    }

    [[nodiscard]] auto intents() const -> std::vector<intent_type> {
        std::lock_guard lock(_mutex);
        return _core.intents();
    }

    [[nodiscard]] auto find(const std::string& key) const -> std::optional<intent_type> {
        std::lock_guard lock(_mutex);
        return _core.find(key);
    }

    [[nodiscard]] auto size() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _core.size();
    }

protected:
    struct ticket_state {
        capacity_ledger_write_status _status{capacity_ledger_write_status::rejected};
        std::string _record_key;
    };

    /// Runs `mutate` against the core under the lock, then `persist` if it
    /// changed something. A persist failure rolls the core back and rejects.
    template<typename Mutate, typename Persist>
    auto write(Mutate&& mutate, Persist&& persist, std::string record_key = {})
        -> capacity_ledger_ticket {
        std::lock_guard lock(_mutex);
        const capacity_ledger_ticket t{++_next_ticket};
        auto before = _core;
        bool ok = mutate(_core);
        if (ok && !persist(_core)) {
            _core = std::move(before);
            ok = false;
        }
        _tickets[t._id] = ticket_state{._status = ok ? capacity_ledger_write_status::committed
                                                     : capacity_ledger_write_status::rejected,
                                       ._record_key = ok ? std::move(record_key) : std::string{}};
        return t;
    }

    mutable std::mutex _mutex;
    capacity_ledger_core<NodeId, PlacementGroupId> _core;
    std::uint64_t _next_ticket{0};
    std::unordered_map<std::uint64_t, ticket_state> _tickets;
};

}  // namespace detail::ledger

/// @brief A ledger in memory only.
///
/// For tests and for embedded use where the controller and everything it
/// provisions share one process lifetime. **Not durable**: a restart forgets
/// every intent, so a machine provisioned before it can only be reaped by its
/// join deadline, never matched by key.
template<typename NodeId, typename PlacementGroupId = std::string>
class memory_capacity_ledger
    : public detail::ledger::immediate_ledger_base<NodeId, PlacementGroupId> {
    using base = detail::ledger::immediate_ledger_base<NodeId, PlacementGroupId>;

public:
    using typename base::intent_type;
    using typename base::patch_type;

    auto record(const intent_type& intent) -> capacity_ledger_ticket {
        return this->write([&](auto& core) { return core.apply_record(intent); },
                           [](auto&) { return true; }, intent._key);
    }
    auto transition(const std::string& key, capacity_intent_state to, const patch_type& patch,
                    std::chrono::system_clock::time_point at) -> capacity_ledger_ticket {
        return this->write([&](auto& core) { return core.apply_transition(key, to, patch, at); },
                           [](auto&) { return true; });
    }
    auto compact(std::chrono::system_clock::time_point before) -> capacity_ledger_ticket {
        return this->write(
            [&](auto& core) {
                core.apply_compact(before);
                return true;
            },
            [](auto&) { return true; });
    }
};

/// @brief A ledger in one local file, rewritten atomically on every write.
///
/// **For single-controller deployments** (Requirement 8.7). Durable across a
/// restart of the controller's process, which is what record-before-act needs;
/// not shared with anyone, so a second controller on another machine would
/// keep a second, disagreeing ledger — the lease cannot detect that, and it is
/// documented as unsupported rather than papered over.
///
/// Each write serialises the whole ledger to `<path>.tmp`, flushes it, and
/// renames it over `<path>`. A crash mid-write leaves the previous file
/// intact. Writes are rare (one per intent state change) and the ledger is
/// bounded by compaction, so rewriting it whole is cheaper than a log format
/// would be to get right.
template<typename NodeId, typename PlacementGroupId = std::string>
class file_capacity_ledger
    : public detail::ledger::immediate_ledger_base<NodeId, PlacementGroupId> {
    using base = detail::ledger::immediate_ledger_base<NodeId, PlacementGroupId>;

public:
    using typename base::intent_type;
    using typename base::patch_type;

    /// @brief Opens (or creates) the ledger at `path`.
    /// @throws std::runtime_error if an existing file cannot be parsed. A
    ///         ledger that silently started empty would forget every intent it
    ///         exists to remember.
    explicit file_capacity_ledger(std::filesystem::path path) : _path(std::move(path)) {
        if (std::filesystem::exists(_path)) {
            std::ifstream in(_path, std::ios::binary);
            std::stringstream buf;
            buf << in.rdbuf();
            try {
                this->_core.from_json(boost::json::parse(buf.str()));
            } catch (const std::exception& e) {
                throw std::runtime_error("file_capacity_ledger: cannot read " + _path.string() +
                                         ": " + e.what());
            }
        }
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

    auto record(const intent_type& intent) -> capacity_ledger_ticket {
        return this->write([&](auto& core) { return core.apply_record(intent); },
                           [this](const auto& core) { return persist(core); }, intent._key);
    }
    auto transition(const std::string& key, capacity_intent_state to, const patch_type& patch,
                    std::chrono::system_clock::time_point at) -> capacity_ledger_ticket {
        return this->write([&](auto& core) { return core.apply_transition(key, to, patch, at); },
                           [this](const auto& core) { return persist(core); });
    }
    auto compact(std::chrono::system_clock::time_point before) -> capacity_ledger_ticket {
        return this->write(
            [&](auto& core) {
                core.apply_compact(before);
                return true;
            },
            [this](const auto& core) { return persist(core); });
    }

private:
    auto persist(const capacity_ledger_core<NodeId, PlacementGroupId>& core) const -> bool {
        const auto tmp = std::filesystem::path{_path.string() + ".tmp"};
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out << boost::json::serialize(core.to_json());
            out.flush();
            if (!out) {
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, _path, ec);
        return !ec;
    }

    std::filesystem::path _path;
};

// ─────────────────────────────────────────────────────────────────────────────
// The replicated ledger: the shipped default
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The ledger as a Raft state machine, for the coordination group.
///
/// Use it as the coordination group's `state_machine_type`. Every replica
/// applies the same commands to the same `capacity_ledger_core`, so the ledger
/// survives the loss of any minority — and the lease (leadership of the same
/// group) and the ledger's durability come from one consensus decision rather
/// than two systems that can disagree (Requirement 8.7, design §7).
///
/// `apply` returns one byte: 1 if the command took effect, 0 if the core
/// refused it (an illegal transition, a duplicate key). That byte is how a
/// proposal's ticket learns its fate.
template<typename NodeId, typename PlacementGroupId = std::string,
         typename LogIndex = std::uint64_t>
class capacity_ledger_state_machine {
public:
    using core_type = capacity_ledger_core<NodeId, PlacementGroupId>;

    auto apply(const std::vector<std::byte>& command, LogIndex index) -> std::vector<std::byte> {
        std::lock_guard lock(*_mutex);
        _last_applied = index;
        const bool ok = _core.apply_command(command);
        return {ok ? std::byte{1} : std::byte{0}};
    }

    [[nodiscard]] auto get_state() const -> std::vector<std::byte> {
        std::lock_guard lock(*_mutex);
        return core_type::to_bytes(_core.to_json());
    }

    auto restore_from_snapshot(const std::vector<std::byte>& data, LogIndex index) -> void {
        std::lock_guard lock(*_mutex);
        _last_applied = index;
        if (data.empty()) {
            _core = core_type{};
            return;
        }
        _core.from_json(boost::json::parse(
            std::string_view{reinterpret_cast<const char*>(data.data()), data.size()}));
    }

    /// @brief The committed state, for `replicated_capacity_ledger`'s reads.
    [[nodiscard]] auto intents() const -> std::vector<capacity_intent<NodeId, PlacementGroupId>> {
        std::lock_guard lock(*_mutex);
        return _core.intents();
    }
    [[nodiscard]] auto find(const std::string& key) const
        -> std::optional<capacity_intent<NodeId, PlacementGroupId>> {
        std::lock_guard lock(*_mutex);
        return _core.find(key);
    }
    [[nodiscard]] auto last_applied() const -> LogIndex {
        std::lock_guard lock(*_mutex);
        return _last_applied;
    }

private:
    // Behind a pointer so the state machine stays movable, which `node_config`
    // requires of whatever it is handed.
    std::unique_ptr<std::mutex> _mutex{std::make_unique<std::mutex>()};
    core_type _core;
    LogIndex _last_applied{0};
};

static_assert(state_machine<capacity_ledger_state_machine<std::uint64_t>, std::uint64_t>);

/// @brief The controller's view of a `capacity_ledger_state_machine` running
///        in a Raft group.
///
/// Two hooks, so this header never names a node or host type:
///
/// - `propose(bytes) -> Future` submits a command to the coordination group
///   (`node::submit_command`, or `multi_raft::submit_command` on the group's
///   id). The future resolves with the state machine's one-byte answer once
///   the entry has applied.
/// - `read()` returns the locally applied intents — `with_state_machine` on
///   the controller's own replica, which is the leader while the lease holds.
///
/// A ticket stays `pending` until its future is ready, and is **never waited
/// on**: the controller polls it from `step()`, so a slow commit delays an
/// action rather than stalling the heartbeat path — which matters, because in
/// an in-process deployment the thread asking may be the one that ticks the
/// group.
///
/// @tparam Future What `propose` returns; must satisfy `kythira::future`.
template<typename NodeId, typename PlacementGroupId, typename Future>
class replicated_capacity_ledger {
public:
    using intent_type = capacity_intent<NodeId, PlacementGroupId>;
    using patch_type = capacity_intent_patch<NodeId, PlacementGroupId>;
    using core_type = capacity_ledger_core<NodeId, PlacementGroupId>;

    replicated_capacity_ledger(std::function<Future(std::vector<std::byte>)> propose,
                               std::function<std::vector<intent_type>()> read)
        : _propose(std::move(propose)), _read(std::move(read)) {}

    auto record(const intent_type& intent) -> capacity_ledger_ticket {
        return submit(core_type::encode_record(intent), intent._key);
    }
    auto transition(const std::string& key, capacity_intent_state to, const patch_type& patch,
                    std::chrono::system_clock::time_point at) -> capacity_ledger_ticket {
        return submit(core_type::encode_transition(key, to, patch, at), {});
    }
    auto compact(std::chrono::system_clock::time_point before) -> capacity_ledger_ticket {
        return submit(core_type::encode_compact(before), {});
    }

    [[nodiscard]] auto status(capacity_ledger_ticket t) -> capacity_ledger_write_status {
        std::lock_guard lock(_mutex);
        auto it = _tickets.find(t._id);
        if (it == _tickets.end()) {
            return capacity_ledger_write_status::rejected;
        }
        return poll(it->second);
    }

    [[nodiscard]] auto token(capacity_ledger_ticket t) -> std::optional<capacity_intent_token> {
        std::string key;
        {
            std::lock_guard lock(_mutex);
            auto it = _tickets.find(t._id);
            if (it == _tickets.end() ||
                poll(it->second) != capacity_ledger_write_status::committed ||
                it->second._record_key.empty()) {
                return std::nullopt;
            }
            key = it->second._record_key;
        }
        // Mint from the committed state, not from the proposal: the token
        // carries what the ledger holds.
        core_type view;
        view.load(_read());
        return view.mint_token(key);
    }

    auto forget(capacity_ledger_ticket t) -> void {
        std::lock_guard lock(_mutex);
        _tickets.erase(t._id);
    }

    [[nodiscard]] auto intents() const -> std::vector<intent_type> { return _read(); }

    [[nodiscard]] auto find(const std::string& key) const -> std::optional<intent_type> {
        for (auto& i : _read()) {
            if (i._key == key) {
                return i;
            }
        }
        return std::nullopt;
    }

private:
    struct ticket_state {
        std::optional<Future> _future;
        capacity_ledger_write_status _status{capacity_ledger_write_status::pending};
        std::string _record_key;
    };

    auto submit(std::vector<std::byte> bytes, std::string record_key) -> capacity_ledger_ticket {
        std::lock_guard lock(_mutex);
        const capacity_ledger_ticket t{++_next_ticket};
        ticket_state s;
        s._record_key = std::move(record_key);
        try {
            s._future.emplace(_propose(std::move(bytes)));
        } catch (...) {
            s._status = capacity_ledger_write_status::rejected;
        }
        _tickets.emplace(t._id, std::move(s));
        return t;
    }

    /// Non-blocking: a future that is not ready leaves the ticket pending.
    static auto poll(ticket_state& s) -> capacity_ledger_write_status {
        if (s._status != capacity_ledger_write_status::pending || !s._future) {
            return s._status;
        }
        if (!s._future->isReady()) {
            return s._status;
        }
        try {
            auto result = std::move(*s._future).get();
            s._status = (!result.empty() && result[0] == std::byte{1})
                            ? capacity_ledger_write_status::committed
                            : capacity_ledger_write_status::rejected;
        } catch (...) {
            // Not leader, timed out, lost: the write may or may not apply
            // later, and acting as though it did is the one thing that is
            // never safe. Rejected; reconciliation sorts out the rest.
            s._status = capacity_ledger_write_status::rejected;
        }
        s._future.reset();
        return s._status;
    }

    std::function<Future(std::vector<std::byte>)> _propose;
    std::function<std::vector<intent_type>()> _read;
    mutable std::mutex _mutex;
    std::uint64_t _next_ticket{0};
    std::unordered_map<std::uint64_t, ticket_state> _tickets;
};

static_assert(capacity_ledger<memory_capacity_ledger<std::uint64_t>, std::uint64_t, std::string>);
static_assert(capacity_ledger<file_capacity_ledger<std::uint64_t>, std::uint64_t, std::string>);

}  // namespace kythira
