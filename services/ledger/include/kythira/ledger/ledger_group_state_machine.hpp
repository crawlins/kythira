// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One state machine type for every group on a ledger node
// (.kiro/specs/verifiable-ledger/ task 7c, Requirements 1.1 and 2.3).
//
// multi_raft builds every group's state machine from one factory and one
// type, then opens it on the group's range of the node's engine. A ledger
// node runs two kinds of group, the catalog and one group per ledger, so
// this type is a handle that decides what it is when it is opened:
//
//   c/catalog/...       the ledger catalog (basic_ledger_catalog)
//   l/<ledger id>/...   that ledger (basic_ledger_state_machine)
//
// Any other range is a configuration error and open() throws.
//
// The handle is cheap to copy and every copy shares one state machine, as
// multi_raft moves the factory's value into the node. Every handle built
// from one context records what it opened there, so the node finds the
// catalog and each ledger's state machine by range without taking the
// Raft node's lock; on_open and on_drop tell it of changes as well.
//
// Ledger groups never split or merge (Requirement 1.1): the split policy
// hooks report no size and no split keys and veto every key, and a split
// or merge that reaches apply anyway throws ledger_split_vetoed (or
// ledger_catalog_error for the catalog). vetoes() counts both, for the
// node's metrics.

#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/persistent_state_machine.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::ledger {

/// The ledger id a range belongs to, when it is a ledger's range.
inline auto ledger_id_of_range(const kythira::key_range& range) -> std::optional<std::string> {
    const std::string_view start = range.start;
    if (!start.starts_with("l/") || !start.ends_with("/") || start.size() < 4) {
        return std::nullopt;
    }
    std::string id(start.substr(2, start.size() - 3));
    if (id.find('/') != std::string::npos || ledger_range(id) != range) {
        return std::nullopt;
    }
    return id;
}

template<kythira::kv_engine Engine> class basic_ledger_group_state_machine {
public:
    using engine_type = Engine;
    using catalog_type = basic_ledger_catalog<Engine>;
    using ledger_type = basic_ledger_state_machine<Engine>;
    using opened = std::variant<std::shared_ptr<catalog_type>, std::shared_ptr<ledger_type>>;

    /// What every group on the node shares: how a ledger's options are
    /// built (its segment directory, chiefly), the open groups by range
    /// start, whom else to tell when a group opens or drops, and the veto
    /// counter.
    struct context {
        std::function<ledger_options(const std::string& ledger_id)> options{};
        std::function<void(const kythira::key_range&, const opened&)> on_open{};
        std::function<void(const kythira::key_range&)> on_drop{};
        std::atomic<std::uint64_t> vetoes{0};

        /// The state machine open on `range` here, if any.
        [[nodiscard]] auto find(const kythira::key_range& range) const -> std::optional<opened> {
            const std::lock_guard lock(mutex);
            const auto it = groups.find(range.start);
            return it == groups.end() ? std::nullopt : std::optional<opened>(it->second);
        }

        mutable std::mutex mutex;
        std::map<std::string, opened> groups;
    };

    basic_ledger_group_state_machine() : basic_ledger_group_state_machine(nullptr) {}

    explicit basic_ledger_group_state_machine(std::shared_ptr<context> ctx)
        : ctx_(ctx ? std::move(ctx) : std::make_shared<context>()),
          state_(std::make_shared<state>()) {}

    // ── persistent_state_machine ────────────────────────────────────────────

    auto open(engine_type& engine, const kythira::key_range& range) -> void {
        if (range == catalog_range()) {
            auto c = std::make_shared<catalog_type>();
            c->open(engine, range);
            state_->sm = c;
        } else if (const auto id = ledger_id_of_range(range)) {
            auto l = std::make_shared<ledger_type>(ctx_->options ? ctx_->options(*id)
                                                                 : ledger_options{});
            l->open(engine, range);
            state_->sm = l;
        } else {
            throw std::invalid_argument("ledger node: range [" + range.start + ", " +
                                        range.end.value_or("") +
                                        ") is neither the catalog nor a ledger");
        }
        state_->range = range;
        {
            const std::lock_guard lock(ctx_->mutex);
            ctx_->groups.insert_or_assign(range.start, *state_->sm);
        }
        if (ctx_->on_open) {
            ctx_->on_open(range, *state_->sm);
        }
    }

    auto apply(const std::vector<std::byte>& command, std::uint64_t index)
        -> std::vector<std::byte> {
        return visit([&](auto& sm) { return sm.apply(command, index); });
    }

    [[nodiscard]] auto applied_index() const -> std::uint64_t {
        return visit([](const auto& sm) { return sm.applied_index(); });
    }

    [[nodiscard]] auto durable_applied_index() const -> std::uint64_t {
        return visit([](const auto& sm) { return sm.durable_applied_index(); });
    }

    auto checkpoint(const std::filesystem::path& dir) -> kythira::snapshot_manifest {
        return visit([&](auto& sm) { return sm.checkpoint(dir); });
    }

    auto ingest(const std::filesystem::path& dir, const kythira::snapshot_manifest& manifest,
                std::uint64_t index) -> void {
        visit([&](auto& sm) { sm.ingest(dir, manifest, index); });
    }

    auto drop() -> void {
        visit([](auto& sm) { sm.drop(); });
        {
            const std::lock_guard lock(ctx_->mutex);
            ctx_->groups.erase(state_->range.start);
        }
        if (ctx_->on_drop) {
            ctx_->on_drop(state_->range);
        }
    }

    // ── persistent_splittable_state_machine: always vetoed ──────────────────

    auto split_at(const std::vector<std::string>& keys) -> std::vector<kythira::key_range> {
        ctx_->vetoes.fetch_add(1, std::memory_order_relaxed);
        return visit([&](auto& sm) { return sm.split_at(keys); });
    }

    auto adopt(const kythira::key_range& range) -> void {
        ctx_->vetoes.fetch_add(1, std::memory_order_relaxed);
        visit([&](auto& sm) { sm.adopt(range); });
    }

    [[nodiscard]] auto range() const -> const kythira::key_range& { return state_->range; }

    // ── split_policy_state_machine: never a reason or a place to split ──────

    /// Zero, so a size-based policy never wants to split a ledger group.
    [[nodiscard]] auto approximate_size_bytes() const -> std::size_t { return 0; }
    [[nodiscard]] auto approximate_key_count() const -> std::size_t { return 0; }
    auto suggest_split_keys(std::size_t /*max*/) -> std::vector<std::string> { return {}; }
    [[nodiscard]] auto can_split_at(const std::string& /*key*/) const -> bool {
        ctx_->vetoes.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // ── For the node ────────────────────────────────────────────────────────

    /// The catalog or ledger state machine, once open.
    [[nodiscard]] auto get() const -> const std::optional<opened>& { return state_->sm; }
    [[nodiscard]] auto shared_context() const -> const std::shared_ptr<context>& { return ctx_; }

private:
    struct state {
        std::optional<opened> sm;
        kythira::key_range range;
    };

    template<typename Fn> auto visit(Fn&& fn) const -> decltype(auto) {
        if (!state_->sm) {
            throw std::logic_error("ledger node: group state machine used before open");
        }
        return std::visit([&](const auto& p) -> decltype(auto) { return fn(*p); }, *state_->sm);
    }

    std::shared_ptr<context> ctx_;
    std::shared_ptr<state> state_;
};

}  // namespace kythira::ledger
