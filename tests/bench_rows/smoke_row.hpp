// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file smoke_row.hpp
/// @brief The per-transport liveness check, as a template each
///        `tests/bench_rows/*.cpp` instantiates for its own pair and nobody
///        else's.
///
/// This is a header in `tests/bench_rows/` rather than beside the runners
/// declaration on purpose: including it instantiates `kv_cluster<Transport>`,
/// which is most of what the split exists to keep out of the two consumers'
/// translation units. Only the three files that define a `smoke_*` runner
/// include it.
///
/// It reports through `row_observer` instead of Boost.Test macros. The suite
/// maps `_require` to `BOOST_REQUIRE_MESSAGE` and `_check` to
/// `BOOST_CHECK_MESSAGE`, so its behaviour is unchanged from when this body
/// lived in `multi_raft_http_benchmark_test.cpp`; the report binary, which
/// links the same library, gets a thrown exception that abandons the row.

#include "multi_raft_benchmark_rows.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace kythira::testing::rows::detail {

/// @brief One PUT and one GET per shard, over whatever socket `Transport` uses.
template<typename Transport> auto smoke(const row_observer& observer) -> void {
    kv_cluster<Transport> cluster{standard_cluster_options()};

    {
        std::ostringstream why;
        why << Transport::name() << ": no leader on every shard within budget";
        observer._require(cluster.await_all_leaders(k_election_budget), why.str());
    }

    operation_tally tally;
    const auto options = cluster.options();

    // One key per shard, taken from the middle of each shard's own range so a
    // boundary bug cannot make the choice accidentally correct.
    for (std::size_t g = 0; g < options._groups; ++g) {
        const auto n = options._key_count * (2 * g + 1) / (2 * options._groups);
        const auto key = kv_key(n);
        const auto value = kv_value(n, 64);

        auto put_latency = cluster.run_command(key, kv_put(key, value), k_operation_timeout, tally);
        {
            std::ostringstream why;
            why << Transport::name() << ": PUT of '" << key << "' did not commit";
            observer._require(put_latency.has_value(), why.str());
        }

        std::vector<std::byte> read_back;
        auto get_latency =
            cluster.run_command(key, kv_get(key), k_operation_timeout, tally, &read_back);
        {
            std::ostringstream why;
            why << Transport::name() << ": GET of '" << key << "' did not commit";
            observer._require(get_latency.has_value(), why.str());
        }

        std::string observed;
        observed.reserve(read_back.size());
        for (auto b : read_back) {
            observed.push_back(static_cast<char>(b));
        }
        {
            std::ostringstream why;
            why << Transport::name() << ": '" << key << "' read back " << observed.size()
                << " bytes, expected " << value.size();
            observer._check(observed == value, why.str());
        }
    }

    const auto problem = cluster.tiling_problem();
    {
        std::ostringstream why;
        why << Transport::name() << ": tiling broken: " << problem.value_or("");
        observer._check(!problem.has_value(), why.str());
    }

    {
        std::ostringstream text;
        text << "  " << Transport::name() << ": " << tally._completed << "/" << tally._offered
             << " operations committed over a real socket";
        observer._message(text.str());
    }
}

}  // namespace kythira::testing::rows::detail
