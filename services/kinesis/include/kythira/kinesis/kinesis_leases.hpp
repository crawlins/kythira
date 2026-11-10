// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_leases.hpp
/// @brief Lease records for the native KCL lease layer
///        (`.kiro/specs/kinesis-compatible-streams/` task 9, design "Native
///        KCL lease layer", Requirement 15.1).
///
/// A lease is a small versioned document: a map from top-level JSON member
/// names to the members' JSON text. The state machine never parses the
/// JSON. It stores the text the API layer validated and hands it back, so
/// `apply()` stays cheap and every replica stores the same bytes.
///
/// A lease's version is the Raft log index of the entry that last wrote it.
/// Versions only grow, and a lease deleted and created again never repeats
/// an old version, so a client holding a stale version cannot overwrite a
/// newer lease that happens to carry the same number.
///
/// Leases live in the catalog range, so the catalog group orders every
/// write and read of one application's leases:
///
/// | Key | Holds |
/// |---|---|
/// | `\0meta/kinesis/leases/<account>/<application>/<lease key>` | a `lease_record` |
///
/// Applications are per account, like the DynamoDB lease table KCL creates
/// on AWS, whose name is the application name.

#include <kythira/kinesis/kinesis_catalog.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

/// Where the Lease API (`kinesis_lease_api.hpp`) is served.
inline constexpr std::string_view k_lease_path_prefix = "/v1/leases/";

/// The most bytes a lease's fields may take (names plus JSON text).
inline constexpr std::size_t k_max_lease_bytes = 64 * 1024;
/// The most leases one list page returns.
inline constexpr std::size_t k_max_lease_page = 1000;
/// Lease keys are at most this many bytes.
inline constexpr std::size_t k_max_lease_key_bytes = 1024;

struct lease_record {
    std::uint64_t _version = 0;
    std::map<std::string, std::string> _fields{};
    friend auto operator==(const lease_record&, const lease_record&) -> bool = default;
};

/// An application name: what KCL would name its DynamoDB lease table, so
/// 3 to 255 of `[A-Za-z0-9_.-]`.
[[nodiscard]] inline auto valid_application(std::string_view s) -> bool {
    if (s.size() < 3 || s.size() > 255) {
        return false;
    }
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// A lease key: 1 to 1024 bytes, no control characters. KCL's keys are
/// shard ids, or `account:stream:epoch:shardId-…` for multi-stream
/// applications.
[[nodiscard]] inline auto valid_lease_key(std::string_view s) -> bool {
    if (s.empty() || s.size() > k_max_lease_key_bytes) {
        return false;
    }
    for (const char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline auto leases_prefix(std::string_view account, std::string_view application)
    -> std::string {
    return catalog_prefix() + "leases/" + std::string(account) + "/" + std::string(application) +
           "/";
}

[[nodiscard]] inline auto lease_storage_key(std::string_view account, std::string_view application,
                                            std::string_view key) -> std::string {
    return leases_prefix(account, application) + std::string(key);
}

[[nodiscard]] inline auto lease_bytes(const std::map<std::string, std::string>& fields)
    -> std::size_t {
    std::size_t n = 0;
    for (const auto& [k, v] : fields) {
        n += k.size() + v.size();
    }
    return n;
}

inline auto encode(codec::writer& w, const lease_record& l) -> void {
    w.u64(l._version).u32(static_cast<std::uint32_t>(l._fields.size()));
    for (const auto& [k, v] : l._fields) {
        w.str(k).str(v);
    }
}

[[nodiscard]] inline auto decode_lease_record(codec::reader& r) -> lease_record {
    lease_record l;
    l._version = r.u64();
    const auto n = r.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        auto k = r.str();
        l._fields[std::move(k)] = r.str();
    }
    return l;
}

/// One lease in a read's answer.
struct lease_entry {
    std::string _key{};
    lease_record _lease{};
    friend auto operator==(const lease_entry&, const lease_entry&) -> bool = default;
};

/// What a lease read returns: the leases found, and for a list whether
/// more follow the last one.
struct lease_page {
    std::vector<lease_entry> _leases{};
    bool _more = false;
    friend auto operator==(const lease_page&, const lease_page&) -> bool = default;
};

[[nodiscard]] inline auto encode_lease_page(const lease_page& p) -> std::vector<std::byte> {
    codec::writer w;
    w.u8(p._more ? 1 : 0).u32(static_cast<std::uint32_t>(p._leases.size()));
    for (const auto& e : p._leases) {
        w.str(e._key);
        encode(w, e._lease);
    }
    return w.take();
}

[[nodiscard]] inline auto decode_lease_page(std::span<const std::byte> bytes) -> lease_page {
    codec::reader r(bytes);
    lease_page p;
    p._more = r.boolean();
    const auto n = r.u32();
    p._leases.reserve(std::min<std::size_t>(n, r.remaining() / 16));
    for (std::uint32_t i = 0; i < n; ++i) {
        lease_entry e;
        e._key = r.str();
        e._lease = decode_lease_record(r);
        p._leases.push_back(std::move(e));
    }
    r.expect_end("lease page");
    return p;
}

}  // namespace kythira::kinesis
