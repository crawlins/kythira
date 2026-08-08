#pragma once

/// @file peer_capability_cache.hpp
/// @brief Remembers which media type each peer last answered in, so a client
///        stops guessing after the first successful exchange.
///
/// Shared by `cpp_httplib_client` and `boost_beast_client` rather than
/// reimplemented per transport, for the same reason `parse_accept_header` is
/// shared: two copies of a caching rule drift, and the drift is invisible until
/// a peer happens to use the transport with the stale copy.
///
/// The cache is an *optimisation, not a protocol*. Every request still
/// advertises the client's full `Accept` list, so a stale or missing entry
/// costs at most one extra round of the peer choosing again — it can never make
/// a request fail. That is what lets this have no TTL and no invalidation: a
/// peer that changes its formats has its entry corrected by its next successful
/// response, and in the meantime its own `Accept`-driven choice still wins.

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kythira {

/// @brief Per-target record of the media type a peer last successfully used.
/// @tparam Key Target identifier — `std::uint64_t` node id for ordinary RPCs,
///         or an address type for bootstrap calls that predate having a node id.
///
/// Carries its own mutex rather than borrowing the client's. The design note
/// suggested reusing the connection-map mutex, but `send_rpc` consults this
/// cache on a path that has already gone through `get_or_create_client()` —
/// which takes that mutex — and `std::mutex` is not recursive. A separate lock
/// removes a deadlock that would otherwise depend on call ordering nobody would
/// think to re-check. The critical sections here are a map lookup and a string
/// copy, so the extra lock is not contended in any way that matters.
template<typename Key = std::uint64_t> class peer_capability_cache {
public:
    /// @brief The media type @p key last answered in, if one was recorded.
    [[nodiscard]] auto get(const Key& key) const -> std::optional<std::string> {
        const std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(key);
        if (it == _entries.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// @brief Record that @p key successfully answered in @p media_type.
    ///
    /// Only ever called after a response has decoded cleanly. Recording on a
    /// failure would cache the very media type that just did not work, and the
    /// next request would repeat the mistake with more confidence.
    auto record(const Key& key, std::string media_type) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _entries[key] = std::move(media_type);
    }

    /// @brief Drop @p key's entry, if any. Used when a peer's connection is
    ///        retired, so a recycled node id cannot inherit a stale format.
    auto forget(const Key& key) -> void {
        const std::lock_guard<std::mutex> lock(_mutex);
        _entries.erase(key);
    }

    [[nodiscard]] auto size() const -> std::size_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _entries.size();
    }

private:
    mutable std::mutex _mutex;
    std::unordered_map<Key, std::string> _entries;
};

/// @brief Choose the media type to send to @p target.
///
/// The cached type wins only if the registry still supports it — a registry can
/// be reconfigured between calls, and sending a type this client can no longer
/// decode would break the *response* leg, not just the request. Falling back to
/// the default is always safe because the default is by construction one this
/// client speaks.
template<typename Registry, typename Cache, typename Key>
[[nodiscard]] auto select_request_media_type(const Registry& registry, const Cache& cache,
                                             const Key& target) -> std::string {
    if (const auto cached = cache.get(target); cached && registry.supports(*cached)) {
        return *cached;
    }
    return registry.default_media_type();
}

/// @brief The next request `Media_Type` to try after a peer rejected the
///        previous one, or `nullopt` when this client has nothing left to offer.
///
/// **Why a blind retry rather than asking the peer.** HTTP negotiates the
/// *response* through `Accept`, which the server reads before answering; the
/// request has no equivalent, so a client must commit to an encoding before it
/// has heard anything from the peer. When that first guess is one the peer does
/// not speak, the peer answers 415 (4.15 in CoAP) and — before this — nothing
/// ever revised the guess, so the pairing stayed broken forever. Requirement 7.3
/// says a multi-serializer node and a single-serializer node SHALL interoperate
/// whenever they share a format, which is precisely this case.
///
/// `Accept-Post` (W3C Linked Data Platform 1.0 §7.1) would let the rejecting
/// server say what it *would* take, converging in one retry instead of up to N.
/// It was not chosen as the mechanism because it requires the **peer** to change,
/// and Requirement 7.3 promises interoperation "without requiring the
/// single-serializer node to change" — an unmodified peer emits no such header,
/// which is exactly the case the requirement is about. It also has no CoAP
/// analogue, and this policy has to hold across all four transports. Adding
/// `Accept-Post` later as an *optimisation* composes with this rather than
/// replacing it: use it when present, fall back to this when absent.
///
/// @param registry      Supplies `preferred_media_types()`, in preference order.
/// @param already_tried Every type already sent to this peer for this call,
///                      including the original. Passed in rather than tracked
///                      here so the caller — which owns the retry loop and its
///                      threading — remains the only thing that needs to be
///                      correct about ordering.
///
/// Bounded by construction: each call returns a type not in @p already_tried, so
/// a caller that appends every result terminates after at most
/// `preferred_media_types().size()` attempts. A single-serializer registry
/// returns `nullopt` immediately, since its one type is always the one that was
/// just rejected — which is what keeps this change free for every configuration
/// that ships today.
template<typename Registry>
[[nodiscard]] auto next_request_media_type_after_rejection(
    const Registry& registry, const std::vector<std::string>& already_tried)
    -> std::optional<std::string> {
    for (const auto& candidate : registry.preferred_media_types()) {
        if (std::find(already_tried.begin(), already_tried.end(), candidate) ==
            already_tried.end()) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace kythira
