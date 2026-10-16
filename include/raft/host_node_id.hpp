// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file host_node_id.hpp
/// @brief The node id type of the host binaries in `cmd/`, and the one parser
///        every `--node-id`, `NODE_ID` and peer list goes through
///        (`.kiro/specs/cloud-composite-node-ids/` Requirement 10).
///
/// The Kconfig choice `KYTHIRA_NODE_ID_KIND` picks the type at build time:
///
/// - **numeric** (the default): `std::uint64_t`. A node id is a decimal
///   integer, exactly as before textual ids existed.
/// - **text**: `std::string` holding canonical text. One binary must accept
///   ids from any provider, which is why it is the string and not one
///   composite type per provider (design, "Key design decision").
///
/// `genconfig.py` writes the choice into `generated/kythira/autoconf.hpp` as
/// `KYTHIRA_NODE_ID_TEXT`, and CMake defines that macro on the targets that
/// link `kythira_host_node_id`. It is not taken from `autoconf.hpp` here:
/// that header also defines every feature macro Kconfig asked for, whether or
/// not CMake found the feature, and including it would turn those on in
/// translation units that cannot link them.

#include <raft/composite_node_id.hpp>
#include <raft/self_node_id.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kythira {

#if defined(KYTHIRA_NODE_ID_TEXT)
/// The host binaries' node id: canonical text (`KYTHIRA_NODE_ID_KIND=text`).
using host_node_id = std::string;
#else
/// The host binaries' node id: a number (`KYTHIRA_NODE_ID_KIND=numeric`).
using host_node_id = std::uint64_t;
#endif

/// Whether the host binaries were built with textual node ids.
inline constexpr bool host_node_id_is_text = node_id_traits<host_node_id>::is_textual;

/// The configured `KYTHIRA_NODE_ID_KIND`, for messages and `--help`.
inline constexpr std::string_view host_node_id_kind = host_node_id_is_text ? "text" : "numeric";

/// A node id type a host binary can be built with.
template<typename NodeId>
concept host_node_id_type =
    std::same_as<NodeId, std::uint64_t> || std::same_as<NodeId, std::string>;

namespace host_node_id_detail {

template<typename... Ids> struct composite_list {};
using all_composites =
    composite_list<aws_ec2_node_id, alibaba_ecs_node_id, azure_vm_node_id, gcp_instance_node_id,
                   oci_instance_node_id, docker_container_node_id>;

template<typename... Ids>
auto parse_any(std::string_view text, composite_list<Ids...> /*unused*/)
    -> std::optional<std::pair<std::string, std::string>> {
    std::optional<std::pair<std::string, std::string>> out;
    auto try_one = [&]<typename Id>() {
        if (out) {
            return;
        }
        if (auto id = Id::parse(text)) {
            out.emplace(std::string(id->scope()), std::string(id->native()));
        }
    };
    (try_one.template operator()<Ids>(), ...);
    return out;
}

inline auto all_digits(std::string_view text) -> bool {
    return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace host_node_id_detail

/// @brief The scope and native id of canonical text of any of the six
///        provider types, or nullopt when `text` is none of them.
[[nodiscard]] inline auto parse_any_composite_node_id(std::string_view text)
    -> std::optional<std::pair<std::string, std::string>> {
    return host_node_id_detail::parse_any(text, host_node_id_detail::all_composites{});
}

/// @brief A node id from configuration text (a peer list entry, or a local
///        id that is not `metadata:`).
///
/// Numeric `NodeId`: a decimal integer that fits, nothing else
/// (`node_id_traits`' strict parser). Text `NodeId`: canonical text of any
/// provider type, or a plain decimal integer, which is normalised (`007` is
/// `7`) so two spellings of one number cannot name two nodes. Anything else
/// is refused: an id that is neither is a typo, and accepting it would start
/// a node no manager can map to a resource.
template<host_node_id_type NodeId>
[[nodiscard]] auto parse_node_id_text(std::string_view text) -> node_id_result<NodeId> {
    auto refuse = [&](std::string_view expected) {
        return node_id_result<NodeId>::failure(
            "'" + std::string(text) + "' is not a node id: expected " + std::string(expected));
    };
    if constexpr (std::same_as<NodeId, std::uint64_t>) {
        if (auto id = node_id_traits<std::uint64_t>::from_text(text)) {
            return *id;
        }
        return refuse(
            "a decimal integer (this binary was built with KYTHIRA_NODE_ID_KIND="
            "numeric)");
    } else {
        if (host_node_id_detail::all_digits(text)) {
            if (auto id = node_id_traits<std::uint64_t>::from_text(text)) {
                return std::to_string(*id);
            }
            return refuse("a decimal integer below 2^64, or canonical composite text");
        }
        if (parse_any_composite_node_id(text)) {
            return std::string(text);
        }
        return refuse(
            "canonical composite text (<provider>:<scope>:<native>, e.g. "
            "aws-ec2:us-east-1:i-0123456789abcdef0) or a decimal integer");
    }
}

/// The prefix that asks for the local id from instance metadata.
inline constexpr std::string_view k_metadata_node_id_prefix = "metadata:";

/// @brief The local node id: `parse_node_id_text`'s forms, or
///        `metadata:<provider>` (Requirement 6.2), resolved now through
///        `self_node_id_text` with `options`.
///
/// The metadata form is the only one that touches the network, and only
/// when given. A numeric build refuses it: no cloud id fits a number, and
/// silently hashing one into a number would be a new allocator with none of
/// the guarantees the managers' own allocation has.
template<host_node_id_type NodeId>
[[nodiscard]] auto parse_local_node_id(std::string_view text,
                                       const self_node_id_options& options = {})
    -> node_id_result<NodeId> {
    if (!text.starts_with(k_metadata_node_id_prefix)) {
        return parse_node_id_text<NodeId>(text);
    }
    const auto provider = text.substr(k_metadata_node_id_prefix.size());
    const auto source = parse_node_id_source(provider);
    if (!source) {
        return node_id_result<NodeId>::failure(
            "'" + std::string(text) +
            "': unknown metadata provider; expected one of aws, alibaba, azure, gcp, oci");
    }
    if constexpr (std::same_as<NodeId, std::uint64_t>) {
        return node_id_result<NodeId>::failure(
            "'" + std::string(text) +
            "' needs a binary built with KYTHIRA_NODE_ID_KIND=text: a cloud instance id does "
            "not fit a numeric node id");
    } else {
        return self_node_id_text(*source, options);
    }
}

/// @brief `parse_local_node_id` for `host_node_id`.
[[nodiscard]] inline auto parse_host_node_id(std::string_view text,
                                             const self_node_id_options& options = {})
    -> node_id_result<host_node_id> {
    return parse_local_node_id<host_node_id>(text, options);
}

/// @brief `parse_node_id_text` for `host_node_id`: a peer's id, which is
///        never looked up from metadata.
[[nodiscard]] inline auto parse_host_peer_node_id(std::string_view text)
    -> node_id_result<host_node_id> {
    return parse_node_id_text<host_node_id>(text);
}

/// @brief A small integer as a `host_node_id`: itself, or its decimal text.
///        For defaults and generated test clusters.
[[nodiscard]] inline auto host_node_id_from_number(std::uint64_t n) -> host_node_id {
    // Not `if constexpr`: outside a template both branches are compiled.
#if defined(KYTHIRA_NODE_ID_TEXT)
    return std::to_string(n);
#else
    return n;
#endif
}

/// @brief `host_node_id` as text: decimal, or the canonical text itself.
[[nodiscard]] inline auto host_node_id_text(const host_node_id& id) -> std::string {
    return node_id_traits<host_node_id>::to_text(id);
}

/// @brief A node id as a JSON value: a number for a numeric id, a string for
///        a textual one, so a textual id never reaches a JSON consumer as a
///        bare token (Requirement 10.4).
template<typename NodeId> [[nodiscard]] auto node_id_json(const NodeId& id) -> std::string {
    if constexpr (!node_id_traits<NodeId>::is_textual) {
        return std::to_string(id);
    } else {
        static constexpr std::string_view k_hex = "0123456789abcdef";
        std::string out = "\"";
        for (const char ch : node_id_traits<NodeId>::to_text(id)) {
            const auto c = static_cast<unsigned char>(ch);
            if (ch == '"' || ch == '\\') {
                out.push_back('\\');
                out.push_back(ch);
            } else if (c < 0x20) {
                out += "\\u00";
                out.push_back(k_hex[c >> 4]);
                out.push_back(k_hex[c & 0x0f]);
            } else {
                out.push_back(ch);
            }
        }
        out.push_back('"');
        return out;
    }
}

/// @brief Splits a leading node id off a `:`-separated list entry such as
///        `id:host:port`, returning the id's text and what follows its `:`.
///
/// A decimal id is the first field. Anything else is taken as canonical
/// composite text, which is exactly three fields because every `:` inside a
/// segment is percent-encoded. Nullopt when there are not enough fields.
[[nodiscard]] inline auto split_leading_node_id(std::string_view entry)
    -> std::optional<std::pair<std::string_view, std::string_view>> {
    auto first = entry.find(':');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t end = first;
    if (!host_node_id_detail::all_digits(entry.substr(0, first))) {
        const auto second = entry.find(':', first + 1);
        if (second == std::string_view::npos) {
            return std::nullopt;
        }
        end = entry.find(':', second + 1);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
    }
    return std::pair{entry.substr(0, end), entry.substr(end + 1)};
}

}  // namespace kythira
