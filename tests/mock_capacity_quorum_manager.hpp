// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file mock_capacity_quorum_manager.hpp
/// @brief A deterministic `quorum_manager` for the elastic-shard-capacity
///        suites (task 7 of `.kiro/specs/elastic-shard-capacity/`), plus the
///        manual clock and manual executor those suites drive it with.
///
/// Everything a cloud does to a capacity controller on a bad day, on demand
/// and without a wall clock:
///
///  - **latency**: a call's future resolves only once the injected clock has
///    passed its ready time, so "the provider took four minutes" is one
///    `clock.advance(4min)` away, not four minutes of test run time;
///  - **failure** and **stock-out** per placement group, or scripted per call;
///  - **partial success**: the provider says yes and creates a machine that is
///    unreachable — it exists (and bills) but never answers;
///  - **never joins**: the machine boots and is reachable, but never
///    heartbeats into the cluster;
///  - **never returns**: the future is never fulfilled at all.
///
/// Every call is recorded for assertions, and every machine the mock ever
/// created is tracked until decommissioned — which is what lets the property
/// suite check "live provider machines ≤ recorded intents" against the
/// provider's own view rather than the controller's.
///
/// No `sleep` appears in this file or in anything that uses it: time is the
/// clock's, and only a test moves it.

#include <raft/future_default.hpp>
#include <raft/quorum_management.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::testing {

// ─────────────────────────────────────────────────────────────────────────────
// Time and execution, by hand
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A clock only the test moves.
class manual_clock {
public:
    using time_point = std::chrono::system_clock::time_point;

    explicit manual_clock(time_point start = time_point{} + std::chrono::hours{24 * 365 * 50})
        : _now(start) {}

    [[nodiscard]] auto now() const -> time_point {
        std::lock_guard lock(_mutex);
        return _now;
    }

    auto advance(std::chrono::milliseconds d) -> void {
        std::lock_guard lock(_mutex);
        _now += d;
    }

    /// @brief As a `std::function`, for the controller's clock hook.
    [[nodiscard]] auto fn() -> std::function<time_point()> {
        return [this] { return now(); };
    }

private:
    mutable std::mutex _mutex;
    time_point _now;
};

/// @brief An executor that runs nothing until told to.
///
/// The controller hands provider work to its executor; a test decides when
/// that work runs, which is how "step() never calls a provider inline" becomes
/// an assertion: after `step()`, the mock has seen no call until `run_all()`.
class manual_executor {
public:
    auto add(std::function<void()> work) -> void {
        std::lock_guard lock(_mutex);
        _queue.push_back(std::move(work));
    }

    /// @brief Run everything queued, including work queued while running.
    auto run_all() -> std::size_t {
        std::size_t n = 0;
        while (true) {
            std::function<void()> next;
            {
                std::lock_guard lock(_mutex);
                if (_queue.empty()) {
                    return n;
                }
                next = std::move(_queue.front());
                _queue.pop_front();
            }
            next();
            ++n;
        }
    }

    [[nodiscard]] auto pending() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _queue.size();
    }

    /// @brief As a `std::function`, for the controller's executor hook.
    [[nodiscard]] auto fn() -> std::function<void(std::function<void()>)> {
        return [this](std::function<void()> w) { add(std::move(w)); };
    }

private:
    mutable std::mutex _mutex;
    std::deque<std::function<void()>> _queue;
};

// ─────────────────────────────────────────────────────────────────────────────
// The mock manager
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What one `provision_node` call does.
enum class mock_provision_outcome : std::uint8_t {
    success,       ///< A reachable machine that joins once the harness starts it.
    failure,       ///< The provider refuses (quota, API error).
    stock_out,     ///< The provider has no capacity in this group.
    partial,       ///< Created but unreachable: exists, bills, never answers.
    never_joins,   ///< Created and reachable, never heartbeats into the cluster.
    never_returns  ///< The call never completes at all.
};

/// @brief The program for one call: what happens and how long it takes.
struct mock_provision_script {
    mock_provision_outcome _outcome{mock_provision_outcome::success};
    std::chrono::milliseconds _latency{0};
};

/// @brief One machine the mock created.
struct mock_machine {
    std::uint64_t _id{0};
    std::string _group;
    std::string _idempotency_key;
    mock_provision_outcome _outcome{mock_provision_outcome::success};
    bool _decommissioned{false};

    /// Whether a harness should make this machine heartbeat into the cluster.
    [[nodiscard]] auto joins() const -> bool {
        return !_decommissioned && _outcome == mock_provision_outcome::success;
    }
    /// Whether `assess_quorum` reports it live.
    [[nodiscard]] auto reachable() const -> bool {
        return !_decommissioned && _outcome != mock_provision_outcome::partial;
    }
};

/// @brief One recorded call, for assertions.
struct mock_call {
    std::string _op;  ///< provision, decommission, assess, find_by_key, set_group_target
    std::string _group;
    std::optional<std::uint64_t> _node;
    std::string _key;
    std::chrono::system_clock::time_point _at{};
};

/// @brief The deterministic quorum manager.
///
/// @tparam Keyed     Expose the optional idempotency-key refinement
///                   (`provision_node_keyed` / `find_by_idempotency_key`), as a
///                   manager that can tag what it creates would.
/// @tparam Resizable Expose the optional `set_group_target` refinement, as a
///                   group-capacity manager (ASG, MIG, VMSS, …) would.
///
/// State lives behind a `shared_ptr`, so copies share it: the controller owns
/// one copy, the test keeps another to program and inspect.
template<bool Keyed = true, bool Resizable = false> class mock_capacity_quorum_manager {
public:
    using node_id_type = std::uint64_t;
    using address_type = std::string;
    using placement_group_id_type = std::string;
    using peer_type = peer_info<node_id_type, address_type>;
    using health_type = quorum_health<node_id_type, placement_group_id_type>;
    using time_point = std::chrono::system_clock::time_point;

    mock_capacity_quorum_manager(manual_clock& clock, desired_topology<std::string> topology,
                                 std::uint64_t first_new_id = 1000)
        : _s(std::make_shared<state>(clock, std::move(topology), first_new_id)) {}

    // ── programming ─────────────────────────────────────────────────────────

    /// @brief The next calls, in order. Falls back to the per-group rule.
    auto script(std::vector<mock_provision_script> calls) -> void {
        std::lock_guard lock(_s->_mutex);
        for (auto& c : calls) {
            _s->_scripted.push_back(c);
        }
    }

    /// @brief Every call into `group` has this outcome until changed.
    auto set_group_outcome(const std::string& group, mock_provision_script s) -> void {
        std::lock_guard lock(_s->_mutex);
        _s->_group_rule[group] = s;
    }

    /// @brief Make `assess_quorum` fail, or report this status.
    auto set_assess_failure(bool fail) -> void {
        std::lock_guard lock(_s->_mutex);
        _s->_assess_fails = fail;
    }
    auto set_status_override(std::optional<quorum_status> s) -> void {
        std::lock_guard lock(_s->_mutex);
        _s->_status_override = s;
    }
    /// @brief Make `decommission_node` fail.
    auto set_decommission_failure(bool fail) -> void {
        std::lock_guard lock(_s->_mutex);
        _s->_decommission_fails = fail;
    }

    /// @brief Pre-existing machines (the cluster before the controller).
    auto add_existing(std::uint64_t id, const std::string& group) -> void {
        std::lock_guard lock(_s->_mutex);
        _s->_machines[id] = mock_machine{._id = id, ._group = group};
    }

    /// @brief Resolve every pending call whose latency has elapsed.
    ///
    /// Called by the test after moving the clock. Calls whose outcome is
    /// `never_returns` are never resolved.
    auto settle() -> std::size_t {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard lock(_s->_mutex);
            const auto now = _s->_clock->now();
            for (auto it = _s->_pending.begin(); it != _s->_pending.end();) {
                if (it->_ready_at <= now) {
                    ready.push_back(std::move(it->_fulfil));
                    it = _s->_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& f : ready) {
            f();
        }
        return ready.size();
    }

    // ── inspection ──────────────────────────────────────────────────────────

    [[nodiscard]] auto calls() const -> std::vector<mock_call> {
        std::lock_guard lock(_s->_mutex);
        return _s->_calls;
    }
    [[nodiscard]] auto call_count(const std::string& op) const -> std::size_t {
        std::lock_guard lock(_s->_mutex);
        return static_cast<std::size_t>(std::count_if(_s->_calls.begin(), _s->_calls.end(),
                                                      [&](const auto& c) { return c._op == op; }));
    }
    [[nodiscard]] auto machines() const -> std::vector<mock_machine> {
        std::lock_guard lock(_s->_mutex);
        std::vector<mock_machine> out;
        for (const auto& [_, m] : _s->_machines) {
            out.push_back(m);
        }
        return out;
    }
    [[nodiscard]] auto machine(std::uint64_t id) const -> std::optional<mock_machine> {
        std::lock_guard lock(_s->_mutex);
        auto it = _s->_machines.find(id);
        if (it == _s->_machines.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    /// @brief Machines the mock created (not pre-existing) still not decommissioned.
    [[nodiscard]] auto live_created_count() const -> std::size_t {
        std::lock_guard lock(_s->_mutex);
        std::size_t n = 0;
        for (const auto& [_, m] : _s->_machines) {
            if (!m._decommissioned && !m._idempotency_key.empty()) {
                ++n;
            }
        }
        return n;
    }
    [[nodiscard]] auto pending_count() const -> std::size_t {
        std::lock_guard lock(_s->_mutex);
        return _s->_pending.size();
    }
    [[nodiscard]] auto group_targets() const -> std::map<std::string, std::size_t> {
        std::lock_guard lock(_s->_mutex);
        return _s->_group_targets;
    }

    // ── the quorum_manager concept ──────────────────────────────────────────

    auto assess_quorum(const std::vector<node_placement<node_id_type, std::string>>& cluster)
        -> future_default<health_type> {
        std::lock_guard lock(_s->_mutex);
        _s->_calls.push_back({._op = "assess", ._at = _s->_clock->now()});
        if (_s->_assess_fails) {
            return future_factory_default::makeExceptionalFuture<health_type>(
                std::make_exception_ptr(std::runtime_error("mock: assess_quorum failed")));
        }
        // The provider's view: every machine it knows about, plus anything
        // the caller named that it does not (reported unreachable).
        health_type h{};
        std::map<std::string, placement_group_health<node_id_type, std::string>> groups;
        for (const auto& t : _s->_topology.groups) {
            groups[t.group_id] =
                placement_group_health<node_id_type, std::string>{.group_id = t.group_id,
                                                                  .live_count = 0,
                                                                  .target_count = t.target_count,
                                                                  .unreachable_nodes = {}};
        }
        std::set<node_id_type> seen;
        for (const auto& [id, m] : _s->_machines) {
            if (m._decommissioned) {
                continue;
            }
            seen.insert(id);
            auto& g = groups[m._group];
            g.group_id = m._group;
            ++h.total_node_count;
            if (m.reachable()) {
                ++g.live_count;
                ++h.live_node_count;
            } else {
                g.unreachable_nodes.push_back(id);
                h.unreachable_nodes.push_back(id);
            }
        }
        for (const auto& np : cluster) {
            if (!seen.contains(np.node_id)) {
                ++h.total_node_count;
                h.unreachable_nodes.push_back(np.node_id);
                groups[np.group_id].group_id = np.group_id;
                groups[np.group_id].unreachable_nodes.push_back(np.node_id);
            }
        }
        for (auto& [_, g] : groups) {
            h.groups.push_back(g);
        }
        if (_s->_status_override) {
            h.status = *_s->_status_override;
        } else if (h.total_node_count == 0 || h.live_node_count * 2 <= h.total_node_count) {
            h.status = h.total_node_count == 0 ? quorum_status::healthy : quorum_status::lost;
        } else if (h.live_node_count == h.total_node_count) {
            h.status = quorum_status::healthy;
        } else {
            h.status = quorum_status::degraded;
        }
        return future_factory_default::makeFuture(std::move(h));
    }

    auto provision_node(std::string group, std::optional<node_id_type> replacing)
        -> future_default<peer_type> {
        return provision(std::move(group), replacing, {});
    }

    auto decommission_node(const node_id_type& id) -> future_default<void> {
        std::lock_guard lock(_s->_mutex);
        _s->_calls.push_back({._op = "decommission", ._node = id, ._at = _s->_clock->now()});
        if (_s->_decommission_fails) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error("mock: decommission failed")));
        }
        // Idempotent, as the concept requires: an unknown or already-removed
        // machine is success.
        if (auto it = _s->_machines.find(id); it != _s->_machines.end()) {
            it->second._decommissioned = true;
        }
        return future_factory_default::makeFuture();
    }

    [[nodiscard]] auto topology() const -> desired_topology<std::string> {
        std::lock_guard lock(_s->_mutex);
        return _s->_topology;
    }

    auto maintain_quorum(const std::vector<node_placement<node_id_type, std::string>>& cluster)
        -> future_default<health_type> {
        return assess_quorum(cluster);
    }

    // ── optional refinements ────────────────────────────────────────────────

    auto provision_node_keyed(std::string group, std::optional<node_id_type> replacing,
                              const std::string& key) -> future_default<peer_type>
    requires Keyed
    {
        return provision(std::move(group), replacing, key);
    }

    auto find_by_idempotency_key(const std::string& key) -> future_default<std::optional<peer_type>>
    requires Keyed
    {
        std::lock_guard lock(_s->_mutex);
        _s->_calls.push_back({._op = "find_by_key", ._key = key, ._at = _s->_clock->now()});
        for (const auto& [id, m] : _s->_machines) {
            if (!m._decommissioned && m._idempotency_key == key) {
                return future_factory_default::makeFuture(
                    std::optional<peer_type>{peer_type{id, address_of(id)}});
            }
        }
        return future_factory_default::makeFuture(std::optional<peer_type>{});
    }

    auto set_group_target(const std::string& group, std::size_t count) -> future_default<void>
    requires Resizable
    {
        std::lock_guard lock(_s->_mutex);
        _s->_calls.push_back(
            {._op = "set_group_target", ._group = group, ._node = count, ._at = _s->_clock->now()});
        _s->_group_targets[group] = count;
        return future_factory_default::makeFuture();
    }

    [[nodiscard]] static auto address_of(node_id_type id) -> std::string {
        return "mock-" + std::to_string(id) + ":7000";
    }

private:
    struct pending_call {
        time_point _ready_at;
        std::function<void()> _fulfil;
    };

    struct state {
        state(manual_clock& clock, desired_topology<std::string> topology, std::uint64_t first)
            : _clock(&clock), _topology(std::move(topology)), _next_id(first) {}

        std::mutex _mutex;
        manual_clock* _clock;
        desired_topology<std::string> _topology;
        std::uint64_t _next_id;
        std::deque<mock_provision_script> _scripted;
        std::map<std::string, mock_provision_script> _group_rule;
        std::map<std::uint64_t, mock_machine> _machines;
        std::vector<mock_call> _calls;
        std::vector<pending_call> _pending;
        std::map<std::string, std::size_t> _group_targets;
        bool _assess_fails{false};
        bool _decommission_fails{false};
        std::optional<quorum_status> _status_override;
    };

    auto provision(std::string group, std::optional<node_id_type>, std::string key)
        -> future_default<peer_type> {
        std::lock_guard lock(_s->_mutex);
        const auto now = _s->_clock->now();
        _s->_calls.push_back({._op = "provision", ._group = group, ._key = key, ._at = now});

        mock_provision_script script;
        if (!_s->_scripted.empty()) {
            script = _s->_scripted.front();
            _s->_scripted.pop_front();
        } else if (auto it = _s->_group_rule.find(group); it != _s->_group_rule.end()) {
            script = it->second;
        }

        // The machine exists from the moment the call is made, for every
        // outcome that creates one — including `never_returns`, whose machine
        // is exactly the orphan a lost call leaves behind.
        std::optional<std::uint64_t> created;
        if (script._outcome != mock_provision_outcome::failure &&
            script._outcome != mock_provision_outcome::stock_out) {
            const auto id = _s->_next_id++;
            _s->_machines[id] = mock_machine{._id = id,
                                             ._group = group,
                                             ._idempotency_key = key.empty() ? "-" : key,
                                             ._outcome = script._outcome};
            created = id;
        }

        promise_default<peer_type> promise;
        auto future = promise.getFuture();
        auto shared = std::make_shared<promise_default<peer_type>>(std::move(promise));
        std::function<void()> fulfil;
        switch (script._outcome) {
            case mock_provision_outcome::failure:
                fulfil = [shared] {
                    shared->setException(
                        std::make_exception_ptr(std::runtime_error("mock: provider refused")));
                };
                break;
            case mock_provision_outcome::stock_out:
                fulfil = [shared, group] {
                    shared->setException(
                        std::make_exception_ptr(std::runtime_error("mock: stock-out in " + group)));
                };
                break;
            case mock_provision_outcome::never_returns:
                // Kept pending forever, so the promise is never broken either.
                _s->_pending.push_back({time_point::max(), [shared] {}});
                return future;
            default:
                fulfil = [shared, id = *created] {
                    shared->setValue(peer_type{id, address_of(id)});
                };
                break;
        }
        if (script._latency <= std::chrono::milliseconds::zero()) {
            // Immediate: fulfil before returning, as a synchronous provider
            // (Docker's REST call, say) does.
            fulfil();
        } else {
            _s->_pending.push_back({now + script._latency, std::move(fulfil)});
        }
        return future;
    }

    std::shared_ptr<state> _s;
};

static_assert(
    quorum_manager<mock_capacity_quorum_manager<>, std::uint64_t, std::string, std::string>);
static_assert(quorum_manager<mock_capacity_quorum_manager<false, true>, std::uint64_t, std::string,
                             std::string>);

}  // namespace kythira::testing
