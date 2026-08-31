// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

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

/// `id=url` — the form both `--peer` and `--peers-from` use, so a Tier E
/// deployment can move a command line into a file without translating it.
auto add_peer(node_options& out, const std::string& spec) -> void {
    const auto eq = spec.find('=');
    if (eq == std::string::npos) {
        throw std::runtime_error("multi_raft_node: --peer expects <id>=<url>, got: " + spec);
    }
    out._peers[to_u64(spec.substr(0, eq), "--peer id")] = spec.substr(eq + 1);
}

auto add_peer_control(node_options& out, const std::string& spec) -> void {
    const auto eq = spec.find('=');
    if (eq == std::string::npos) {
        throw std::runtime_error(
            "multi_raft_node: --peer-control expects <id>=<host>:<port>, got: " + spec);
    }
    out._peer_control[to_u64(spec.substr(0, eq), "--peer-control id")] = spec.substr(eq + 1);
}

auto add_voters(node_options& out, const std::string& spec) -> void {
    out._voters.clear();
    std::istringstream in(spec);
    std::string part;
    while (std::getline(in, part, ',')) {
        if (!part.empty()) {
            out._voters.push_back(to_u64(part, "--voters"));
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
    throw std::runtime_error(
        "multi_raft_node: --persistence must be memory|file-buffered|file-barrier, got: " + s);
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

auto node_usage() -> std::string {
    return R"(multi_raft_node — a measurement host for `multi_raft`.

NOT A SUPPORTED SERVER. This binary exists so that
.kiro/specs/multi-raft-performance/ can take Tier C and Tier E rows — one host
process per node — and it is deliberately kept out of any install target. It
has no authentication, no authorisation, no multi-tenancy and no rate limiting,
and that is a decision rather than an omission: every one of them would add
cost to the measured path that the numbers would then have to be corrected
for, and none of them is what is being measured. Do not deploy it.

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
  --persistence MODE         memory | file-buffered | file-barrier (default
                             memory). file-buffered is NOT DURABLE.
  --data-dir PATH            Where a file-backed log lives.
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

  --config FILE              Read any of the above as `key=value` lines.
  --help                     This text.
)";
}

auto parse_node_options(int argc, char** argv) -> node_options {
    node_options out;

    // One applier, shared by the command line and the file, so an option can
    // never be accepted by one and rejected by the other.
    std::function<void(node_options&, const std::string&, const std::string&)> apply =
        [&apply](node_options& o, const std::string& flag, const std::string& value) {
            if (flag == "--node-id") {
                o._node_id = to_u64(value, flag.c_str());
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
    if (out._peers.find(out._node_id) == out._peers.end()) {
        throw std::runtime_error(
            "multi_raft_node: --peer must include this host's own id; the transport's URL map is "
            "used for every target including self");
    }
    if (out._voters.empty()) {
        // Every peer, which is what Tier C wants and what the in-process rows
        // do. Named rather than left implicit so a reader of the row knows.
        for (const auto& [id, url] : out._peers) {
            out._voters.push_back(id);
        }
    }
    if (out._groups == 0) {
        throw std::runtime_error("multi_raft_node: --groups must be at least 1");
    }
    if (out._persistence != persistence_mode::memory && out._data_dir.empty()) {
        throw std::runtime_error(
            "multi_raft_node: --persistence file-* requires --data-dir; a file-backed log with "
            "nowhere to live would silently become a memory one");
    }
    return out;
}

}  // namespace kythira::bench
