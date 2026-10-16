// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file generated_node_name.hpp
/// @brief The resource names the name-chosen quorum managers (Azure VM, GCP
///        Compute, Docker) generate before a create when the node id is that
///        name (composite and string mode; spec cloud-composite-node-ids,
///        Requirement 5.3).
///
/// The shape is `kythira-<cluster>-<suffix>`, where the suffix is eight
/// random characters of lower-case RFC 4648 base32. Lower case and digits
/// are valid in every provider's name (an RFC 1035 label on GCP, an Azure VM
/// name, which Azure compares case-insensitively, a Docker container name and
/// the DNS label its peers dial), so the suffix never needs escaping. Forty
/// random bits make a collision within one cluster negligible, and the
/// managers create with "only if absent" semantics and retry a fresh suffix
/// if one does happen.

#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kythira {

/// Characters in a generated name's random suffix.
inline constexpr std::size_t generated_name_suffix_length = 8;

/// Lower-case RFC 4648 base32.
inline constexpr std::string_view generated_name_alphabet = "abcdefghijklmnopqrstuvwxyz234567";

/// @brief `generated_name_suffix_length` random base32 characters, drawn from
///        `std::random_device`.
inline auto random_name_suffix() -> std::string {
    std::random_device rd;
    std::uint64_t bits = (static_cast<std::uint64_t>(rd()) << 32U) | rd();
    std::string out;
    out.reserve(generated_name_suffix_length);
    for (std::size_t i = 0; i < generated_name_suffix_length; ++i) {
        out.push_back(generated_name_alphabet[bits & 31U]);
        bits >>= 5U;
    }
    return out;
}

/// @brief `kythira-<cluster>-<suffix>`, at most @p max_length characters.
///
/// When the whole name would not fit, the cluster part is cut short rather
/// than the suffix: the suffix is what keeps two nodes apart, while the
/// cluster part is only a human-readable hint (the managers find a node by its
/// full name, never by prefix). Any `-`, `.` or `_` left at the end of a cut
/// cluster part is dropped as well, so the name never gains a doubled
/// separator; a cluster part cut to nothing yields `kythira-<suffix>`.
///
/// @throws std::invalid_argument if @p max_length cannot hold even
///         `kythira-<suffix>`.
inline auto generated_node_name(std::string_view cluster, std::string_view suffix,
                                std::size_t max_length) -> std::string {
    constexpr std::string_view prefix = "kythira-";
    if (prefix.size() + suffix.size() > max_length) {
        throw std::invalid_argument("generated_node_name: " + std::to_string(max_length) +
                                    " characters cannot hold a generated name");
    }
    // Room for the cluster part and the '-' that follows it.
    const std::size_t room = max_length - prefix.size() - suffix.size();
    std::string_view part = cluster;
    if (part.size() + 1 > room) {
        part = part.substr(0, room == 0 ? 0 : room - 1);
        while (!part.empty() && (part.back() == '-' || part.back() == '.' || part.back() == '_')) {
            part.remove_suffix(1);
        }
    }
    std::string name(prefix);
    if (!part.empty()) {
        name.append(part);
        name.push_back('-');
    }
    name.append(suffix);
    return name;
}

}  // namespace kythira
