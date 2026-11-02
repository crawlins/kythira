// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "probe.hpp"

#include <cstdio>

int main() {
    const auto sum = probe::alpha(1) + probe::beta(2) + probe::gamma(3) + probe::delta(4);
    std::printf("%llu\n", static_cast<unsigned long long>(sum));
    return sum == 0 ? 1 : 0;
}
