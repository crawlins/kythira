// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file synchronized.hpp
/// @brief Minimal, portable `folly::Synchronized<T>`-alike: a value guarded by
///     a `std::shared_mutex`, accessed through RAII lock proxies. Exists so
///     that mutex-guarded shared state (peer2peer replication's progress
///     tables, gossip transport's membership sets) doesn't force a Folly
///     dependency independent of which kythira::Future backend is selected --
///     `folly::Synchronized` itself has nothing to do with futures, but
///     pulling in `<folly/Synchronized.h>` transitively requires Folly to be
///     found/on the include path regardless.
///
/// Supports exactly the subset of `folly::Synchronized`'s API this project's
/// call sites use: `wlock()`/`rlock()` returning a proxy with `operator->`
/// and `operator*`, usable directly (`x.wlock()->foo()`) or bound to a local
/// (`auto locked = x.rlock();`).

#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>

namespace kythira {

template<typename T> class synchronized {
public:
    synchronized() = default;
    explicit synchronized(T value) : _value(std::move(value)) {}

    // std::shared_mutex is neither copyable nor movable, so these can't be
    // defaulted -- but folly::Synchronized<T> (what this replaces) DOES
    // support copy/move by locking the source and transferring just the
    // guarded value into a freshly-default-constructed mutex, which real
    // call sites (e.g. a peer2peer_replicator value stored in a node_config
    // that later gets moved into node<Types>) rely on. Each new instance
    // gets its own independent mutex; no attempt is made to prevent a
    // theoretical concurrent-bidirectional-copy deadlock since nothing in
    // this codebase copies/moves these across threads concurrently.
    synchronized(const synchronized& other) : _value(*other.rlock()) {}
    synchronized(synchronized&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : _value(std::move(*other.wlock())) {}

    auto operator=(const synchronized& other) -> synchronized& {
        if (this != &other) {
            *wlock() = *other.rlock();
        }
        return *this;
    }
    auto operator=(synchronized&& other) noexcept(std::is_nothrow_move_assignable_v<T>)
        -> synchronized& {
        if (this != &other) {
            *wlock() = std::move(*other.wlock());
        }
        return *this;
    }

    class write_proxy {
    public:
        write_proxy(T& value, std::shared_mutex& mutex) : _value(value), _lock(mutex) {}
        [[nodiscard]] auto operator->() const -> T* { return &_value; }
        [[nodiscard]] auto operator*() const -> T& { return _value; }

    private:
        T& _value;
        std::unique_lock<std::shared_mutex> _lock;
    };

    class read_proxy {
    public:
        read_proxy(const T& value, std::shared_mutex& mutex) : _value(value), _lock(mutex) {}
        [[nodiscard]] auto operator->() const -> const T* { return &_value; }
        [[nodiscard]] auto operator*() const -> const T& { return _value; }

    private:
        const T& _value;
        std::shared_lock<std::shared_mutex> _lock;
    };

    [[nodiscard]] auto wlock() -> write_proxy { return write_proxy(_value, _mutex); }
    [[nodiscard]] auto rlock() const -> read_proxy { return read_proxy(_value, _mutex); }

private:
    T _value{};
    mutable std::shared_mutex _mutex;
};

}  // namespace kythira
