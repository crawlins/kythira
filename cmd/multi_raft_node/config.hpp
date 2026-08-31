// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file config.hpp
/// @brief Everything `multi_raft_node` takes from the command line or a
///        configuration file. `.kiro/specs/multi-raft-host-binary/`
///        Requirement 1.2: **no axis this project sweeps may require a
///        rebuild.**
///
/// That is not a convenience. The performance spec sweeps transport, wire
/// serializer, tick cadence, group count, node count, persistence mode and the
/// shard split, and a row whose configuration is a compile-time constant is a
/// row that cannot be re-taken by anyone who does not also rebuild — which
/// makes every comparison between two rows a comparison of two binaries.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kythira::bench {

/// @brief Which inter-node transport carries Raft RPCs.
///
/// The three the Tier B matrix sweeps, so that a Tier C row can be read beside
/// the Tier B row sharing its transport (Requirement 1.5). Chosen at runtime
/// from a string, never at compile time.
enum class node_transport : std::uint8_t {
    httplib = 0,
    beast = 1,
    proxygen = 2,
};

/// @brief Which serializer encodes the RPCs on the wire.
enum class wire_serializer : std::uint8_t {
    json = 0,
    cbor = 1,
};

/// @brief What the host's log is written to.
///
/// The same three arms the in-process durability axis sweeps, with the same
/// names, so a Tier C durability row and a Tier B one are labelled identically.
/// `file_buffered` is **not durable** and every row carrying it says so.
enum class persistence_mode : std::uint8_t {
    memory = 0,
    file_buffered = 1,
    file_barrier = 2,
};

struct node_options {
    /// This host's node id. Also its identity in every peer's URL map.
    std::uint64_t _node_id{1};

    /// Where this host serves Raft RPCs to its peers.
    std::string _bind_address{"0.0.0.0"};
    std::uint16_t _raft_port{0};

    /// Where clients reach the data path, and — separately — the control
    /// surface. Two ports, because Requirement 2.4 asks that a measurement
    /// cannot accidentally include control traffic and a path prefix does not
    /// achieve that.
    std::uint16_t _data_port{0};
    std::uint16_t _control_port{0};

    /// Every peer's Raft URL, including this host's own. A static list is the
    /// reproducible baseline and all Tier C needs (Requirement 5.1);
    /// `--peers-from` reads the same mapping out of a file for Tier E, where
    /// no operator wants to edit a command line on every machine.
    std::map<std::uint64_t, std::string> _peers;

    /// Every peer's **control** address, as `host:port`. Only the network
    /// probe uses it, and only Tier E needs it.
    ///
    /// Kept separate from `_peers` rather than derived from it. A Raft URL
    /// does not carry a control port, and deriving one by a convention
    /// ("control is Raft plus two") would silently probe whatever happens to
    /// be listening there — which is exactly the class of measurement error
    /// this project keeps finding. A peer with no entry here is reported with
    /// null figures rather than guessed at.
    std::map<std::uint64_t, std::string> _peer_control;

    /// The voters of every group. All hosts hold a replica of every shard,
    /// which is what the in-process rows do, so a Tier C row differs from a
    /// Tier B row in the tier and not in the placement.
    std::vector<std::uint64_t> _voters;

    /// **Pre-split into N ranges** (Requirement 4.4), answering Appendix B's
    /// question 4 the reproducible way and matching what `kv_cluster` does, so
    /// Tier C and Tier B tile the key space identically. Growth by automatic
    /// split is what a real deployment does, is a different measurement, and
    /// the row records which it got.
    std::size_t _groups{4};
    /// The key space the ranges tile, in the same units `kv_key` uses.
    std::uint64_t _key_count{100000};

    /// `multi_raft` has no timer of its own; this is the period of the thread
    /// that drives `tick()`, and it is a swept axis (Requirement 1.3).
    std::chrono::milliseconds _tick_interval{2};

    std::chrono::milliseconds _election_timeout_min{150};
    std::chrono::milliseconds _election_timeout_max{300};
    std::chrono::milliseconds _heartbeat_interval{50};
    std::chrono::milliseconds _rpc_timeout{1000};
    std::chrono::milliseconds _op_timeout{3000};

    node_transport _transport{node_transport::httplib};
    wire_serializer _serializer{wire_serializer::json};
    persistence_mode _persistence{persistence_mode::memory};
    std::filesystem::path _data_dir{};

    std::size_t _executor_stripes{4};

    /// @brief Threads the data-path server will use to serve client requests.
    ///
    /// **A swept axis, because it turned out to be one.** cpp-httplib defaults
    /// its pool to `max(8, hardware_concurrency - 1)`, and a request handler
    /// blocks for the whole commit — so the number of client operations a host
    /// can have in flight is capped by this and not by the cluster. Measured on
    /// a four-core machine at Tier C: four operations in flight gives 1530.2
    /// ops/sec at a 6.1% spread and a `stable` verdict, while sixteen collapses
    /// into a bimodal 75/1300 with a 1622% spread and **zero elections in any
    /// window** — so the cause is queueing here, not consensus there.
    ///
    /// Zero leaves cpp-httplib's default, which is what the first Tier C rows
    /// were taken with.
    std::size_t _data_threads{0};

    /// How long to wait for a leader before reporting readiness on the control
    /// port. A driver waits on that rather than sleeping.
    std::chrono::milliseconds _ready_timeout{30000};
};

[[nodiscard]] auto parse_node_options(int argc, char** argv) -> node_options;
[[nodiscard]] auto node_usage() -> std::string;

}  // namespace kythira::bench
