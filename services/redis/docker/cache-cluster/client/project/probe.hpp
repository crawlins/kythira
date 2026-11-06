// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#ifndef KYTHIRA_SALT
#define KYTHIRA_SALT 0
#endif

namespace probe {

std::uint64_t alpha(std::uint64_t seed);
std::uint64_t beta(std::uint64_t seed);
std::uint64_t gamma(std::uint64_t seed);
std::uint64_t delta(std::uint64_t seed);

}  // namespace probe
