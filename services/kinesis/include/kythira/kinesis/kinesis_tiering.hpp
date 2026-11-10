// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_tiering.hpp
/// @brief Sealed segments in object storage (`.kiro/specs/kinesis-compatible-streams/`
///        task 10, Requirement 11.3).
///
/// A segment's object key is a function of its descriptor alone:
///
///     <prefix>kinesis/segments/<routing key hex>/<first counter>-<SHA-256 hex>
///
/// with the first counter in twenty decimal digits, so a listing sorts in
/// stream order.
///
/// so the stream state records only *that* a segment is tiered
/// (`shard_state::_tiered`), and any node finds it with no lookup. With the
/// SHA-256 in the name an object only ever has one content, so a retried
/// upload (a leader that died before proposing the tier entry, and its
/// successor uploading again) writes the same bytes, and the fetch checks
/// them against the descriptor anyway.
///
/// `tier_store` is the five-operation object store (`key_object_store.hpp`)
/// with the bucket and prefix bound, type-erased so the daemon picks S3,
/// GCS, Azure Blob, OCI or OSS at startup and the state machine and
/// maintenance pass stay non-template.
///
/// Fetch-back: a read that needs a tiered segment fetches it through the
/// node's `segment_fetcher`, which tries the object store after the peers;
/// the bytes are installed into the local store (size, SHA-256 and every
/// record checked by `install_segment`) and read like any local segment.
/// `fetched_segments` remembers when, so the maintenance pass drops the
/// copy again once no read has wanted it for a while.

#include <raft/key_object_store.hpp>
#include <raft/record_segment_store.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::kinesis {

/// An object store with the bucket and key prefix bound.
struct tier_store {
    std::function<void(const std::string& key, std::string_view bytes)> _put;
    std::function<std::optional<std::string>(const std::string& key)> _get;
    std::function<void(const std::string& key)> _remove;
    /// Prepended to every object key; empty or ending in `/`.
    std::string _prefix;

    [[nodiscard]] auto configured() const -> bool { return static_cast<bool>(_put); }
};

/// `store` (shared, since the functions outlive the call) under `bucket`,
/// with keys under `prefix`.
template<key_object_store Store>
[[nodiscard]] auto make_tier_store(std::shared_ptr<const Store> store, std::string bucket,
                                   std::string prefix) -> tier_store {
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }
    tier_store t;
    t._prefix = std::move(prefix);
    t._put = [store, bucket](const std::string& key, std::string_view bytes) {
        (void)store->put_object(bucket, key, bytes);
    };
    t._get = [store, bucket](const std::string& key) -> std::optional<std::string> {
        auto got = store->get_object(bucket, key);
        if (!got) {
            return std::nullopt;
        }
        return std::move(got->body);
    };
    t._remove = [store, bucket](const std::string& key) { store->delete_object(bucket, key); };
    return t;
}

[[nodiscard]] inline auto hex_of(std::span<const std::byte> bytes) -> std::string {
    static constexpr char k_digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes) {
        const auto v = static_cast<unsigned>(b);
        out.push_back(k_digits[v >> 4U]);
        out.push_back(k_digits[v & 0xfU]);
    }
    return out;
}

/// Where segment `d` lives in `prefix`.
[[nodiscard]] inline auto segment_object_key(std::string_view prefix,
                                             const record_segment_descriptor& d) -> std::string {
    std::array<char, 24> first{};
    std::snprintf(first.data(), first.size(), "%020llu",
                  static_cast<unsigned long long>(d._first_seq));
    return std::string(prefix) + "kinesis/segments/" +
           hex_of(std::as_bytes(std::span{d._stream.data(), d._stream.size()})) + "/" +
           first.data() + "-" + hex_of(d._sha256);
}

/// When this node last installed a tiered segment for a read, so the
/// maintenance pass keeps that copy while reads still want it.
class fetched_segments {
public:
    void note(const std::string& stream, std::uint64_t first, std::int64_t now_ms) {
        const std::scoped_lock lock(_mutex);
        _when[{stream, first}] = now_ms;
    }

    /// Whether the copy was fetched within `keep_ms` of `now_ms`. A copy
    /// this node did not fetch since it started counts as stale.
    [[nodiscard]] auto recent(const std::string& stream, std::uint64_t first, std::int64_t now_ms,
                              std::int64_t keep_ms) const -> bool {
        const std::scoped_lock lock(_mutex);
        const auto it = _when.find({stream, first});
        return it != _when.end() && now_ms - it->second < keep_ms;
    }

    void forget(const std::string& stream, std::uint64_t first) {
        const std::scoped_lock lock(_mutex);
        _when.erase({stream, first});
    }

private:
    mutable std::mutex _mutex;
    std::map<std::pair<std::string, std::uint64_t>, std::int64_t> _when;
};

/// Fetch `d` from the object store into `store`. False when the object is
/// absent or its bytes do not match `d` (never installed then). Throws
/// what the object store throws for a transport failure.
inline auto fetch_from_tier(record_segment_store& store, const tier_store& tier,
                            const record_segment_descriptor& d) -> bool {
    if (!tier.configured()) {
        return false;
    }
    auto body = tier._get(segment_object_key(tier._prefix, d));
    if (!body) {
        return false;
    }
    try {
        store.install_segment(d, std::as_bytes(std::span{body->data(), body->size()}));
    } catch (const std::invalid_argument&) {
        return false;
    }
    return true;
}

}  // namespace kythira::kinesis
