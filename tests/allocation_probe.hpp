// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Records the largest single `operator new` request made while a probe is
// armed, so a test can assert that a decoder's allocations stay proportional
// to its input instead of following a length the input merely declares.
//
// This header REPLACES the global allocation functions, which C++ allows only
// once per program and never `inline`. Include it from exactly one translation
// unit of a test executable (every serializer test is a single .cpp file).
// Allocations made by C libraries through malloc() (ion-c, for example) are
// not seen; tests for those check that the decoder rejects before calling in.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace kythira::test {

inline std::atomic<bool> allocation_probe_armed{false};
inline std::atomic<std::size_t> allocation_probe_largest{0};

/// Arms the probe for its lifetime; `largest()` is the biggest request seen.
class allocation_probe {
public:
    allocation_probe() {
        allocation_probe_largest.store(0);
        allocation_probe_armed.store(true);
    }
    ~allocation_probe() { allocation_probe_armed.store(false); }
    allocation_probe(const allocation_probe&) = delete;
    auto operator=(const allocation_probe&) -> allocation_probe& = delete;

    [[nodiscard]] static auto largest() -> std::size_t { return allocation_probe_largest.load(); }
};

inline auto allocation_probe_record(std::size_t size) -> void {
    if (!allocation_probe_armed.load(std::memory_order_relaxed)) {
        return;
    }
    auto seen = allocation_probe_largest.load(std::memory_order_relaxed);
    while (size > seen && !allocation_probe_largest.compare_exchange_weak(seen, size)) {
    }
}

}  // namespace kythira::test

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc)
auto operator new(std::size_t size) -> void* {
    kythira::test::allocation_probe_record(size);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}

auto operator new[](std::size_t size) -> void* {
    return ::operator new(size);
}

auto operator new(std::size_t size, const std::nothrow_t&) noexcept -> void* {
    kythira::test::allocation_probe_record(size);
    return std::malloc(size == 0 ? 1 : size);
}

auto operator new[](std::size_t size, const std::nothrow_t& tag) noexcept -> void* {
    return ::operator new(size, tag);
}

auto operator delete(void* p) noexcept -> void {
    std::free(p);
}

auto operator delete[](void* p) noexcept -> void {
    std::free(p);
}

auto operator delete(void* p, std::size_t) noexcept -> void {
    std::free(p);
}

auto operator delete[](void* p, std::size_t) noexcept -> void {
    std::free(p);
}
// NOLINTEND(misc-new-delete-overloads,cppcoreguidelines-no-malloc)
