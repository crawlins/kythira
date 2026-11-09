// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Leader-side checkpoints for format-1 InstallSnapshot
// (.kiro/specs/file-install-snapshot/ Requirement 5).
//
// A leader sends a persistent group's snapshot from a checkpoint of its
// state machine: a directory of files and a manifest. Taking one is cheap on
// a real engine (hard links) but the files pin disk until deleted, so the
// registry shares one checkpoint between every transfer at the same
// (index, term) (5.2), counts the leases on it, and deletes it once it is
// unused and either superseded by a newer checkpoint or older than a
// configured age (5.3). An unused checkpoint is kept for that age, rather
// than deleted on its last release, so a transfer that failed can start
// again (or, with task 6, resume) from the same files.
//
// Not thread-safe: the node calls it under its mutex. A lease may be
// released from any thread, so leases only touch an atomic count; the
// registry reads the counts when it collects.

#include <raft/persistent_state_machine.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace kythira {

class checkpoint_registry {
public:
    using clock = std::chrono::steady_clock;

    /// Names of the checkpoint directories the registry owns under its
    /// root; the node deletes leftovers with this prefix at start.
    static constexpr std::string_view k_prefix = "kythira-checkpoint-";

    struct entry {
        std::uint64_t index{0};
        std::uint64_t term{0};
        std::filesystem::path dir;
        snapshot_manifest manifest;
        std::atomic<std::size_t> leases{0};
        // When it was last acquired or released, as clock ticks; a lease is
        // released from any thread.
        std::atomic<clock::rep> last_used{0};

        auto touch() -> void { last_used = clock::now().time_since_epoch().count(); }
        [[nodiscard]] auto last_used_at() const -> clock::time_point {
            return clock::time_point(clock::duration(last_used.load()));
        }
    };

    /// Shared use of one checkpoint. Dropping the last copy releases it.
    class lease {
    public:
        lease() = default;
        [[nodiscard]] auto index() const -> std::uint64_t { return _entry->index; }
        [[nodiscard]] auto term() const -> std::uint64_t { return _entry->term; }
        [[nodiscard]] auto directory() const -> const std::filesystem::path& { return _entry->dir; }
        [[nodiscard]] auto manifest() const -> const snapshot_manifest& { return _entry->manifest; }
        explicit operator bool() const noexcept { return _entry != nullptr; }

    private:
        friend class checkpoint_registry;
        explicit lease(std::shared_ptr<entry> e)
            : _entry(std::move(e)), _hold(_entry.get(), [](entry* p) {
                  p->touch();
                  --p->leases;
              }) {
            ++_entry->leases;
        }
        std::shared_ptr<entry> _entry;
        // Decrements the count when the last copy of this lease goes.
        std::shared_ptr<entry> _hold;
    };

    checkpoint_registry() = default;
    checkpoint_registry(std::filesystem::path root, std::chrono::milliseconds max_unused_age)
        : _root(std::move(root)), _max_unused_age(max_unused_age) {}

    [[nodiscard]] auto root() const -> const std::filesystem::path& { return _root; }

    /// A lease on the checkpoint at (index, term), taking it with
    /// `take(dir)` if there is none. `take` must create `dir` and return its
    /// manifest; if it throws, nothing is registered and `dir` is removed.
    /// Collects unused checkpoints first, so a newer checkpoint supersedes
    /// older ones.
    auto acquire(std::uint64_t index, std::uint64_t term,
                 const std::function<snapshot_manifest(const std::filesystem::path&)>& take)
        -> lease {
        const auto now = clock::now();
        for (const auto& e : _entries) {
            if (e->index == index && e->term == term) {
                e->touch();
                return lease(e);
            }
        }
        collect(now, index);
        std::filesystem::create_directories(_root);
        auto dir = _root / std::format("{}{}-{}-{}", k_prefix, index, term, ++_serial);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        auto e = std::make_shared<entry>();
        e->index = index;
        e->term = term;
        e->dir = dir;
        e->touch();
        try {
            e->manifest = take(dir);
        } catch (...) {
            std::filesystem::remove_all(dir, ec);
            throw;
        }
        _entries.push_back(e);
        return lease(e);
    }

    /// Deletes every unused checkpoint that is older than the age limit or
    /// below `keep_from_index`. Returns how many it deleted.
    auto collect(clock::time_point now = clock::now(), std::uint64_t keep_from_index = 0,
                 bool all_unused = false) -> std::size_t {
        std::size_t removed = 0;
        std::erase_if(_entries, [&](const std::shared_ptr<entry>& e) {
            if (e->leases.load() != 0) {
                return false;
            }
            if (!all_unused && e->index >= keep_from_index &&
                now - e->last_used_at() < _max_unused_age) {
                return false;
            }
            std::error_code ec;
            std::filesystem::remove_all(e->dir, ec);
            ++removed;
            return true;
        });
        return removed;
    }

    /// Deletes every unused checkpoint, whatever its age (leadership lost,
    /// Requirement 5.4). Checkpoints still leased go when collected later.
    auto release_unused() -> std::size_t { return collect(clock::now(), 0, /*all_unused=*/true); }

    /// Checkpoints held, and their bytes (Requirement 5.3's metric).
    [[nodiscard]] auto count() const -> std::size_t { return _entries.size(); }
    [[nodiscard]] auto bytes() const -> std::uint64_t {
        std::uint64_t total = 0;
        for (const auto& e : _entries) {
            total += e->manifest.total_bytes();
        }
        return total;
    }

private:
    std::filesystem::path _root;
    std::chrono::milliseconds _max_unused_age{std::chrono::minutes{10}};
    std::vector<std::shared_ptr<entry>> _entries;
    std::uint64_t _serial{0};
};

}  // namespace kythira
