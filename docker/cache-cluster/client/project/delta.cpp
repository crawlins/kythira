// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "probe.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace probe {

// Enough standard-library code that the compile takes a noticeable moment,
// so a hit is visibly cheaper than a miss.
std::uint64_t delta(std::uint64_t seed) {
    std::vector<std::uint64_t> v(64);
    std::iota(v.begin(), v.end(), seed + KYTHIRA_SALT);
    std::map<std::string, std::uint64_t> m;
    for (auto x : v) {
        m[std::to_string(x * 11U)] = x;
    }
    std::sort(v.begin(), v.end(), [](auto a, auto b) { return (a * 11U) % 97U < (b * 11U) % 97U; });
    return std::accumulate(v.begin(), v.end(), m.size());
}

}  // namespace probe
