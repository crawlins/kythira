// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <raft/aws_endpoint_override.hpp>
#include <raft/env_config_result.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace kythira::bench {
namespace {

[[nodiscard]] auto to_u64(const std::string& s, const char* what) -> std::uint64_t {
    try {
        return static_cast<std::uint64_t>(std::stoull(s));
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("multi_raft_node: ") + what + ": not a number: " + s);
    }
}

[[nodiscard]] auto to_ms(const std::string& s, const char* what) -> std::chrono::milliseconds {
    return std::chrono::milliseconds{static_cast<std::int64_t>(to_u64(s, what))};
}

/// A peer's node id: `kythira::host_node_id`'s forms, never `metadata:`
/// (only this host's own id is read from instance metadata).
[[nodiscard]] auto to_peer_id(const std::string& s, const char* what) -> kythira::host_node_id {
    auto id = kythira::parse_host_peer_node_id(s);
    if (!id) {
        throw std::runtime_error(std::string("multi_raft_node: ") + what + ": " + id.error());
    }
    return std::move(id).value();
}

/// `id=url` — the form both `--peer` and `--peers-from` use, so a Tier E
/// deployment can move a command line into a file without translating it.
/// Canonical id text holds no `=`, so the first one ends the id.
auto add_peer(node_options& out, const std::string& spec) -> void {
    const auto eq = spec.find('=');
    if (eq == std::string::npos) {
        throw std::runtime_error("multi_raft_node: --peer expects <id>=<url>, got: " + spec);
    }
    out._peers[to_peer_id(spec.substr(0, eq), "--peer id")] = spec.substr(eq + 1);
}

auto add_peer_control(node_options& out, const std::string& spec) -> void {
    const auto eq = spec.find('=');
    if (eq == std::string::npos) {
        throw std::runtime_error(
            "multi_raft_node: --peer-control expects <id>=<host>:<port>, got: " + spec);
    }
    out._peer_control[to_peer_id(spec.substr(0, eq), "--peer-control id")] = spec.substr(eq + 1);
}

auto add_voters(node_options& out, const std::string& spec) -> void {
    out._voters.clear();
    std::istringstream in(spec);
    std::string part;
    while (std::getline(in, part, ',')) {
        if (!part.empty()) {
            out._voters.push_back(to_peer_id(part, "--voters"));
        }
    }
}

[[nodiscard]] auto parse_transport(const std::string& s) -> node_transport {
    if (s == "httplib") {
        return node_transport::httplib;
    }
    if (s == "beast") {
        return node_transport::beast;
    }
    if (s == "proxygen") {
        return node_transport::proxygen;
    }
    throw std::runtime_error("multi_raft_node: --transport must be httplib|beast|proxygen, got: " +
                             s);
}

[[nodiscard]] auto parse_serializer(const std::string& s) -> wire_serializer {
    if (s == "json") {
        return wire_serializer::json;
    }
    if (s == "cbor") {
        return wire_serializer::cbor;
    }
    throw std::runtime_error("multi_raft_node: --serializer must be json|cbor, got: " + s);
}

[[nodiscard]] auto parse_persistence(const std::string& s) -> persistence_mode {
    if (s == "memory") {
        return persistence_mode::memory;
    }
    if (s == "file-buffered") {
        return persistence_mode::file_buffered;
    }
    if (s == "file-barrier") {
        return persistence_mode::file_barrier;
    }
    // Accepted in every build: whether the arm is compiled in is `main`'s
    // question, so a build without it names the switch to turn on rather
    // than reporting an unknown mode (Requirement 7.2).
    if (s == "s3") {
        return persistence_mode::s3;
    }
    throw std::runtime_error(
        "multi_raft_node: --persistence must be memory|file-buffered|file-barrier|s3, got: " + s);
}

[[nodiscard]] auto persistence_spelling(persistence_mode mode) -> const char* {
    switch (mode) {
        case persistence_mode::memory:
            return "memory";
        case persistence_mode::file_buffered:
            return "file-buffered";
        case persistence_mode::file_barrier:
            return "file-barrier";
        case persistence_mode::s3:
            return "s3";
    }
    return "unknown";
}

/// A `0|1` flag, the way `--capacity-dry-run` reads one, except that any
/// other number is an error: `--s3-write-behind 2` is a typo, not "on".
[[nodiscard]] auto to_bool01(const std::string& s, const char* what) -> bool {
    const auto v = to_u64(s, what);
    if (v > 1) {
        throw std::runtime_error(std::string("multi_raft_node: ") + what +
                                 " must be 0 or 1, got: " + s);
    }
    return v == 1;
}

/// The `--s3-*` group. Returns false for any other flag so the caller's
/// chain can go on. Range rules the engine enforces itself (segment sizes,
/// concurrency, retention) are deliberately not repeated here: `main`
/// preflights the options against the engine, so there is one rule and one
/// message (Requirement 5.3).
[[nodiscard]] auto apply_s3_flag(s3_persistence_options& s3, const std::string& flag,
                                 const std::string& value) -> bool {
    if (!flag.starts_with("--s3-")) {
        return false;
    }
    const char* what = flag.c_str();
    auto& engine = s3.engine;
    if (flag == "--s3-bucket" || flag == "--s3-prefix" || flag == "--s3-region" ||
        flag == "--s3-endpoint") {
        if (value.empty()) {
            throw std::runtime_error("multi_raft_node: " + flag + " needs a non-empty value");
        }
    }
    if (flag == "--s3-bucket") {
        s3.bucket = value;
    } else if (flag == "--s3-prefix") {
        s3.prefix = value;
        while (!s3.prefix.empty() && s3.prefix.back() == '/') {
            s3.prefix.pop_back();
        }
        if (s3.prefix.empty()) {
            throw std::runtime_error(
                "multi_raft_node: --s3-prefix is empty once trailing slashes are removed");
        }
    } else if (flag == "--s3-region") {
        s3.region = value;
    } else if (flag == "--s3-endpoint") {
        s3.endpoint = value;
    } else if (flag == "--s3-timeout") {
        s3.timeout = to_ms(value, what);
        if (s3.timeout.count() == 0) {
            throw std::runtime_error("multi_raft_node: --s3-timeout must be at least 1 ms");
        }
    } else if (flag == "--s3-fencing") {
        if (value == "none") {
            s3.fencing = s3_fencing::none;
        } else if (value == "cas") {
            s3.fencing = s3_fencing::cas;
        } else {
            throw std::runtime_error("multi_raft_node: --s3-fencing must be none|cas, got: " +
                                     value);
        }
    } else if (flag == "--s3-hard-state") {
        if (value == "separate") {
            engine.hard_state = kythira::hard_state_layout::separate;
        } else if (value == "combined") {
            engine.hard_state = kythira::hard_state_layout::combined;
        } else {
            throw std::runtime_error(
                "multi_raft_node: --s3-hard-state must be separate|combined, got: " + value);
        }
    } else if (flag == "--s3-log") {
        if (value == "per-entry") {
            engine.log = kythira::log_layout::per_entry;
        } else if (value == "segmented") {
            engine.log = kythira::log_layout::segmented;
        } else {
            throw std::runtime_error(
                "multi_raft_node: --s3-log must be per-entry|segmented, got: " + value);
        }
    } else if (flag == "--s3-max-segment-entries") {
        engine.max_segment_entries = static_cast<std::size_t>(to_u64(value, what));
    } else if (flag == "--s3-max-segment-bytes") {
        engine.max_segment_bytes = static_cast<std::size_t>(to_u64(value, what));
    } else if (flag == "--s3-write-behind") {
        engine.write_behind = to_bool01(value, what);
    } else if (flag == "--s3-append-concurrency") {
        engine.append_concurrency = static_cast<std::size_t>(to_u64(value, what));
    } else if (flag == "--s3-write-retries") {
        engine.write_retries = static_cast<unsigned>(to_u64(value, what));
    } else if (flag == "--s3-snapshot-retention") {
        engine.snapshot_retention = static_cast<std::size_t>(to_u64(value, what));
    } else if (flag == "--s3-verify-checksums") {
        engine.verify_checksums = to_bool01(value, what);
    } else if (flag == "--s3-max-object-bytes") {
        engine.max_object_bytes = static_cast<std::size_t>(to_u64(value, what));
    } else {
        throw std::runtime_error("multi_raft_node: unknown option " + flag);
    }
    s3.flags_seen.push_back(flag);
    return true;
}

/// The `--persistence s3` rows of the validation table in the design, plus
/// the refusal of `--s3-*` under every other mode.
auto validate_s3(node_options& out) -> void {
    auto& s3 = out._s3;
    if (out._persistence != persistence_mode::s3) {
        if (!s3.flags_seen.empty()) {
            throw std::runtime_error("multi_raft_node: " + s3.flags_seen.front() +
                                     " applies to --persistence s3 only, and this host is "
                                     "--persistence " +
                                     persistence_spelling(out._persistence));
        }
        return;
    }
    if (s3.bucket.empty()) {
        throw std::runtime_error("multi_raft_node: --persistence s3 requires --s3-bucket");
    }
    if (s3.prefix.empty()) {
        throw std::runtime_error(
            "multi_raft_node: --persistence s3 requires --s3-prefix, e.g. runs/<run-tag>; the "
            "host appends /nodes/<node-id> itself");
    }
    if (!out._data_dir.empty()) {
        throw std::runtime_error(
            "multi_raft_node: --data-dir is not used under --persistence s3; every group's log "
            "is in the bucket, not on local disk");
    }
    if (!s3.endpoint.empty()) {
        auto check = kythira::validate_s3_endpoint(s3.endpoint);
        if (!check.ok()) {
            // The validator's message names "--endpoint", the backup tool's
            // spelling; this binary's flag is --s3-endpoint.
            auto message = check.error;
            if (message.starts_with("--endpoint")) {
                message.insert(2, "s3-");
            }
            throw std::runtime_error("multi_raft_node: " + message);
        }
        for (auto& warning : check.warnings) {
            if (warning.starts_with("--endpoint")) {
                warning.insert(2, "s3-");
            }
            s3.warnings.push_back(std::move(warning));
        }
        s3.endpoint = check.normalised;
    } else {
        for (auto& warning :
             kythira::ignored_endpoint_variable_warnings(kythira::process_env_lookup())) {
            const auto at = warning.find("--endpoint");
            if (at != std::string::npos) {
                warning.replace(at, 10, "--s3-endpoint");
            }
            s3.warnings.push_back(std::move(warning));
        }
    }
    if (out._election_timeout_min < k_s3_min_election_timeout) {
        s3.warnings.push_back(
            "--election-timeout-min " + std::to_string(out._election_timeout_min.count()) +
            " ms is below " + std::to_string(k_s3_min_election_timeout.count()) +
            " ms; against real S3 a vote must be persisted inside the election, and one PUT can "
            "take longer than that. See \"Election-timeout sizing\" in "
            "doc/cloud_object_persistence.md. Fine against a loopback LocalStack or MinIO");
    }
}

/// One `key=value` line, `#` comments and blanks ignored. Deliberately the
/// same option names the command line uses, minus the leading dashes, so that
/// there is one vocabulary rather than two.
auto apply_file(
    node_options& out, const std::string& path,
    const std::function<void(node_options&, const std::string&, const std::string&)>& apply)
    -> void {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("multi_raft_node: cannot read config file: " + path);
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        const auto last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, last - first + 1);
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("multi_raft_node: config line is not key=value: " + line);
        }
        apply(out, "--" + line.substr(0, eq), line.substr(eq + 1));
    }
}

}  // namespace

namespace {

/// The `--s3-*` section. Separate from the rest so its availability line can
/// depend on the build.
[[nodiscard]] auto s3_usage() -> std::string {
    std::string text = R"(
S3 persistence (.kiro/specs/multi-raft-node-s3-persistence/)
)";
#ifdef KYTHIRA_NODE_HAS_S3_PERSISTENCE
    text += "  Available in this build.\n";
#else
    text +=
        "  UNAVAILABLE IN THIS BUILD: --persistence s3 needs CONFIG_AWS_S3_PERSISTENCE=y and\n"
        "  the AWS SDK found at configure time.\n";
#endif
    text += R"(  Every group's state lives at <prefix>/nodes/<node-id>/groups/<group>, so the
  hosts of one cluster can share one bucket and one prefix. Credentials come
  from the AWS SDK default chain only.
  --s3-bucket NAME           Required.
  --s3-prefix PATH           Required, e.g. runs/<run-tag>.
  --s3-region NAME           Default: the SDK chain, first match wins:
                             $AWS_DEFAULT_REGION, $AWS_REGION, the profile's
                             region ($AWS_CONFIG_FILE or ~/.aws/config,
                             $AWS_PROFILE), instance metadata, us-east-1.
  --s3-endpoint URL          An S3-compatible service, e.g. a LocalStack at
                             http://localhost:4566. Needs a scheme; no path,
                             query or userinfo. Implies path-style addressing
                             and disables IMDS. AWS_ENDPOINT_URL and
                             AWS_ENDPOINT_URL_S3 are NOT read.
  --s3-timeout MS            Per-request timeout (default 30000). The SDK
                             takes whole seconds, so it is rounded up.
  --s3-fencing MODE          cas | none (default cas). cas refuses a second
                             host started with the same node id.
  --s3-hard-state LAYOUT     combined | separate (default combined: one PUT
                             per term-and-vote write).
  --s3-log LAYOUT            per-entry | segmented (default per-entry).
  --s3-max-segment-entries N (default 100; segmented only)
  --s3-max-segment-bytes N   (default 1048576; segmented only)
  --s3-write-behind 0|1      Leader group commit (default 0).
  --s3-append-concurrency N  Log PUTs in flight at once (default 8).
  --s3-write-retries N       Unconditional-PUT retries (default 1).
  --s3-snapshot-retention N  Snapshot generations kept (default 1).
  --s3-verify-checksums 0|1  Check each PUT's ETag (default 1).
  --s3-max-object-bytes N    Largest single PUT (default 67108864).
  An --election-timeout-min below 1000 ms draws a warning: against real S3 a
  vote must be persisted inside the election. See "Election-timeout sizing"
  in doc/cloud_object_persistence.md.
)";
    return text;
}

}  // namespace

auto node_usage() -> std::string {
    return R"(multi_raft_node — a measurement host for `multi_raft`.

NOT A SUPPORTED SERVER. This binary exists so that
.kiro/specs/multi-raft-performance/ can take Tier C and Tier E rows — one host
process per node — and it is deliberately kept out of any install target. It
has no authentication, no authorisation, no multi-tenancy and no rate limiting,
and that is a decision rather than an omission: every one of them would add
cost to the measured path that the numbers would then have to be corrected
for, and none of them is what is being measured. Do not deploy it. The one
exception is the elastic-capacity control plane, which is off the measured
path and can create machines, so it takes a bearer token (see below).

Usage: multi_raft_node --node-id N --raft-port P --data-port P --control-port P
                       --peer 1=http://host:port [--peer 2=...] --voters 1,2,3
                       [options]

Identity and addressing
  --node-id N                This host's node id.
  --bind ADDR                Interface for all three servers (default 0.0.0.0).
  --raft-port P              Where peers reach this host's Raft RPCs.
  --data-port P              Where clients reach put/get/delete.
  --control-port P           Health, status and readiness. A SEPARATE port from
                             the data path so a measurement cannot include
                             control traffic.
  --peer ID=URL              A peer's Raft URL. Repeatable; include this host.
  --peer-control ID=HOST:PORT
                             A peer's CONTROL address, used only by the
                             inter-node network probe (Requirement 5.4).
                             Separate from --peer rather than derived from it:
                             deriving a control port by convention would
                             silently probe whatever happens to be listening
                             there. A peer with no entry is reported with null
                             figures rather than guessed at.
  --peers-from FILE          The same mapping from a file, one `peer=ID=URL`
                             per line, for a deployment nobody wants to edit a
                             command line on.
  --voters 1,2,3             Every group's voter set.

Shape (all swept axes; none requires a rebuild)
  --groups N                 Shards, PRE-SPLIT into N ranges (default 4).
  --key-count N              The key space those ranges tile (default 100000).
  --tick-interval MS         Period of the thread that drives tick() (default 2).
  --transport NAME           httplib | beast | proxygen (default httplib).
  --serializer NAME          json | cbor (default json).
  --persistence MODE         memory | file-buffered | file-barrier | s3
                             (default memory). file-buffered is NOT DURABLE.
                             s3 keeps every group's log in a bucket; see
                             "S3 persistence" below.
  --data-dir PATH            Where a file-backed log lives. Refused under s3.
  --executor-stripes N       Host executor stripes (default 4).
  --data-threads N           Threads serving the data path. A handler blocks
                             for the whole commit, so this caps how many client
                             operations a host can have in flight — measurably:
                             four in flight is stable at 1530 ops/sec on a
                             four-core machine while sixteen goes bimodal with
                             zero elections. 0 keeps cpp-httplib's default of
                             max(8, cores-1).

Timing
  --election-timeout-min MS  (default 150)
  --election-timeout-max MS  (default 300)
  --heartbeat-interval MS    (default 50)
  --rpc-timeout MS           (default 1000)
  --op-timeout MS            Client operation deadline (default 3000).
  --ready-timeout MS         How long to wait for a leader before the control
                             port reports ready (default 30000).

Discovery — how the peer list is obtained
  --discovery MODE           static | ec2-tag (default static).
                             THE STATIC LIST IS THE DEFAULT FOR A MEASURED ROW
                             on purpose: discovery is a control-plane API call,
                             and a row whose numbers include one of those is
                             measuring the cloud rather than this cluster.
                             ec2-tag exists to show the cluster can form with
                             no hand-edited peer list, which is a functional
                             claim and not a performance one.
  --discovery-run-tag VALUE  The run-scoped tag scoping the search. Required
                             with ec2-tag. Same tag the leak audit keys off.
  --advertise URL            The Raft URL peers should dial, e.g.
                             http://10.0.0.4:9001. Required with ec2-tag: it
                             cannot be derived from --bind, which is normally
                             0.0.0.0, and a host advertising that would tell
                             every peer to connect to itself.
  --discovery-expect N       How many hosts must appear, this one included.
                             Required with ec2-tag. Without it discovery would
                             proceed with whatever it found and the cluster
                             would run a replica short with nothing saying so.
  --discovery-budget MS      Time allowed to converge (default 120000). On
                             expiry the host REFUSES TO START and names the
                             ids it never saw.
  --discovery-role-prefix P  Only instances whose role tag starts with P are
                             peers. Shape 2 runs N hosts and one driver under
                             one run tag, and the driver is not a replica.
  --discovery-control-port N Every discovered peer's control port, for the
                             network probe. STATED, never derived from the
                             Raft URL by a convention — omitted, peer control
                             addresses stay empty and the probe reports null
                             rather than measuring whatever is listening.
  --discovery-region REGION  Region for the discovery client.
  --discovery-endpoint URL   EC2 endpoint override, e.g. a LocalStack at
                             http://localhost:4566. Exists so discovery can be
                             exercised end to end with no cloud account. The
                             SDK's AWS_ENDPOINT_URL_EC2 variable is NOT honoured
                             by the vendored version — setting it sends the
                             request to real EC2, which then fails with
                             AuthFailure and looks like a credentials problem.
  --discovery-instance-id ID This instance's id. Empty asks IMDS.

Elastic shard capacity (.kiro/specs/elastic-shard-capacity/, task 16)
  --capacity-role ROLE       off | controller | member (default off). Off is
                             the measurement host, unchanged. A controller runs
                             the capacity controller over docker_quorum_manager
                             and serves its control plane; a member's
                             placement-driver hooks call that control plane.
                             Needs --transport httplib --serializer json.
  --capacity-port P          The controller's control-plane port (default 7003).
                             It listens on --bind, like the other surfaces.
  --capacity-token-file F    A file holding the control plane's shared bearer
                             token (16+ characters). $KYTHIRA_CAPACITY_TOKEN
                             is the alternative; the file wins if both are
                             set. The controller requires it on every request
                             and refuses to start on a non-loopback --bind
                             without one; a member sends it. The controller
                             also hands it to every machine it creates.
  --capacity-controller H:P  Where a member reaches the controller.
  --capacity-cluster NAME    docker_quorum_manager's cluster name.
  --capacity-network NAME    The container network new machines join.
  --capacity-image IMAGE     The image new machines run.
  --capacity-docker-url URL  The container API (default
                             unix:///var/run/docker.sock; a rootless Podman
                             socket mounted there works too).
  --capacity-max-nodes N     The controller's ceiling; 0 derives 3x the floor.
  --capacity-shards-high X   Scale out above X shards per node (default 200).
  --capacity-shards-low X    Scale in at or below X (default 80).
  --capacity-sustained MS    How long a crossing must last (default 300000).
  --capacity-heartbeat MS    Placement-driver heartbeat (default 1000).
  --capacity-dry-run 0|1     Decide and log only (default 0).
  --capacity-join-env K=V    Environment for each new machine. Repeatable.
  --split-keys N             Split a shard past N keys; 0 is off (default 0).
  --groups 0                 Allowed for a member only: a new machine starts
                             empty and its replicas arrive by lazy creation.

  --config FILE              Read any of the above as `key=value` lines.
  --help                     This text.
)" + s3_usage();
}

namespace {

[[nodiscard]] auto parse_capacity_role(const std::string& value) -> capacity_role {
    if (value == "off") {
        return capacity_role::off;
    }
    if (value == "controller") {
        return capacity_role::controller;
    }
    if (value == "member") {
        return capacity_role::member;
    }
    throw std::runtime_error(
        "multi_raft_node: --capacity-role must be off, controller or member, got " + value);
}

[[nodiscard]] auto to_double(const std::string& s, const char* what) -> double {
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used == s.size()) {
            return v;
        }
    } catch (const std::exception&) {
    }
    throw std::runtime_error(std::string("multi_raft_node: ") + what + " is not a number: " + s);
}

[[nodiscard]] auto parse_discovery(const std::string& value) -> discovery_mode {
    if (value == "static" || value == "static-list") {
        return discovery_mode::static_list;
    }
    if (value == "ec2-tag" || value == "ec2_tag") {
        return discovery_mode::ec2_tag;
    }
    throw std::runtime_error("multi_raft_node: --discovery must be static or ec2-tag, got " +
                             value);
}

/// The whole file, less trailing whitespace: an editor's final newline is not
/// part of the secret, and a token that silently included one would match no
/// member's.
[[nodiscard]] auto read_token_file(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("multi_raft_node: cannot read --capacity-token-file " + path);
    }
    std::string token((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())) != 0) {
        token.pop_back();
    }
    if (token.empty()) {
        throw std::runtime_error("multi_raft_node: --capacity-token-file " + path + " is empty");
    }
    return token;
}

}  // namespace

auto parse_node_options(int argc, char** argv) -> node_options {
    node_options out;

    // One applier, shared by the command line and the file, so an option can
    // never be accepted by one and rejected by the other.
    std::function<void(node_options&, const std::string&, const std::string&)> apply =
        [&apply](node_options& o, const std::string& flag, const std::string& value) {
            if (apply_s3_flag(o._s3, flag, value)) {
                return;
            }
            if (flag == "--node-id") {
                // metadata:<provider> is resolved here, so a host that cannot
                // learn its id refuses to start with the metadata error.
                auto id = kythira::parse_host_node_id(value);
                if (!id) {
                    throw std::runtime_error("multi_raft_node: --node-id: " + id.error());
                }
                o._node_id = std::move(id).value();
            } else if (flag == "--bind") {
                o._bind_address = value;
            } else if (flag == "--raft-port") {
                o._raft_port = static_cast<std::uint16_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--data-port") {
                o._data_port = static_cast<std::uint16_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--control-port") {
                o._control_port = static_cast<std::uint16_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--peer") {
                add_peer(o, value);
            } else if (flag == "--peer-control") {
                add_peer_control(o, value);
            } else if (flag == "--peers-from") {
                apply_file(o, value, apply);
            } else if (flag == "--voters") {
                add_voters(o, value);
            } else if (flag == "--groups") {
                o._groups = static_cast<std::size_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--key-count") {
                o._key_count = to_u64(value, flag.c_str());
            } else if (flag == "--tick-interval") {
                o._tick_interval = to_ms(value, flag.c_str());
            } else if (flag == "--transport") {
                o._transport = parse_transport(value);
            } else if (flag == "--serializer") {
                o._serializer = parse_serializer(value);
            } else if (flag == "--persistence") {
                o._persistence = parse_persistence(value);
            } else if (flag == "--data-dir") {
                o._data_dir = value;
            } else if (flag == "--data-threads") {
                o._data_threads = static_cast<std::size_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--executor-stripes") {
                o._executor_stripes = static_cast<std::size_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--election-timeout-min") {
                o._election_timeout_min = to_ms(value, flag.c_str());
            } else if (flag == "--election-timeout-max") {
                o._election_timeout_max = to_ms(value, flag.c_str());
            } else if (flag == "--heartbeat-interval") {
                o._heartbeat_interval = to_ms(value, flag.c_str());
            } else if (flag == "--rpc-timeout") {
                o._rpc_timeout = to_ms(value, flag.c_str());
            } else if (flag == "--op-timeout") {
                o._op_timeout = to_ms(value, flag.c_str());
            } else if (flag == "--ready-timeout") {
                o._ready_timeout = to_ms(value, flag.c_str());
            } else if (flag == "--discovery") {
                o._discovery = parse_discovery(value);
            } else if (flag == "--discovery-run-tag") {
                o._discovery_run_tag = value;
            } else if (flag == "--discovery-region") {
                o._discovery_region = value;
            } else if (flag == "--discovery-role-prefix") {
                o._discovery_role_prefix = value;
            } else if (flag == "--discovery-expect") {
                o._discovery_expect = static_cast<std::size_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--discovery-budget") {
                o._discovery_budget = to_ms(value, flag.c_str());
            } else if (flag == "--discovery-control-port") {
                o._discovery_control_port = static_cast<std::uint16_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--discovery-endpoint") {
                o._discovery_endpoint = value;
            } else if (flag == "--discovery-instance-id") {
                o._discovery_instance_id = value;
            } else if (flag == "--advertise") {
                o._advertise_address = value;
            } else if (flag == "--capacity-role") {
                o._capacity_role = parse_capacity_role(value);
            } else if (flag == "--capacity-port") {
                o._capacity_port = static_cast<std::uint16_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--capacity-token-file") {
                o._capacity_token = read_token_file(value);
            } else if (flag == "--capacity-controller") {
                o._capacity_controller = value;
            } else if (flag == "--capacity-cluster") {
                o._capacity_cluster = value;
            } else if (flag == "--capacity-network") {
                o._capacity_network = value;
            } else if (flag == "--capacity-image") {
                o._capacity_image = value;
            } else if (flag == "--capacity-docker-url") {
                o._capacity_docker_url = value;
            } else if (flag == "--capacity-max-nodes") {
                o._capacity_max_nodes = static_cast<std::size_t>(to_u64(value, flag.c_str()));
            } else if (flag == "--capacity-shards-high") {
                o._capacity_shards_high = to_double(value, flag.c_str());
            } else if (flag == "--capacity-shards-low") {
                o._capacity_shards_low = to_double(value, flag.c_str());
            } else if (flag == "--capacity-sustained") {
                o._capacity_sustained = to_ms(value, flag.c_str());
            } else if (flag == "--capacity-heartbeat") {
                o._capacity_heartbeat = to_ms(value, flag.c_str());
            } else if (flag == "--capacity-dry-run") {
                o._capacity_dry_run = to_u64(value, flag.c_str()) != 0;
            } else if (flag == "--capacity-join-env") {
                o._capacity_join_env.push_back(value);
            } else if (flag == "--split-keys") {
                o._split_keys = static_cast<std::size_t>(to_u64(value, flag.c_str()));
            } else {
                throw std::runtime_error("multi_raft_node: unknown option " + flag);
            }
        };

    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            throw std::invalid_argument("help");
        }
        if (i + 1 >= argc) {
            throw std::runtime_error("multi_raft_node: " + flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (flag == "--config") {
            apply_file(out, value, apply);
        } else {
            apply(out, flag, value);
        }
    }

    // Validation, here rather than at the first use: a host that starts and
    // then fails to route is much harder to diagnose than one that refuses to
    // start and says which option was missing.
    if (out._raft_port == 0 || out._data_port == 0 || out._control_port == 0) {
        throw std::runtime_error(
            "multi_raft_node: --raft-port, --data-port and --control-port are all required");
    }
    if (out._discovery == discovery_mode::static_list) {
        if (out._peers.find(out._node_id) == out._peers.end()) {
            throw std::runtime_error(
                "multi_raft_node: --peer must include this host's own id; the transport's URL map "
                "is used for every target including self");
        }
    } else {
        // Under discovery the peer list is empty here BY DESIGN — it is filled
        // in before the transport is built. What the host must be given
        // instead is checked with the same "refuse at startup, not at first
        // use" rule: a host that starts and then cannot route is far harder to
        // diagnose than one that says which option was missing.
        if (out._discovery_run_tag.empty()) {
            throw std::runtime_error(
                "multi_raft_node: --discovery ec2-tag requires --discovery-run-tag; an empty run "
                "tag matches no instances and is indistinguishable from a cluster that has not "
                "started yet");
        }
        if (out._advertise_address.empty()) {
            throw std::runtime_error(
                "multi_raft_node: --discovery ec2-tag requires --advertise, the Raft URL peers "
                "should dial. It cannot be derived from --bind, which is normally 0.0.0.0 — a "
                "host advertising that would tell every peer to connect to itself");
        }
        if (out._discovery_expect == 0) {
            throw std::runtime_error(
                "multi_raft_node: --discovery ec2-tag requires --discovery-expect, the number of "
                "hosts that must appear. Without it discovery would proceed with whatever it "
                "found and measure a cluster with a replica missing");
        }
    }
    if (out._voters.empty()) {
        // Every peer, which is what Tier C wants and what the in-process rows
        // do. Named rather than left implicit so a reader of the row knows.
        for (const auto& [id, url] : out._peers) {
            out._voters.push_back(id);
        }
    }
    // A member may start empty: that is what a machine the controller just
    // provisioned is, and its replicas arrive by lazy creation.
    if (out._groups == 0 && out._capacity_role != capacity_role::member) {
        throw std::runtime_error(
            "multi_raft_node: --groups must be at least 1 (0 only with --capacity-role member)");
    }
    if (out._capacity_role == capacity_role::member && out._capacity_controller.empty()) {
        throw std::runtime_error(
            "multi_raft_node: --capacity-role member requires --capacity-controller HOST:PORT");
    }
    if (out._capacity_role == capacity_role::controller &&
        (out._capacity_cluster.empty() || out._capacity_network.empty() ||
         out._capacity_image.empty())) {
        throw std::runtime_error(
            "multi_raft_node: --capacity-role controller requires --capacity-cluster, "
            "--capacity-network and --capacity-image: they are what a new machine is made of");
    }
    if (out._capacity_token.empty()) {
        if (const char* env = std::getenv(k_capacity_token_env); env != nullptr) {
            out._capacity_token = env;
        }
    }
    if (!out._capacity_token.empty() && out._capacity_token.size() < k_min_capacity_token_size) {
        throw std::runtime_error(
            "multi_raft_node: the capacity token must be at least 16 characters; a shorter one is "
            "guessable by anyone who can reach the control plane");
    }
    if (out._capacity_role != capacity_role::off &&
        (out._transport != node_transport::httplib || out._serializer != wire_serializer::json)) {
        throw std::runtime_error(
            "multi_raft_node: --capacity-role needs --transport httplib --serializer json, the "
            "one stack the control plane is compiled into");
    }
    validate_s3(out);
    if ((out._persistence == persistence_mode::file_buffered ||
         out._persistence == persistence_mode::file_barrier) &&
        out._data_dir.empty()) {
        throw std::runtime_error(
            "multi_raft_node: --persistence file-* requires --data-dir; a file-backed log with "
            "nowhere to live would silently become a memory one");
    }
    return out;
}

}  // namespace kythira::bench
