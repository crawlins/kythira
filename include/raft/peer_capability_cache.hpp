#pragma once

/// @file peer_capability_cache.hpp
/// @brief Remembers which media type each peer last *accepted a request in*, so
///        a client stops guessing after the first successful exchange.
///
/// Shared by `cpp_httplib_client` and `boost_beast_client` rather than
/// reimplemented per transport, for the same reason `parse_accept_header` is
/// shared: two copies of a caching rule drift, and the drift is invisible until
/// a peer happens to use the transport with the stale copy.
///
/// **Accepted, not answered — and the distinction is the whole correctness
/// argument.** What this cache is consulted for is the *request* encoding, so
/// the only evidence worth storing is evidence about what the peer will decode:
/// a request `Content-Type`/`Content-Format` that came back as anything other
/// than a 415. The media type a peer *answers* in looks like the same fact and
/// is not one. They coincide for our own server, which answers in a type drawn
/// from a set it also decodes, and nothing in HTTP or CoAP obliges a foreign
/// peer to accept what it replies in. Recording the response type instead — as
/// this did until the asymmetry was measured — leaves a peer that decodes cbor
/// but always answers json cached as json, 415ing and retrying on *every*
/// subsequent request rather than converging after one, which is exactly the
/// promise the next paragraph makes.
///
/// The cache is an *optimisation, not a protocol*. Every request still
/// advertises the client's full `Accept` list, so a stale or missing entry
/// costs at most one extra round of the peer choosing again — it can never make
/// a request fail. That is what lets this have no TTL and no invalidation: a
/// peer that changes what it decodes has its entry corrected by the next
/// exchange that succeeds, and in the meantime its own `Accept`-driven choice
/// still wins on the response leg.
///
/// `accept_post_negotiation_test` holds both halves of that claim as counts
/// rather than as outcomes: a client that never converges completes every RPC
/// too, just at one extra round trip each, so only the attempt count separates
/// "corrected once" from "paying forever".

#include <raft/http_content_negotiation.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kythira {

/// @brief Per-target record of the media type a peer last accepted a request in.
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
    /// @brief The media type @p key last accepted a request in, if one was
    ///        recorded.
    [[nodiscard]] auto get(const Key& key) const -> std::optional<std::string> {
        const std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _entries.find(key);
        if (it == _entries.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// @brief Record that @p key accepted a request encoded in @p media_type.
    ///
    /// @p media_type is the *request*'s own `Content-Type`/`Content-Format` on
    /// an attempt the peer did not reject — never the type the response came
    /// back in. See the file comment: only the former is evidence about the leg
    /// this cache steers.
    ///
    /// Only ever called after the whole exchange succeeded, response decode
    /// included. Recording on a failure would cache the very media type that
    /// just did not work, and the next request would repeat the mistake with
    /// more confidence.
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
/// Reads the cache written by `record` above, which is why that has to hold
/// what the peer *accepts*: this is the request leg, and a cache of response
/// types answers a different question.
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
/// `Accept-Post` (W3C Linked Data Platform 1.0 §7.1) lets the rejecting server
/// say what it *would* take, converging in one retry instead of up to N. It is
/// not the mechanism, because it requires the **peer** to change and Requirement
/// 7.3 promises interoperation "without requiring the single-serializer node to
/// change" — an unmodified peer emits no such header, which is exactly the case
/// the requirement is about. It also has no CoAP analogue, and this policy has
/// to hold across all four transports. It is layered on top instead, by the
/// overload below: use it when present, fall back to this when absent.
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

/// @brief The same choice, but informed by the rejecting peer's `Accept-Post`.
///
/// `Accept-Post` (W3C Linked Data Platform 1.0 §7.1) is the response header a
/// server may put on a 415 to name the request media types it *would* have
/// accepted. When a peer sends one, this returns the first type in **the peer's
/// own order** that this client can also produce and has not already tried — so
/// a client with N registered serializers converges on the second attempt rather
/// than possibly the Nth.
///
/// **Strictly an optimisation, and shaped so it cannot become anything else.**
/// Three properties, each deliberate:
///
///   * An absent or empty header is exactly the old behaviour. This is the
///     common case and always will be: an unmodified single-serializer peer —
///     the case Requirement 7.3 is about — emits no such header.
///   * The peer's order wins over ours when both would work. The peer is the one
///     that just refused something; between two types we can both handle, its
///     stated preference is the better information, and either choice succeeds.
///   * **A header that names nothing usable falls back to the blind walk rather
///     than giving up.** A peer could list only types we do not have, list types
///     we already tried, send a wildcard, or simply be wrong about itself. In
///     every one of those cases the blind walk is what would have run anyway, so
///     trusting the header can cost a few wasted round trips but can never turn
///     an exchange that the blind walk would have completed into a failure. That
///     asymmetry is the whole justification for layering this on at all.
///
/// The header is parsed with `parse_accept_header`, not a second parser of its
/// own: `Accept-Post` is a comma-separated media-type list with the same
/// syntax, and two parsers for one grammar is how the copy that is subtly wrong
/// stays hidden. Wildcards therefore come through verbatim and simply fail
/// `registry.supports()`, which lands on the fallback — the right answer, since
/// a peer that says it accepts `*/*` has told us nothing that narrows the walk.
///
/// @param accept_post The peer's raw header value; empty when it sent none.
template<typename Registry>
[[nodiscard]] auto next_request_media_type_after_rejection(
    const Registry& registry, const std::vector<std::string>& already_tried,
    std::string_view accept_post) -> std::optional<std::string> {
    for (const auto& offered : parse_accept_header(accept_post)) {
        if (registry.supports(offered) &&
            std::find(already_tried.begin(), already_tried.end(), offered) == already_tried.end()) {
            return offered;
        }
    }
    return next_request_media_type_after_rejection(registry, already_tried);
}

}  // namespace kythira
