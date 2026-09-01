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

/// @brief Where this host's peer list comes from.
///
/// `.kiro/specs/multi-machine-placement/` Requirement 3. `static_list` is the
/// DEFAULT AND THE ONE A MEASURED ROW USES (Requirement 3.4): discovery is a
/// control-plane API call, and a row whose numbers include one of those is
/// measuring the cloud's control plane. `ec2_tag` exists to prove the cluster
/// can form without a hand-edited file, which is a functional claim and not a
/// performance one.
enum class discovery_mode : std::uint8_t {
    static_list = 0,
    ec2_tag = 1,
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

    /// Where `_peers` comes from. `static_list` leaves it exactly as the
    /// command line gave it, which is what every measured row uses.
    discovery_mode _discovery{discovery_mode::static_list};

    /// The run-scoped tag scoping an `ec2_tag` search. The same tag the leak
    /// audit keys off, so one run has one identity everywhere.
    std::string _discovery_run_tag{};

    /// Region for the discovery client. Empty uses the SDK's own resolution.
    std::string _discovery_region{};

    /// Only instances whose role tag starts with this are peers. Shape 2
    /// launches N hosts and one driver under one run tag, and the driver is
    /// not a replica — without this the cluster believes it has N+1 voters.
    std::string _discovery_role_prefix{};

    /// How many peers must appear, this host included. Discovery that
    /// proceeded with whatever it found would measure a cluster with a replica
    /// missing and report a number for it (Requirement 3.5).
    std::size_t _discovery_expect{0};

    /// Total time discovery is allowed to converge before the host refuses to
    /// start and names what it never saw.
    std::chrono::milliseconds _discovery_budget{120000};

    /// The Raft URL this host publishes for its peers to dial. It cannot be
    /// derived from `_bind_address`, which is typically 0.0.0.0 — a host that
    /// advertised that would tell every peer to connect to itself.
    std::string _advertise_address{};

    /// Every discovered peer's control port.
    ///
    /// **Stated, never derived.** `_peer_control` is deliberately not computed
    /// from a peer's Raft URL by a convention like "control is Raft plus two",
    /// because that silently probes whatever happens to be listening there.
    /// This is not that: it is the operator saying that in THIS deployment
    /// every host was given the same control port, which Shape 2 guarantees by
    /// construction because each host is alone on its machine. Left zero,
    /// `_peer_control` stays empty and the network probe reports null figures
    /// rather than guessing — the same behaviour a missing `--peer-control`
    /// already has.
    std::uint16_t _discovery_control_port{0};

    /// EC2 endpoint override for the discovery client.
    ///
    /// Present so that discovery is TESTABLE WITHOUT A CLOUD ACCOUNT — pointed
    /// at LocalStack, the whole path runs on a workstation. The AWS SDK's
    /// `AWS_ENDPOINT_URL_EC2` environment variable is not honoured by the
    /// version vendored here: a run configured that way silently reached real
    /// EC2 and failed with `AuthFailure`, which reads like a credentials
    /// problem and is actually a routing one. An explicit option cannot do
    /// that.
    std::string _discovery_endpoint{};

    /// This instance's own id. Empty asks IMDS, which is the only way a
    /// process learns it unaided; injectable for tests and for a controller
    /// driving this from outside the instance.
    std::string _discovery_instance_id{};
};

[[nodiscard]] auto parse_node_options(int argc, char** argv) -> node_options;
[[nodiscard]] auto node_usage() -> std::string;

}  // namespace kythira::bench
