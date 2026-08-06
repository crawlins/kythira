#pragma once

/// @file serializer_registry.hpp
/// @brief The set of serializers a transport may negotiate between, and the
///        dispatch from a negotiated media type to the serializer that handles
///        it.
///
/// Four serializers ship (JSON, CBOR, Protocol Buffers, Amazon Ion) and, before
/// this, no transport could negotiate between them: each `Types` bundle named
/// exactly one `serializer_type` and every HTTP response carried a hardcoded
/// `application/json` `Content-Type` regardless of which one was configured.
///
/// Two registries rather than one, because the single-serializer case is not
/// merely a degenerate multi case:
///
///  * `single_serializer_registry<S>` keeps today's configuration expressible
///    with byte-identical wire behaviour, which is what makes adopting a
///    registry an additive change rather than a migration. Its dispatch is a
///    compile-time-known comparison, so it costs nothing over calling the
///    serializer directly.
///  * `multi_serializer_registry<S...>` folds over a `std::tuple` at runtime.
///    The first type parameter is the default, since a peer that expresses no
///    preference has to get *something* and "whatever was listed first" is the
///    only ordering the caller actually controls.
///
/// Neither owns any negotiation policy beyond `select_output_media_type`: the
/// transports decide what to do with a `std::nullopt` (406/4.06), because the
/// status codes differ per protocol.

#include <raft/http_exceptions.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace kythira {

/// @brief A registry holding exactly one serializer.
/// @tparam S Serializer type; must satisfy `rpc_serializer`.
template<typename S>
requires rpc_serializer<S, std::vector<std::byte>>
class single_serializer_registry {
public:
    using data_type = std::vector<std::byte>;

    single_serializer_registry() = default;
    explicit single_serializer_registry(S s) : _s(std::move(s)) {}

    [[nodiscard]] auto default_media_type() const -> std::string { return _s.media_type(); }

    [[nodiscard]] auto preferred_media_types() const -> std::vector<std::string> {
        return {_s.media_type()};
    }

    [[nodiscard]] auto supports(const std::string& media_type) const -> bool {
        return media_type == _s.media_type();
    }

    /// @brief Best supported media type among @p accepted, most-preferred first.
    ///
    /// An empty `accepted` means the peer stated no preference — which is not
    /// the same as "accepts nothing" — so it yields the default rather than
    /// `nullopt`. This matters because a missing `Accept` header is the common
    /// case for a peer that predates negotiation entirely.
    [[nodiscard]] auto select_output_media_type(const std::vector<std::string>& accepted) const
        -> std::optional<std::string> {
        if (accepted.empty()) {
            return default_media_type();
        }
        for (const auto& media_type : accepted) {
            if (supports(media_type)) {
                return media_type;
            }
        }
        return std::nullopt;
    }

    /// @throws unsupported_media_type_error if @p media_type is not this
    ///         registry's serializer.
    template<typename Request>
    [[nodiscard]] auto encode_with(const std::string& media_type, const Request& req) const
        -> data_type {
        if (!supports(media_type)) {
            throw unsupported_media_type_error(media_type);
        }
        return _s.serialize(req);
    }

    /// @throws unsupported_media_type_error if @p media_type is not this
    ///         registry's serializer.
    template<typename T>
    [[nodiscard]] auto decode_with(const std::string& media_type, const data_type& data) const
        -> T {
        if (!supports(media_type)) {
            throw unsupported_media_type_error(media_type);
        }
        return _s.template deserialize<T>(data);
    }

    [[nodiscard]] auto serializer() const -> const S& { return _s; }

private:
    S _s{};
};

/// @brief A registry holding two or more serializers, negotiating between them.
/// @tparam Serializers Two or more types satisfying `rpc_serializer`. The first
///         is the default.
///
/// Every registered serializer must report a **distinct** `media_type()`. Two
/// claiming the same one is a configuration error that cannot be diagnosed
/// here: `media_type()` is a runtime call, so there is nothing to `static_assert`
/// on, and a per-call check would tax the hot path to catch a mistake that is
/// fixed at construction. The consequence of getting it wrong is that the last
/// duplicate wins for both encode and decode — consistently, so it degrades to
/// "one of them is unreachable" rather than to a wire-format mismatch.
/// `serializer_registry_unit_test.cpp` asserts the shipped serializers are
/// mutually distinct, which is where the risk actually lives.
template<typename... Serializers>
requires(sizeof...(Serializers) >= 2) &&
        (rpc_serializer<Serializers, std::vector<std::byte>> && ...)
class multi_serializer_registry {
public:
    using data_type = std::vector<std::byte>;

    multi_serializer_registry() = default;
    explicit multi_serializer_registry(Serializers... serializers)
        : _serializers(std::move(serializers)...) {}

    [[nodiscard]] auto default_media_type() const -> std::string {
        return std::get<0>(_serializers).media_type();
    }

    /// @brief Every supported media type, in declaration order.
    ///
    /// Declaration order *is* preference order — it becomes the `Accept` header
    /// this side sends — so it is deliberately not sorted or deduplicated.
    [[nodiscard]] auto preferred_media_types() const -> std::vector<std::string> {
        std::vector<std::string> result;
        result.reserve(sizeof...(Serializers));
        std::apply([&](const auto&... s) { (result.push_back(s.media_type()), ...); },
                   _serializers);
        return result;
    }

    [[nodiscard]] auto supports(const std::string& media_type) const -> bool {
        return std::apply([&](const auto&... s) { return ((s.media_type() == media_type) || ...); },
                          _serializers);
    }

    /// @brief Best supported media type among @p accepted.
    ///
    /// The *peer's* order wins, not ours: `accepted` is scanned in order and
    /// the first supported entry returned. A client that ranks its preferences
    /// is entitled to have them honoured, and both sides scanning their own
    /// order would make the negotiated type depend on which side asked.
    [[nodiscard]] auto select_output_media_type(const std::vector<std::string>& accepted) const
        -> std::optional<std::string> {
        if (accepted.empty()) {
            return default_media_type();
        }
        for (const auto& media_type : accepted) {
            if (supports(media_type)) {
                return media_type;
            }
        }
        return std::nullopt;
    }

    /// @throws unsupported_media_type_error if no registered serializer claims
    ///         @p media_type.
    template<typename Request>
    [[nodiscard]] auto encode_with(const std::string& media_type, const Request& req) const
        -> data_type {
        std::optional<data_type> result;
        std::apply(
            [&](const auto&... s) {
                ((s.media_type() == media_type ? (result = s.serialize(req), void()) : void()),
                 ...);
            },
            _serializers);
        if (!result) {
            throw unsupported_media_type_error(media_type);
        }
        return std::move(*result);
    }

    /// @throws unsupported_media_type_error if no registered serializer claims
    ///         @p media_type.
    template<typename T>
    [[nodiscard]] auto decode_with(const std::string& media_type, const data_type& data) const
        -> T {
        std::optional<T> result;
        std::apply(
            [&](const auto&... s) {
                ((s.media_type() == media_type ? (result = s.template deserialize<T>(data), void())
                                               : void()),
                 ...);
            },
            _serializers);
        if (!result) {
            throw unsupported_media_type_error(media_type);
        }
        return std::move(*result);
    }

private:
    std::tuple<Serializers...> _serializers{};
};

}  // namespace kythira
