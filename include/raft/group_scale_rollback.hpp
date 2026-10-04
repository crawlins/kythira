// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file group_scale_rollback.hpp
/// @brief How a group quorum manager undoes a timed-out scale-up
///        (`.kiro/specs/group-scale-up-rollback/`, Requirements 2, 3, 5 and 6).
///
/// Five quorum managers add a Raft node by growing a cloud-managed group
/// (ASG, ESS, OCI instance pool, VMSS, MIG) and, when `provision_timeout`
/// expires, used to undo it by lowering the group's desired size. That
/// leaves the choice of victim to the group's scale-in policy, which does
/// not know which members are Raft voters: a remediation meant to restore
/// quorum could terminate a healthy voter and report only "timeout".
///
/// The decision this header makes instead is: remove by id every instance
/// the scale-up caused (present in the final listing, absent from the
/// pre-growth snapshot, not already leaving), and fall back to restoring the
/// desired size only when there is no such instance, so the cloud has no
/// extra member to choose. That fallback is then audited, because the cloud
/// can list a late launch between the final listing and the shrink.
///
/// No cloud SDK and no network I/O, so every build leg compiles and tests
/// it. Each manager maps its own lifecycle strings onto `member_state`
/// and performs the cloud calls itself.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::group_rollback {

/// The three classes every provider's lifecycle strings map onto.
///
/// `terminal` means the instance will not serve again (ASG `Terminating*`,
/// ESS `Removing`, OCI `TERMINATING`, ...). Such an instance is neither
/// removed by the rollback nor counted as a member the rollback cost.
enum class member_state {
    live,
    pending,
    terminal
};

/// One instance in a group listing.
struct listed_member {
    std::string id;
    member_state state{member_state::live};
    /// The provider's own lifecycle string, for the error message only.
    std::string lifecycle{};
    /// The Raft node id this member carries, when adopted; message only.
    std::string node{};
};

/// What the timeout path will do.
struct rollback_plan {
    /// Fresh, non-terminal instances to remove by id, in listing order.
    std::vector<std::string> remove;
    /// True when nothing fresh is removable and the desired size has to be
    /// restored with a capacity write instead.
    bool restore_desired_size = false;
};

/// What the post-shrink audit found (Requirement 3.2-3.4).
struct audit_result {
    /// Pre-growth members that were serving before the shrink and are gone
    /// or leaving after it: the shrink chose a member.
    std::vector<listed_member> lost_members;
    /// Fresh, non-terminal instances listed after the shrink. Reported, never
    /// removed: a decrementing removal would take the group below its
    /// pre-growth size.
    std::vector<listed_member> late_arrivals;
};

namespace detail {

[[nodiscard]] inline auto contains(const std::vector<std::string>& ids, const std::string& id)
    -> bool {
    return std::ranges::find(ids, id) != ids.end();
}

[[nodiscard]] inline auto find_member(const std::vector<listed_member>& listing,
                                      const std::string& id) -> const listed_member* {
    const auto it = std::ranges::find(listing, id, &listed_member::id);
    return it == listing.end() ? nullptr : &*it;
}

}  // namespace detail

/// @brief Decide how to undo a timed-out scale-up (Requirements 2.1-2.3, 3.1).
///
/// @param pre_growth_ids Every instance id the group listed, in every
///        lifecycle state, immediately before the grow call.
/// @param final_listing The group's listing after the timeout. Empty when
///        the listing failed, which plans a size restore: with nothing known
///        to be fresh there is nothing to name.
///
/// The result never names a pre-growth instance, whatever its state.
[[nodiscard]] inline auto plan_scale_up_rollback(const std::vector<std::string>& pre_growth_ids,
                                                 const std::vector<listed_member>& final_listing)
    -> rollback_plan {
    rollback_plan plan;
    for (const auto& member : final_listing) {
        if (member.id.empty() || member.state == member_state::terminal ||
            detail::contains(pre_growth_ids, member.id) ||
            detail::contains(plan.remove, member.id)) {
            continue;
        }
        plan.remove.push_back(member.id);
    }
    plan.restore_desired_size = plan.remove.empty();
    return plan;
}

/// @brief Compare the listing after a size restore with the one before it
///        (Requirement 3.2-3.4).
///
/// A pre-growth member counts as lost only when it was non-terminal in
/// @p final_listing: one that was already leaving before the shrink is not
/// the rollback's doing. A member absent from @p final_listing (the listing
/// failed or missed it) is judged by @p pre_growth_ids alone, so a lost
/// voter is still reported when the final listing came back empty.
[[nodiscard]] inline auto audit_after_shrink(const std::vector<std::string>& pre_growth_ids,
                                             const std::vector<listed_member>& final_listing,
                                             const std::vector<listed_member>& after_shrink)
    -> audit_result {
    audit_result out;
    for (const auto& id : pre_growth_ids) {
        const auto* before = detail::find_member(final_listing, id);
        if (before != nullptr && before->state == member_state::terminal) {
            continue;
        }
        const auto* after = detail::find_member(after_shrink, id);
        if (after == nullptr) {
            out.lost_members.push_back(before != nullptr ? *before : listed_member{.id = id});
        } else if (after->state == member_state::terminal) {
            auto lost = *after;
            if (lost.node.empty() && before != nullptr) {
                lost.node = before->node;
            }
            out.lost_members.push_back(std::move(lost));
        }
    }
    for (const auto& member : after_shrink) {
        if (member.state != member_state::terminal &&
            !detail::contains(pre_growth_ids, member.id)) {
            out.late_arrivals.push_back(member);
        }
    }
    return out;
}

/// The settle window the post-shrink audit polls for:
/// `min(provision_timeout, max(3 * poll_interval, 30 s))`.
[[nodiscard]] inline auto settle_window(std::chrono::milliseconds provision_timeout,
                                        std::chrono::milliseconds poll_interval)
    -> std::chrono::milliseconds {
    return std::min(provision_timeout, std::max<std::chrono::milliseconds>(
                                           3 * poll_interval, std::chrono::seconds{30}));
}

/// @brief Poll @p list until two consecutive listings agree or @p window
///        runs out, and return the last listing that succeeded.
///
/// Reports only; it never mutates. A failed listing is skipped, and when
/// every listing fails the result is `std::nullopt`, which the caller
/// reports as "audit unavailable" rather than as a clean audit.
///
/// @tparam Lister `() -> std::vector<listed_member>`, may throw.
template<typename Lister>
[[nodiscard]] auto settle_listing(Lister&& list, std::chrono::milliseconds window,
                                  std::chrono::milliseconds poll_interval)
    -> std::optional<std::vector<listed_member>> {
    const auto same = [](const std::vector<listed_member>& a, const std::vector<listed_member>& b) {
        return std::ranges::equal(a, b, [](const listed_member& x, const listed_member& y) {
            return x.id == y.id && x.state == y.state;
        });
    };
    const auto deadline = std::chrono::steady_clock::now() + window;
    std::optional<std::vector<listed_member>> last;
    for (;;) {
        try {
            auto current = list();
            if (last.has_value() && same(*last, current)) {
                return current;
            }
            last = std::move(current);
        } catch (...) {
            // A failed read is not evidence either way; the window bounds us.
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return last;
        }
        std::this_thread::sleep_for(poll_interval);
    }
}

/// Everything the timeout path did, for the error message (Requirement 5.2).
struct rollback_outcome {
    rollback_plan plan;
    /// Ids from `plan.remove` whose removal succeeded or found them gone.
    std::vector<std::string> removed;
    /// `(id, cloud error)` for each removal that failed.
    std::vector<std::pair<std::string, std::string>> removal_failures;
    /// The size the restore wrote, when `plan.restore_desired_size`.
    std::optional<std::int64_t> restored_size;
    /// The capacity write's error, when it failed.
    std::string restore_error;
    /// The audit, when one ran and at least one listing succeeded.
    std::optional<audit_result> audit;
    /// True when a restore ran but no listing after it succeeded.
    bool audit_unavailable = false;
    /// The final listing the plan was made from, for the lifecycle strings.
    std::vector<listed_member> final_listing;
};

/// @brief Carry out the whole timeout path: plan, remove by id or restore the
///        size, and audit after a restore (Requirements 2-3).
///
/// Every manager's timeout path is this sequence with its own cloud calls
/// plugged in, so the sequence lives here once and is unit tested without a
/// cloud.
///
/// @param pre_growth_ids Every instance id listed immediately before the grow.
/// @param final_listing  The listing after the timeout; empty if it failed.
/// @param original_size  The desired size before the grow, which a restore writes.
/// @param remove `(const std::string& id) -> std::string`: removes one instance
///        by id and decrements the desired size; returns the cloud's error
///        text, or empty on success or when the instance is already gone.
/// @param restore `(std::int64_t size) -> void`: writes the desired size;
///        throws on failure.
/// @param list `() -> std::vector<listed_member>`: the listing the audit
///        polls; may throw.
/// @param window, poll_interval The audit's settle bounds; see
///        `settle_window`.
///
/// Never throws: a timeout path that throws from its rollback would replace
/// the timeout with a less useful error. Anything unexpected is recorded as
/// `restore_error` so `describe` still says what happened.
template<typename Remove, typename Restore, typename Lister>
[[nodiscard]] auto execute_rollback(const std::vector<std::string>& pre_growth_ids,
                                    std::vector<listed_member> final_listing,
                                    std::int64_t original_size, Remove&& remove, Restore&& restore,
                                    Lister&& list, std::chrono::milliseconds window,
                                    std::chrono::milliseconds poll_interval) noexcept
    -> rollback_outcome {
    rollback_outcome outcome;
    try {
        outcome.final_listing = std::move(final_listing);
        outcome.plan = plan_scale_up_rollback(pre_growth_ids, outcome.final_listing);
        for (const auto& id : outcome.plan.remove) {
            std::string error;
            try {
                error = remove(id);
            } catch (const std::exception& ex) {
                error = ex.what();
            }
            if (error.empty()) {
                outcome.removed.push_back(id);
            } else {
                outcome.removal_failures.emplace_back(id, std::move(error));
            }
        }
        if (!outcome.plan.restore_desired_size) {
            return outcome;
        }
        try {
            restore(original_size);
            outcome.restored_size = original_size;
        } catch (const std::exception& ex) {
            outcome.restore_error = ex.what();
            return outcome;
        }
        const auto after = settle_listing(list, window, poll_interval);
        if (after.has_value()) {
            outcome.audit = audit_after_shrink(pre_growth_ids, outcome.final_listing, *after);
        } else {
            outcome.audit_unavailable = true;
        }
    } catch (const std::exception& ex) {
        outcome.restore_error = std::string("rollback aborted: ") + ex.what();
    }
    return outcome;
}

namespace detail {

[[nodiscard]] inline auto label(const listed_member& m) -> std::string {
    std::string out = m.id;
    if (!m.node.empty()) {
        out += " (node " + m.node + ")";
    }
    return out;
}

}  // namespace detail

/// @brief The clause each manager appends to its timeout message, starting
///        with `"rollback: "`.
///
/// Shapes:
/// - `rollback: removed i-0abc (fresh, Pending)`
/// - `rollback: desired size restored to 3; lost member i-0def (node 7)
///   during rollback`
/// - `rollback: removal of i-0abc failed: <error>; desired size left at 4`
///
/// @param grown_size The desired size after the grow call, for the "left
///        at" wording when a removal fails.
[[nodiscard]] inline auto describe(const rollback_outcome& outcome, std::int64_t grown_size)
    -> std::string {
    std::vector<std::string> parts;
    for (const auto& id : outcome.removed) {
        const auto* m = detail::find_member(outcome.final_listing, id);
        std::string part = "removed " + id + " (fresh";
        if (m != nullptr && !m->lifecycle.empty()) {
            part += ", " + m->lifecycle;
        }
        parts.push_back(part + ")");
    }
    for (const auto& [id, error] : outcome.removal_failures) {
        parts.push_back("removal of " + id + " failed: " + error);
    }
    if (!outcome.removal_failures.empty()) {
        parts.push_back(
            "desired size left at " +
            std::to_string(grown_size - static_cast<std::int64_t>(outcome.removed.size())));
    }
    if (outcome.plan.restore_desired_size) {
        if (!outcome.restore_error.empty()) {
            parts.push_back("restoring the desired size failed: " + outcome.restore_error +
                            "; desired size left at " + std::to_string(grown_size));
        } else if (outcome.restored_size.has_value()) {
            parts.push_back("desired size restored to " + std::to_string(*outcome.restored_size));
        }
        if (outcome.audit.has_value()) {
            for (const auto& m : outcome.audit->lost_members) {
                parts.push_back("lost member " + detail::label(m) + " during rollback");
            }
            for (const auto& m : outcome.audit->late_arrivals) {
                parts.push_back("late instance " + m.id +
                                (m.lifecycle.empty() ? std::string{} : " (" + m.lifecycle + ")") +
                                " appeared after the shrink and was left in place");
            }
        } else if (outcome.audit_unavailable) {
            parts.push_back(
                "no listing after the shrink succeeded, so whether it cost a member "
                "is unknown");
        }
    }
    std::string out = "rollback: ";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += "; ";
        }
        out += parts[i];
    }
    if (parts.empty()) {
        out += "nothing to undo";
    }
    return out;
}

}  // namespace kythira::group_rollback
