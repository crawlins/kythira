#pragma once

#include <raft/raft.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <format>
#include <vector>

namespace kythira::chaos {

// Verify at most one leader per term across all nodes.
// Fails the test if two nodes are simultaneously leaders in the same term.
template<typename Types>
void assert_election_safety(const std::vector<kythira::node<Types>*>& nodes) {
    using term_id_type = typename Types::term_id_type;

    std::vector<std::pair<term_id_type, std::size_t>> leaders;  // (term, node_index)

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        auto snap = nodes[i]->debug_state();
        if (snap.is_leader) {
            leaders.emplace_back(snap.current_term, i);
        }
    }

    for (std::size_t i = 0; i < leaders.size(); ++i) {
        for (std::size_t j = i + 1; j < leaders.size(); ++j) {
            if (leaders[i].first == leaders[j].first) {
                BOOST_FAIL(std::format(
                    "election safety violated: nodes {} and {} both claim leadership in term {}",
                    leaders[i].second, leaders[j].second, leaders[i].first));
            }
        }
    }
}

// Verify log matching: if any two nodes share a (term, index) entry the
// command bytes at that index must be identical, and the same must hold for
// all preceding indices.
template<typename Types> void assert_log_matching(const std::vector<kythira::node<Types>*>& nodes) {
    using log_index_type = typename Types::log_index_type;

    if (nodes.empty()) {
        return;
    }

    // Collect snapshots; find min last_applied to limit comparison range.
    std::vector<typename kythira::node<Types>::debug_snapshot> snaps;
    snaps.reserve(nodes.size());
    log_index_type min_applied = std::numeric_limits<log_index_type>::max();
    for (auto* n : nodes) {
        auto s = n->debug_state();
        min_applied = std::min(min_applied, s.last_applied);
        snaps.push_back(std::move(s));
    }
    if (min_applied == std::numeric_limits<log_index_type>::max() || min_applied == 0) {
        return;
    }

    // For each index up to min_applied, if two nodes both have that entry,
    // the term and command must match.
    for (log_index_type idx = 1; idx <= min_applied; ++idx) {
        for (std::size_t i = 0; i < snaps.size(); ++i) {
            for (std::size_t j = i + 1; j < snaps.size(); ++j) {
                const auto& log_i = snaps[i].log;
                const auto& log_j = snaps[j].log;

                // Find entries at this index (log is 1-based, stored in a vector)
                auto find_entry = [idx](const auto& log_span) {
                    for (const auto& e : log_span) {
                        if (e.index() == idx) {
                            return &e;
                        }
                    }
                    return static_cast<const decltype(&log_span[0])>(nullptr);
                };

                auto* ei = find_entry(log_i);
                auto* ej = find_entry(log_j);

                if (ei == nullptr || ej == nullptr) {
                    continue;
                }

                if (ei->term() != ej->term()) {
                    BOOST_FAIL(
                        std::format("log matching violated at index {}: "
                                    "node {} has term {}, node {} has term {}",
                                    idx, i, ei->term(), j, ej->term()));
                }
                if (ei->command() != ej->command()) {
                    BOOST_FAIL(
                        std::format("log matching violated at index {}: "
                                    "node {} and node {} have different command bytes",
                                    idx, i, j));
                }
            }
        }
    }
}

// Verify state machine safety: two nodes that have applied to the same index
// must have applied the same command bytes.  We derive this from the log,
// since the state machine safety follows directly from log matching.
template<typename Types>
void assert_state_machine_safety(const std::vector<kythira::node<Types>*>& nodes,
                                 std::uint64_t up_to_index) {
    using log_index_type = typename Types::log_index_type;

    std::vector<typename kythira::node<Types>::debug_snapshot> snaps;
    snaps.reserve(nodes.size());
    for (auto* n : nodes) {
        snaps.push_back(n->debug_state());
    }

    for (log_index_type idx = 1; idx <= static_cast<log_index_type>(up_to_index); ++idx) {
        for (std::size_t i = 0; i < snaps.size(); ++i) {
            if (snaps[i].last_applied < idx) {
                continue;
            }
            for (std::size_t j = i + 1; j < snaps.size(); ++j) {
                if (snaps[j].last_applied < idx) {
                    continue;
                }

                auto find_entry = [idx](const auto& log_span) {
                    for (const auto& e : log_span) {
                        if (e.index() == idx) {
                            return &e;
                        }
                    }
                    return static_cast<const decltype(&log_span[0])>(nullptr);
                };

                auto* ei = find_entry(snaps[i].log);
                auto* ej = find_entry(snaps[j].log);

                if (ei == nullptr || ej == nullptr) {
                    continue;
                }

                if (ei->command() != ej->command()) {
                    BOOST_FAIL(
                        std::format("state machine safety violated at index {}: "
                                    "node {} and node {} applied different commands",
                                    idx, i, j));
                }
            }
        }
    }
}

}  // namespace kythira::chaos
