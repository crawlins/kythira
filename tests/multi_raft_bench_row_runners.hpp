// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_bench_row_runners.hpp
/// @brief One non-template entry point per `(transport x wire serializer)`
///        pair, each **defined in its own translation unit**.
///
/// ### Why this file exists
///
/// `throughput_row<Transport>` and `read_row<Transport>` in
/// `multi_raft_benchmark_rows.hpp` instantiate the entire multi-Raft stack —
/// cluster, workload, transport, serializer — for every argument they are
/// given. Both consumers named nine such arguments *in one translation unit
/// each*, and the two of them were the most expensive compiles in this tree by
/// a factor of three:
///
///                                              stdexec     folly
///     multi_raft_http_benchmark_test.cpp       21,114 MiB  6,247 MiB
///     multi_raft_performance_report.cpp        16,640 MiB  6,057 MiB
///
/// against 5,967 MiB for the heaviest previously-pooled TU in the tree. On a
/// cold `Full suite (stdexec)` build those two were a 36.9 GiB pair on a runner
/// with 16 GiB and a 24 GiB swapfile, and they killed it twice. The `heavy_tu`
/// job pool stops them running *concurrently*; it cannot make either one
/// smaller, so a serialised chain containing both stayed the critical path.
///
/// This is the same move `cmd/multi_raft_node` already made, and for the same
/// reason: its `main.cpp` named four `(transport x serializer)` pairs and
/// peaked at 10,974 MiB, and splitting them into one `run_*.cpp` apiece behind
/// the declarations in `host_runners.hpp` took it to 1,509. See that file.
///
/// ### The second gain, which the host binary did not have
///
/// The two consumers here name an overlapping set, so the pairs are compiled
/// **once for both** rather than once each. Seven of the nine are named by both
/// binaries; only `cpp-httplib/cbor` and `cpp-httplib/protobuf` are the suite's
/// alone. That is why these live in a library both link rather than in each
/// binary's own sources.
///
/// ### The rule for adding a pair
///
/// A new `(transport, serializer)` pair gets **a new file** in
/// `tests/bench_rows/`, listed in `tests/CMakeLists.txt`, and a declaration
/// here. Do not add a second pair to an existing file: the whole property this
/// header buys is that no translation unit instantiates the stack more than
/// once, and nothing but the file boundary enforces it.
///
/// The set is closed deliberately, exactly as `host_runners.hpp`'s is. A pair
/// that no consumer names is a stack instantiation nobody asked for.

#include "multi_raft_benchmark_rows.hpp"

namespace kythira::testing::rows {

/// @brief A write row: `k_required_repetitions` measurements of one spec.
///
/// The signature is `throughput_row`'s with the template argument bound. It is
/// a plain function pointer type so the report binary's catalog can store one
/// without a `std::type_identity` tag — see `build_catalog`.
using write_runner = auto (*)(const write_row_spec&, const row_observer&) -> repeated_result;

/// @brief A read row, the same way.
using read_runner = auto (*)(const read_row_spec&, const row_observer&) -> repeated_result;

/// @brief The cheapest liveness check a transport has: one PUT and one GET per
///        shard, over a real socket, asserted through the observer.
///
/// Takes a `row_observer` rather than using Boost.Test macros directly, for the
/// reason the rows themselves do: this code is compiled into a library that the
/// report binary links too, and `_require` there abandons a row instead of
/// aborting a test case.
using smoke_runner = auto (*)(const row_observer&) -> void;

// ── the fabric (in-process, no socket) ───────────────────────────────────────

auto write_fabric(const write_row_spec& spec, const row_observer& observer) -> repeated_result;

// ── transactions (in-process fabric) ─────────────────────────────────────────
//
// `.kiro/specs/multi-raft-transactions/` Requirement 13.6. The rows and their
// cluster are in the Redis module (services/redis/tests/
// multi_raft_txn_bench_rows.hpp); the spec is here so the report's catalog
// can build one without including that header.

}  // namespace kythira::testing::rows

namespace kythira::testing {

/// @brief What a transaction row does in each operation.
enum class txn_row_kind : std::uint8_t {
    /// Begin, write one key on each of `_keys_per_txn` keys, commit.
    write,
    /// Begin a snapshot, read `_keys_per_txn` keys, commit (read-only: no
    /// prepare). Needs a versioned state machine.
    snapshot_read,
};

/// @brief Everything that distinguishes one transaction row from another.
struct txn_row_spec {
    txn_row_kind _kind{txn_row_kind::write};
    /// How many data shards the cluster has. The keys of one transaction are
    /// spread over all of them, round robin, so `_shards = 1` is the
    /// one-shard (one-phase, under `mvcc::off`) row.
    std::size_t _shards{1};
    /// Keys per transaction.
    std::size_t _keys_per_txn{2};
    std::size_t _operations{200};
    std::size_t _in_flight{4};
    std::size_t _value_bytes{128};
};

/// @brief One transaction row: `k_required_repetitions` measurements.
using txn_runner = auto (*)(const txn_row_spec&, const row_observer&) -> repeated_result;

}  // namespace kythira::testing

namespace kythira::testing::rows {

/// The tier-1 rows: `read_committed` over `mvcc::off` (one-phase on one shard).
auto txn_fabric(const txn_row_spec& spec, const row_observer& observer) -> repeated_result;
/// The tier-2 rows: `snapshot` over `mvcc::on`, with an oracle group.
auto txn_snapshot_fabric(const txn_row_spec& spec, const row_observer& observer) -> repeated_result;

// ── cpp-httplib ──────────────────────────────────────────────────────────────

auto write_httplib_json(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result;
auto write_httplib_cbor(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result;
auto smoke_httplib_json(const row_observer& observer) -> void;

#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
auto write_httplib_protobuf(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result;
#endif

// ── Boost.Beast ──────────────────────────────────────────────────────────────

#if defined(KYTHIRA_BENCH_HAS_BEAST)
auto write_beast_json(const write_row_spec& spec, const row_observer& observer) -> repeated_result;
/// The only read pair either consumer names: the read taxonomy and the
/// shard-size curve are both Beast/JSON.
auto read_beast_json(const read_row_spec& spec, const row_observer& observer) -> repeated_result;
auto smoke_beast_json(const row_observer& observer) -> void;

auto write_beast_cbor(const write_row_spec& spec, const row_observer& observer) -> repeated_result;

#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
auto write_beast_protobuf(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result;
#endif
#if defined(KYTHIRA_BENCH_HAS_ION)
auto write_beast_ion(const write_row_spec& spec, const row_observer& observer) -> repeated_result;
#endif
#endif  // KYTHIRA_BENCH_HAS_BEAST

// ── Proxygen ─────────────────────────────────────────────────────────────────

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
auto write_proxygen_json(const write_row_spec& spec, const row_observer& observer)
    -> repeated_result;
auto smoke_proxygen_json(const row_observer& observer) -> void;
#endif

// ── libcoap ──────────────────────────────────────────────────────────────────

#if defined(KYTHIRA_BENCH_HAS_COAP)
/// The CoAP row (Requirement 17a). JSON only: the question it answers is what
/// one shared client costs N groups, and the encoding axis is already answered
/// on the HTTP rows.
auto write_coap_json(const write_row_spec& spec, const row_observer& observer) -> repeated_result;
auto smoke_coap_json(const row_observer& observer) -> void;
#endif

}  // namespace kythira::testing::rows
