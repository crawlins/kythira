// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file service_module.hpp
/// @brief The interface a Kythira service implements to run in a node
///        (.kiro/specs/service-colocation/ task 1, Requirement 1).
///
/// A service module (Redis, DynamoDB, Kinesis, the ledger, IAM, the HSM,
/// KMS) plugs into a node through `service_module<Host>`, where `Host` is
/// the node's multi-raft host. The node never names a service: it builds
/// the modules the build registered (`module_registry.hpp`), gives each a
/// `group_space` on the shared host, and drives them through
/// configure, register, publish, start, drain and stop.
///
/// Everything here is core public API, so a module depends only on what
/// any outside consumer could use (Requirement 1.2, services/README.md).

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::node {

/// Each module's fixed number (design.md "One host, partitioned ids and
/// keys"). A composite node gives module `m` the group ids
/// `[m << 56, (m + 1) << 56)` and the key prefix `"\0" m "/"`.
namespace module_numbers {
inline constexpr std::uint8_t redis = 1;
inline constexpr std::uint8_t dynamodb = 2;
inline constexpr std::uint8_t kinesis = 3;
inline constexpr std::uint8_t ledger = 4;
inline constexpr std::uint8_t iam = 5;
inline constexpr std::uint8_t hsm = 6;
inline constexpr std::uint8_t kms = 7;
/// The largest number a module may take: 255 would put the end of its
/// range past 2^64.
inline constexpr std::uint8_t max = 254;
}  // namespace module_numbers

/// The Raft group ids and key prefix one module owns on a node's host
/// (Requirement 4).
///
/// In a composite node the range is `[m << 56, (m + 1) << 56)` and the
/// prefix `"\0" m "/"`. A stand-alone daemon's module keeps the ids and
/// keys it uses today: no offset and no prefix (Requirement 4.4), which is
/// why a stand-alone data directory cannot be opened by a composite node.
class group_space {
public:
    static constexpr unsigned k_shift = 56;

    /// A composite node's space for module `number`.
    /// @throws std::invalid_argument for 0 or a number above
    ///         `module_numbers::max`.
    [[nodiscard]] static auto composite(std::uint8_t number) -> group_space {
        if (number == 0 || number > module_numbers::max) {
            throw std::invalid_argument("group_space: module number " + std::to_string(number) +
                                        " is outside 1-" + std::to_string(module_numbers::max));
        }
        return group_space(number, true);
    }

    /// A stand-alone daemon's space: every id, no prefix.
    [[nodiscard]] static auto standalone(std::uint8_t number) -> group_space {
        return group_space(number, false);
    }

    [[nodiscard]] auto module_number() const noexcept -> std::uint8_t { return _number; }
    [[nodiscard]] auto is_composite() const noexcept -> bool { return _composite; }

    /// The first group id the module owns.
    [[nodiscard]] auto first() const noexcept -> std::uint64_t {
        return _composite ? std::uint64_t{_number} << k_shift : 0;
    }

    /// The last group id the module owns (inclusive).
    [[nodiscard]] auto last() const noexcept -> std::uint64_t {
        return _composite ? ((std::uint64_t{_number} + 1) << k_shift) - 1
                          : std::numeric_limits<std::uint64_t>::max();
    }

    [[nodiscard]] auto contains(std::uint64_t group) const noexcept -> bool {
        return group >= first() && group <= last();
    }

    /// The host's id for the module's own group id `local` (the id the
    /// module uses stand-alone: DynamoDB's bootstrap groups 1 and 2, its
    /// per-voter ranges `(ordinal + 1) << 48`).
    /// @throws std::out_of_range when `local` does not fit the range.
    [[nodiscard]] auto to_host(std::uint64_t local) const -> std::uint64_t {
        if (!_composite) {
            return local;
        }
        if (local >= (std::uint64_t{1} << k_shift)) {
            throw std::out_of_range("group_space: group " + std::to_string(local) +
                                    " does not fit module " + std::to_string(_number) + "'s range");
        }
        return first() | local;
    }

    /// The module's own id for host group `group`, or nothing when the
    /// group is another module's.
    [[nodiscard]] auto to_local(std::uint64_t group) const noexcept
        -> std::optional<std::uint64_t> {
        if (!contains(group)) {
            return std::nullopt;
        }
        return group - first();
    }

    /// The prefix every key of the module carries on the host: empty
    /// stand-alone.
    [[nodiscard]] auto key_prefix() const -> std::string {
        if (!_composite) {
            return {};
        }
        return std::string{'\0', static_cast<char>(_number), '/'};
    }

    /// `key` as the host stores it.
    [[nodiscard]] auto to_host_key(std::string_view key) const -> std::string {
        return key_prefix() + std::string(key);
    }

    /// A host key as the module knows it, or nothing for another module's
    /// key.
    [[nodiscard]] auto to_local_key(std::string_view key) const -> std::optional<std::string_view> {
        const auto prefix = key_prefix();
        if (!key.starts_with(prefix)) {
            return std::nullopt;
        }
        return key.substr(prefix.size());
    }

    /// Whether two spaces share a group id. Stand-alone spaces cover every
    /// id, so two of them always overlap: one node holds at most one.
    [[nodiscard]] auto overlaps(const group_space& other) const noexcept -> bool {
        return first() <= other.last() && other.first() <= last();
    }

private:
    group_space(std::uint8_t number, bool composite) : _number(number), _composite(composite) {}

    std::uint8_t _number;
    bool _composite;
};

/// Who is calling across an in-process interface (Requirement 7.2): the
/// calling module and the principal of the request that led to the call.
/// The target authorizes `principal` exactly as it authorizes the same
/// principal arriving over the network, so co-location never widens
/// access.
struct caller {
    /// The calling module's name ("dynamodb").
    std::string module;
    /// The original caller's principal ARN; empty for the module's own
    /// background work, which the target authorizes as the module's
    /// service principal.
    std::string principal;
    /// The original caller's account, when known.
    std::string account;
    /// The request id of the call that led here, for correlated logs.
    std::string source_request_id;
};

/// A module's health, as the node's health endpoint reports it.
struct module_health {
    bool healthy = false;
    /// Why not, or what is degraded; empty when healthy.
    std::string detail;
};

/// One metric a module reports. The node writes it as
/// `kythira_<module>_<name>` (Requirement 1.1: under the module's own
/// prefix), so `name` carries no prefix of its own.
struct module_metric {
    std::string name;
    double value = 0;
    /// Prometheus labels, as `key="value"` pairs.
    std::vector<std::pair<std::string, std::string>> labels;
};

/// Reads one environment variable; nothing when unset or empty.
using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

class module_registry;

/// What a node needs from a service (Requirement 1.1). The node calls, in
/// order: `configure`, `register_groups`, `publish` (every module), then
/// `start` (in dependency order, so a module's lookups find what it
/// depends on), and at shutdown `drain` then `stop` (in reverse order).
///
/// `Host` is the node's multi-raft host; a module registers its state
/// machines and bootstrap groups on it, with ids and keys mapped through
/// its `group_space`.
template<typename Host> class service_module {
public:
    service_module() = default;
    service_module(const service_module&) = delete;
    auto operator=(const service_module&) -> service_module& = delete;
    service_module(service_module&&) = delete;
    auto operator=(service_module&&) -> service_module& = delete;
    virtual ~service_module() = default;

    /// The name `KYTHIRA_SERVICES` uses ("dynamodb").
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;

    /// The module's number (`module_numbers`), which fixes its group
    /// space in a composite node.
    [[nodiscard]] virtual auto number() const -> std::uint8_t = 0;

    /// The prefix of the module's own environment variables
    /// ("KYTHIRA_DYNAMODB_"); node-wide settings are `KYTHIRA_NODE_*`.
    [[nodiscard]] virtual auto env_prefix() const -> std::string_view = 0;

    /// The node's data directory subdirectory the module keeps its files
    /// in; by default its name.
    [[nodiscard]] virtual auto data_subdir() const -> std::string { return std::string(name()); }

    /// Modules this one needs co-located: the node refuses to start
    /// without them. A module that can fall back to the network for
    /// another's service does not list it (Requirement 7.3).
    [[nodiscard]] virtual auto requires_modules() const -> std::vector<std::string> { return {}; }

    /// Modules that, when co-located, must start before this one, so this
    /// one's lookups find their interfaces. Optional: each need not be
    /// enabled.
    [[nodiscard]] virtual auto starts_after() const -> std::vector<std::string> { return {}; }

    /// Reads and checks the module's settings.
    /// @returns the reason the settings are wrong, or nothing.
    [[nodiscard]] virtual auto configure(const env_lookup& env) -> std::optional<std::string> = 0;

    /// Registers the module's state machines and bootstrap groups on the
    /// host, inside `space`.
    virtual void register_groups(Host& host, const group_space& space) = 0;

    /// Publishes the module's in-process interfaces. Called for every
    /// module before any starts.
    virtual void publish(module_registry& /*registry*/) {}

    /// Opens listeners and starts serving; looks up other modules'
    /// interfaces here.
    virtual void start(module_registry& registry) = 0;

    /// Stops taking new requests and finishes the ones in flight, before
    /// Raft stops.
    virtual void drain() {}

    /// Stops; the node then stops the host.
    virtual void stop() = 0;

    [[nodiscard]] virtual auto health() const -> module_health = 0;

    [[nodiscard]] virtual auto metrics() const -> std::vector<module_metric> { return {}; }
};

}  // namespace kythira::node
