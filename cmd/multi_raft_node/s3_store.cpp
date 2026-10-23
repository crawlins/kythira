// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file s3_store.cpp
/// @brief The S3 arm of `switchable_persistence`
///        (`.kiro/specs/multi-raft-node-s3-persistence/`).
///
/// Compiled only when `KYTHIRA_NODE_HAS_S3_PERSISTENCE` is defined, which
/// CMake does when the AWS SDK is found and `CONFIG_AWS_S3_PERSISTENCE` is on.
/// Both fencing instantiations of the engine live here and nowhere else; see
/// `s3_group_store` for why they are behind an interface.

#include "s3_describe.hpp"
#include "switchable_persistence.hpp"

#include <raft/aws_client_config.hpp>
#include <raft/aws_s3_client.hpp>
#include <raft/object_store_persistence.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira::bench {
namespace {

using unfenced_engine =
    kythira::object_store_persistence_engine<kythira::aws_s3_client, kythira::host_node_id,
                                             std::uint64_t, std::uint64_t>;
using fenced_engine =
    kythira::fenced_object_store_persistence_engine<kythira::aws_s3_client, kythira::host_node_id,
                                                    std::uint64_t, std::uint64_t>;

/// One engine behind `s3_group_store`. Every mutating call goes through
/// `guard`, which logs a run-time fence once per group (Requirement 4.4) and
/// rethrows: the engine's latch already fails every later write, and the host
/// neither retries nor tries to clear it.
template<typename Engine> class s3_group_store_impl final : public s3_group_store {
public:
    s3_group_store_impl(std::uint64_t group, Engine engine)
        : _group(group), _engine(std::move(engine)) {}

    auto save_current_term(std::uint64_t term) -> void override {
        guard([&] { _engine.save_current_term(term); });
    }
    auto load_current_term() -> std::uint64_t override { return _engine.load_current_term(); }
    auto save_voted_for(const kythira::host_node_id& node) -> void override {
        guard([&] { _engine.save_voted_for(node); });
    }
    auto load_voted_for() -> std::optional<kythira::host_node_id> override {
        return _engine.load_voted_for();
    }
    auto append_log_entry(const log_entry_t& entry) -> void override {
        guard([&] { _engine.append_log_entry(entry); });
    }
    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> override {
        return _engine.get_log_entry(index);
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end)
        -> std::vector<log_entry_t> override {
        return _engine.get_log_entries(start, end);
    }
    auto get_last_log_index() -> std::uint64_t override { return _engine.get_last_log_index(); }
    auto truncate_log(std::uint64_t index) -> void override {
        guard([&] { _engine.truncate_log(index); });
    }
    auto save_snapshot(const snapshot_t& snap) -> void override {
        guard([&] { _engine.save_snapshot(snap); });
    }
    auto load_snapshot() -> std::optional<snapshot_t> override { return _engine.load_snapshot(); }
    auto delete_log_entries_before(std::uint64_t index) -> void override {
        guard([&] { _engine.delete_log_entries_before(index); });
    }
    auto append_log_entry_sequenced(const log_entry_t& entry) -> kythira::write_sequence override {
        return guard([&] { return _engine.append_log_entry_sequenced(entry); });
    }
    auto barrier_through(kythira::write_sequence seq) -> void override {
        guard([&] { _engine.barrier_through(seq); });
    }
    [[nodiscard]] auto durable_through() const -> kythira::write_sequence override {
        return _engine.durable_through();
    }
    [[nodiscard]] auto durability() const -> kythira::durability_class override {
        return _engine.durability();
    }
    [[nodiscard]] auto flushes_issued() const -> std::uint64_t override {
        return _engine.flushes_issued();
    }
    [[nodiscard]] auto is_fenced() const -> bool override { return _engine.is_fenced(); }

private:
    template<typename F> auto guard(F&& f) -> decltype(auto) {
        try {
            return std::forward<F>(f)();
        } catch (const kythira::persistence_fenced_error& e) {
            if (!_fence_logged.exchange(true)) {
                std::cerr << "multi_raft_node: group " << _group
                          << " is FENCED: another process wrote " << e.key()
                          << ". Every further write to this group fails; the host does not "
                             "retry. "
                          << e.what() << "\n";
            }
            throw;
        }
    }

    std::uint64_t _group;
    Engine _engine;
    std::atomic<bool> _fence_logged{false};
};

[[nodiscard]] auto client_config(const s3_persistence_options& s3) -> kythira::aws_client_config {
    kythira::aws_client_config cfg;
    cfg.region = s3.region;
    cfg.endpoint_override = s3.endpoint;
    cfg.api_timeout = s3_api_timeout(s3.timeout);
    return cfg;
}

}  // namespace

auto make_s3_group_store(const node_options& opt, std::uint64_t group)
    -> std::unique_ptr<s3_group_store> {
    const auto& s3 = opt._s3;
    const auto prefix = kythira::group_key_prefix(s3_host_prefix(opt), group);
    auto engine_options = s3_engine_options(opt);
    try {
        kythira::aws_s3_client client{client_config(s3)};
        if (s3.fencing == s3_fencing::cas) {
            return std::make_unique<s3_group_store_impl<fenced_engine>>(
                group, fenced_engine{std::move(client), s3.bucket, prefix, engine_options});
        }
        return std::make_unique<s3_group_store_impl<unfenced_engine>>(
            group, unfenced_engine{std::move(client), s3.bucket, prefix, engine_options});
    } catch (const kythira::persistence_fenced_error& e) {
        throw std::runtime_error(
            "group " + std::to_string(group) + ": " + e.key() +
            " is held by another process, so this host (owner \"" + engine_options.owner_id +
            "\") refuses to start rather than interleave two logs. Is "
            "another host running with node id " +
            kythira::host_node_id_text(opt._node_id) +
            ", or did this one move address under the same --s3-prefix? (" + e.what() + ")");
    } catch (const std::exception& e) {
        throw std::runtime_error("group " + std::to_string(group) + " at s3://" + s3.bucket + "/" +
                                 prefix + ": " + e.what());
    }
}

}  // namespace kythira::bench
