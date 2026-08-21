// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file transport_types_registry_conformance_test.cpp
/// @brief Pins the `serializer_registry_type` requirement that Task 5 added to
///        `transport_types` (`.kiro/specs/transport-multi-serializer/`).
///
/// Every assertion here is a `static_assert`, so this binary's real work happens
/// at compile time and a passing run means the compile succeeded. That is the
/// point: the failure mode this file exists to prevent is a *silent* one.
/// `transport_types` is a concept, and a concept that has quietly stopped
/// constraining anything still lets every consumer compile and every test pass
/// — the same "green while doing nothing" shape this codebase has been bitten by
/// repeatedly. The negative cases below are therefore the load-bearing ones: they
/// fail the build if the new requirement is ever dropped or weakened to a bare
/// `typename T::serializer_registry_type;`.

#include <raft/coap_transport.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/types.hpp>

#define BOOST_TEST_MODULE transport_types_registry_conformance_test
#include <boost/test/unit_test.hpp>

#include <concepts>
#include <cstddef>
#include <vector>

namespace {

using data_type = std::vector<std::byte>;
using ser = kythira::json_rpc_serializer<data_type>;
using met = kythira::noop_metrics;
// `transport_types` never invokes the executor, only names it, so any complete
// type serves. Using a non-executor here keeps this file off Folly.
using exe = int;

// ---------------------------------------------------------------------------
// Positive: the shipped single-serializer bundles still conform.
// ---------------------------------------------------------------------------

static_assert(kythira::transport_types<kythira::http_transport_types<ser, met, exe>>,
              "http_transport_types must still model transport_types");
static_assert(kythira::transport_types<kythira::simple_http_transport_types<ser, met, exe>>,
              "simple_http_transport_types must still model transport_types");
static_assert(kythira::transport_types<kythira::coap_transport_types<ser, met, exe>>,
              "coap_transport_types must still model transport_types");
static_assert(kythira::transport_types<kythira::simple_coap_transport_types<ser, met, exe>>,
              "simple_coap_transport_types must still model transport_types");

// `std_http_transport_types` and `std_coap_transport_types` are deliberately
// absent. Both pin `future_template` to `std::future`, which does not model
// `future` (no `wait(duration)` yielding a testable value), so neither has ever
// satisfied `transport_types` — confirmed by asserting it against the tree as it
// stood before this change. They are unreachable configurations, not casualties
// of the registry requirement, and asserting either way here would only encode a
// false expectation about which one broke them.

// ---------------------------------------------------------------------------
// Positive: the bundles keep `serializer_type` and the registry's default in
// agreement, which is the invariant Requirement 3.2 relies on when it lets
// existing code go on naming `Types::serializer_type` directly.
// ---------------------------------------------------------------------------

static_assert(
    std::same_as<typename kythira::http_transport_types<ser, met, exe>::serializer_registry_type,
                 kythira::single_serializer_registry<ser>>);
static_assert(
    std::same_as<typename kythira::coap_transport_types<ser, met, exe>::serializer_registry_type,
                 kythira::single_serializer_registry<ser>>);

// ---------------------------------------------------------------------------
// Negative: the requirement is load-bearing.
// ---------------------------------------------------------------------------

/// The pre-Task-5 bundle shape: everything the concept used to ask for, and no
/// registry. If this ever starts satisfying `transport_types`, the requirement
/// has been dropped.
struct bundle_without_registry {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = ser;
    using metrics_type = met;
    using executor_type = exe;
};
static_assert(!kythira::transport_types<bundle_without_registry>,
              "serializer_registry_type must be required, not optional");

/// A bundle that names a `serializer_registry_type` which is not in fact a
/// registry. Catches the weaker mistake of asking only that the *name* exist —
/// a bare `typename T::serializer_registry_type;` in the concept would let this
/// through, and the resulting error would surface much later, inside a
/// negotiation call, as an unreadable substitution failure.
struct not_a_registry {};
struct bundle_with_non_registry {
    template<typename T> using future_template = kythira::future_default<T>;
    using serializer_type = ser;
    using serializer_registry_type = not_a_registry;
    using metrics_type = met;
    using executor_type = exe;
};
static_assert(!kythira::transport_types<bundle_with_non_registry>,
              "serializer_registry_type must be checked against the serializer_registry concept");

/// A registry missing exactly one member of the concept. `supports()` is chosen
/// because it is the one a hand-rolled registry is likeliest to forget: the
/// other three all feed the header-building path and would be missed
/// immediately, while `supports()` is only consulted on the decode side.
struct registry_missing_supports {
    [[nodiscard]] auto default_media_type() const -> std::string { return "application/json"; }
    [[nodiscard]] auto preferred_media_types() const -> std::vector<std::string> {
        return {"application/json"};
    }
    [[nodiscard]] auto select_output_media_type(const std::vector<std::string>&) const
        -> std::optional<std::string> {
        return std::nullopt;
    }
};
static_assert(!kythira::serializer_registry<registry_missing_supports, data_type>,
              "serializer_registry must require every member, including supports()");

// ---------------------------------------------------------------------------
// Negative: `raft_types` is untouched (Requirement 3.5). The node-internal
// bundle serializes log entries and snapshots, not wire RPCs, so it has no
// business negotiating a media type with anyone and must not have acquired a
// registry by association with the transport-side change.
// ---------------------------------------------------------------------------

/// Spelt as a concept rather than as a bare `requires` expression at namespace
/// scope: a requires-expression whose operands are all non-dependent is
/// evaluated eagerly, so `requires { typename X::missing_type; }` is a hard
/// error under GCC instead of yielding `false`. Routing it through a template
/// parameter makes the lookup dependent, which is what makes the negative
/// answer expressible at all.
template<typename T>
concept has_serializer_registry_type = requires { typename T::serializer_registry_type; };

static_assert(has_serializer_registry_type<kythira::http_transport_types<ser, met, exe>>,
              "sanity check that has_serializer_registry_type detects a registry at all");
static_assert(!has_serializer_registry_type<kythira::default_raft_types>,
              "the node-internal raft_types bundle deliberately has no registry");

}  // namespace

/// Boost.Test needs at least one case; the assertions above have already run by
/// the time this executes.
BOOST_AUTO_TEST_CASE(transport_types_requires_a_serializer_registry) {
    BOOST_TEST(true);
}
