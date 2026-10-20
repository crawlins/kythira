// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file legacy_layout_reader.hpp
/// @brief The parse rules an **older** binary applies to an object-store
///        prefix, frozen — so a test can show that every body the new layouts
///        write is refused by them.
///
/// `.kiro/specs/object-store-batched-layout/` rests its rollback argument on one
/// fact: every new format lives in a key an older binary already reads
/// (`<prefix>/term`, `<prefix>/log/<index>`), in a body that older binary
/// already refuses to parse. That cannot be tested by running an older binary
/// in CI, so the predicates the claim rests on are copied here instead
/// (Requirement 15.5, design Property 3):
///
///   * the engine's `parse_unsigned` (`<prefix>/term`, `<prefix>/voted_for`)
///     and `json_to_entry` (`<prefix>/log/<index>`), with the strict base64
///     decoder `json_to_entry` calls;
///   * the backup tool's `parse_u64` (`create` reading `term`), the parse
///     half of `check_entry_term` (`verify` reading a log entry's term), and
///     `verify`'s manifest version gate (added with batched-layout task 5,
///     copied from the same commit).
///
/// **Copied verbatim from commit 92a1825e5a7456014afead0941684b153d16fca1**
/// (`include/raft/object_store_persistence.hpp` and
/// `include/raft/object_store_backup.hpp` at that commit). Only the wrapping
/// changed: static members became free functions over the default
/// `std::uint64_t` instantiation, `check_entry_term`'s parse is returned as a
/// value instead of being pushed into a `verification_report`, and the throw
/// messages keep their original prefixes.
///
/// **Never edit these to match the engine.** They are not a second
/// implementation to keep in sync; they are a record of what binaries that have
/// already shipped do. If the engine's parser changes, this file must not, and a
/// case that starts failing here means a new body has become readable by an
/// older binary — which is the silent rollback this spec exists to prevent, not
/// a test to update.

#include <raft/types.hpp>

#include <boost/json.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace kythira::legacy_layout {

// ── object_store_persistence_engine, commit 92a1825 ──────────────────────────

/// Strict decimal parse: the *whole* body must be digits. `std::stoull`
/// would happily read "12" out of "12garbage"; a persistence engine that
/// does that turns a truncated upload into a plausible-looking term.
inline auto parse_unsigned(std::string_view body, const std::string& key) -> unsigned long long {
    // Tolerate a single trailing newline: an operator repairing state with
    // a shell writes one whether they mean to or not.
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.remove_suffix(1);
    }
    unsigned long long value = 0;
    const char* first = body.data();
    const char* last = body.data() + body.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || body.empty()) {
        throw std::runtime_error("object_store_persistence_engine: corrupt object " + key +
                                 ": expected a decimal number, got \"" + std::string(body) + "\"");
    }
    return value;
}

inline constexpr std::string_view k_b64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Decodes strictly: a character outside the alphabet throws rather than
/// being skipped the way the file engine skips it. Skipping is how a
/// corrupted command silently decodes to *some* byte string, which the
/// state machine would then apply.
inline auto base64_to_bytes(const std::string& in) -> std::vector<std::byte> {
    static const auto tbl = [] {
        std::array<std::int8_t, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i) {
            t[static_cast<std::uint8_t>(k_b64[static_cast<std::size_t>(i)])] =
                static_cast<std::int8_t>(i);
        }
        return t;
    }();
    std::vector<std::byte> out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t v = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=') {
            break;
        }
        const std::int8_t b = tbl[static_cast<std::uint8_t>(c)];
        if (b < 0) {
            throw std::runtime_error("base64: invalid character in encoded command");
        }
        v = (v << 6U) | static_cast<std::uint32_t>(b);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return out;
}

using log_entry_t = log_entry<>;
using TermId = std::uint64_t;
using LogIndex = std::uint64_t;

inline auto json_to_entry(const std::string& s) -> log_entry_t {
    auto obj = boost::json::parse(s).as_object();
    log_entry_t e;
    e._term = static_cast<TermId>(obj.at("term").as_int64());
    e._index = static_cast<LogIndex>(obj.at("index").as_int64());
    e._command = base64_to_bytes(std::string(obj.at("command").as_string()));
    e._type = obj.contains("type") ? static_cast<entry_type>(obj.at("type").as_int64())
                                   : entry_type::normal;
    return e;
}

// ── object_store_backup, commit 92a1825 ──────────────────────────────────────

/// Parses a decimal that must consume the whole field, naming @p key on
/// failure. The engine's `term` and `voted_for` objects are bare decimals.
[[nodiscard]] inline auto parse_u64(std::string_view text, const std::string& key)
    -> std::uint64_t {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos) {
        throw std::runtime_error("object_store_backup: corrupt object " + key + ": expected a" +
                                 " decimal integer, got \"" + std::string(text) + "\"");
    }
    return std::stoull(std::string(text));
}

/// The parse half of `object_store_backup::check_entry_term`: the entry's term,
/// or the exception that made `verify` report the object as
/// `unreadable_entry`. Rethrown rather than reported, so a case can assert
/// "refused" directly.
inline auto check_entry_term_parse(const std::string& body) -> std::uint64_t {
    std::uint64_t entry_term = 0;
    const auto parsed = boost::json::parse(body);
    entry_term =
        static_cast<std::uint64_t>(parsed.as_object().at("term").to_number<std::int64_t>());
    return entry_term;
}

/// `object_store_backup_detail::k_manifest_format_version`.
inline constexpr int k_manifest_format_version = 1;

/// The version gate at the top of `object_store_backup::verify`, over the
/// field as `parse_manifest` reads it. The older tool reports
/// `manifest_version` for any other value and stops, and its `restore_clone`
/// and `restore_seed` run that `verify` first and abort on it.
inline auto manifest_version_gate(const std::string& manifest_body) -> bool {
    const auto parsed = boost::json::parse(manifest_body);
    const auto& obj = parsed.as_object();
    const auto format_version =
        static_cast<int>(obj.at("format_version").to_number<std::int64_t>());
    return format_version == k_manifest_format_version;
}

// ── Verdicts, for the cases ──────────────────────────────────────────────────

/// Whether an older engine refuses `body` at `<prefix>/term`.
[[nodiscard]] inline auto engine_refuses_term(const std::string& body) -> bool {
    try {
        static_cast<void>(parse_unsigned(body, "term"));
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

/// Whether an older backup tool's `create` refuses `body` at `<prefix>/term`.
[[nodiscard]] inline auto backup_refuses_term(const std::string& body) -> bool {
    try {
        static_cast<void>(parse_u64(body, "term"));
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

/// Whether an older backup tool's `verify` — and so its restores — refuses a
/// backup whose `manifest.json` is `manifest_body`.
[[nodiscard]] inline auto backup_refuses_manifest(const std::string& manifest_body) -> bool {
    try {
        return !manifest_version_gate(manifest_body);
    } catch (const std::exception&) {
        return true;
    }
}

/// Whether an older engine refuses `body` at `<prefix>/log/<index>`.
[[nodiscard]] inline auto engine_refuses_log_object(const std::string& body) -> bool {
    try {
        static_cast<void>(json_to_entry(body));
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

/// Whether an older backup tool's `verify` reports `body` at
/// `<prefix>/log/<index>` as `unreadable_entry`.
[[nodiscard]] inline auto backup_refuses_log_object(const std::string& body) -> bool {
    try {
        static_cast<void>(check_entry_term_parse(body));
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace kythira::legacy_layout
