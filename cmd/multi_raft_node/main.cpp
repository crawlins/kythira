// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file main.cpp
/// @brief `multi_raft_node` — one process hosting one `multi_raft`, serving a
///        client-facing key-value data path.
///        `.kiro/specs/multi-raft-host-binary/` Requirement 1.
///
/// **This is a measurement and testing host, not a supported server**
/// (Requirement 6.1). It exists because every performance row this project has
/// ever produced is Tier A or Tier B — every host inside one process — and
/// `.kiro/specs/multi-raft-performance/` Requirement 3.3 forbids a
/// like-for-like comparison against an external number from any tier below C.
/// One host per process is the whole difference.
///
/// It has no authentication, no authorisation, no multi-tenancy and no rate
/// limiting, and Requirement 6.2 asks that the reason be said rather than
/// implied: each of them would add cost to the measured path that every number
/// would then have to be corrected for, and none of them is what is being
/// measured. It is kept out of every install target for the same reason.
///
/// **The workload vocabulary is shared, not reimplemented.**
/// `tests/multi_raft_kv_workload.hpp` is on this target's include path
/// deliberately: `kv_key`, `kv_shard_ranges` and `kv_partitioner` define how
/// the key space is spelled and tiled, and a second copy of any of them in
/// `cmd/` would make every Tier B → Tier C delta a comparison of two workloads
/// rather than of two tiers. That is the failure Requirement 4.1 names, and
/// this include is what prevents it.
///
/// **The four `multi_raft` instantiations are not here.** They are one per
/// `(transport × wire serializer)` pair in `run_httplib_json.cpp`,
/// `run_httplib_cbor.cpp`, `run_beast_json.cpp` and `run_beast_cbor.cpp`,
/// behind the declarations in `host_runners.hpp`. Compiling all four here made
/// this the heaviest translation unit in the tree at 10,974 MiB of compiler RSS
/// and broke CI's 16 GiB runner; `host_stacks.hpp` records the measurement.
/// What is left in this file is the part that is cheap to compile and has to
/// happen exactly once: the command line, discovery, `folly::Init`, and the
/// signal handlers.

#include "config.hpp"
#include "host_runners.hpp"
#include "stop_signal.hpp"

#include <folly/init/Init.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#ifdef KYTHIRA_HAS_AWS_SDK
#include <raft/aws_ec2_peer_discovery.hpp>

#include <aws/core/Aws.h>
#endif

namespace kythira::bench::host {

std::atomic<bool> g_stop{false};
std::mutex g_stop_mu;
std::condition_variable g_stop_cv;

void on_signal(int) {
    g_stop.store(true);
    g_stop_cv.notify_all();
}

void wait_for_stop() {
    std::unique_lock lock(g_stop_mu);
    g_stop_cv.wait(lock, [] { return g_stop.load(); });
}

}  // namespace kythira::bench::host

namespace {

using kythira::bench::node_options;
using kythira::bench::node_transport;
using kythira::bench::wire_serializer;

/// Not wired here. Proxygen's server needs a caller-owned
/// `folly::IOThreadPoolExecutor` and a shutdown sequence this binary does not
/// yet implement, and a transport that half-works in a measurement host would
/// produce rows nobody could trust. Refused loudly rather than silently
/// substituted.
[[nodiscard]] auto refuse_proxygen() -> int {
    std::cerr << "multi_raft_node: --transport proxygen is not implemented in this host; "
                 "use httplib or beast\n";
    return 2;
}

// ─────────────────────────────────────────────────────────────────────────────
// Discovery
// ─────────────────────────────────────────────────────────────────────────────
//
// `.kiro/specs/multi-machine-placement/` Requirement 3: bring N hosts up
// without every one of them being handed every address.
//
// This runs ONCE, BEFORE the transport is built, and never again. That is the
// whole design and it is what keeps Requirement 3.4 true: the peer list a
// measured window uses is a fixed map either way, and the only difference
// between `--discovery static` and `--discovery ec2-tag` is who wrote it.
// Re-scanning during the window would put a control-plane call on the data
// path and the row would be measuring EC2.
//
// A failure here refuses to start. The alternative — carry on with the peers
// that did answer — runs the cluster a replica short, and nothing downstream
// can tell that from a healthy row.
void resolve_peers_by_discovery(node_options& opt) {
    if (opt._discovery == kythira::bench::discovery_mode::static_list) {
        return;
    }
#ifndef KYTHIRA_HAS_AWS_SDK
    throw std::runtime_error(
        "multi_raft_node: --discovery ec2-tag needs the AWS SDK, and this binary was built "
        "without it. Rebuild with the SDK available, or use --discovery static with --peer");
#else
    Aws::SDKOptions sdk_options;
    Aws::InitAPI(sdk_options);
    // The SDK is shut down on every path out of here, including the throwing
    // ones: a half-initialised SDK left behind by a failed discovery would
    // outlive the reason it was created.
    struct sdk_guard {
        Aws::SDKOptions& o;
        ~sdk_guard() { Aws::ShutdownAPI(o); }
    } guard{sdk_options};

    kythira::aws_ec2_peer_discovery_config cfg;
    cfg.aws.region = opt._discovery_region;
    cfg.aws.endpoint_override = opt._discovery_endpoint;
    cfg.run_tag_value = opt._discovery_run_tag;
    cfg.role_tag_prefix = opt._discovery_role_prefix;
    cfg.instance_id = opt._discovery_instance_id;

    kythira::aws_ec2_peer_discovery<std::uint64_t, std::string> finder{cfg};

    std::cerr << "multi_raft_node: registering node " << opt._node_id << " at "
              << opt._advertise_address << " under tag " << opt._discovery_run_tag << "\n";
    finder.register_node(opt._node_id, opt._advertise_address).get();

    std::cerr << "multi_raft_node: waiting for " << opt._discovery_expect << " peer(s), budget "
              << opt._discovery_budget.count() << " ms\n";
    // No `required` list is passed: the ids are precisely what is not known in
    // advance here, which is the point of discovery. The count is the contract,
    // and await_peers still names every peer it DID see when it gives up.
    const auto peers = finder.await_peers(opt._discovery_expect, opt._discovery_budget);

    opt._peers.clear();
    opt._peer_control.clear();
    for (const auto& peer : peers) {
        opt._peers.emplace(peer.node_id, peer.address);
        if (opt._discovery_control_port != 0) {
            // Host part of the advertised Raft URL, with the operator's stated
            // control port. Not a "control is Raft plus two" convention: the
            // port is given explicitly, and omitted it stays absent so the
            // probe reports null rather than measuring whatever is listening.
            const auto scheme = peer.address.find("://");
            auto host =
                scheme == std::string::npos ? peer.address : peer.address.substr(scheme + 3);
            const auto colon = host.rfind(':');
            if (colon != std::string::npos) {
                host = host.substr(0, colon);
            }
            opt._peer_control.emplace(peer.node_id,
                                      host + ":" + std::to_string(opt._discovery_control_port));
        }
    }

    if (opt._peers.find(opt._node_id) == opt._peers.end()) {
        // The one failure discovery can produce that looks like success: every
        // OTHER host answered, so the count is met, but this host's own tags
        // are missing from the result. The transport's URL map is used for
        // every target including self, so a host absent from its own map
        // cannot route to itself.
        throw std::runtime_error(
            "multi_raft_node: discovery returned " + std::to_string(opt._peers.size()) +
            " peer(s) but not this host (id " + std::to_string(opt._node_id) +
            "). The transport's URL map is used for every target including self");
    }

    if (opt._voters.empty()) {
        for (const auto& [id, url] : opt._peers) {
            opt._voters.push_back(id);
        }
    }

    std::cerr << "multi_raft_node: discovered " << opt._peers.size() << " peer(s):";
    for (const auto& [id, url] : opt._peers) {
        std::cerr << " " << id << "=" << url;
    }
    std::cerr << "\n";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch
// ─────────────────────────────────────────────────────────────────────────────
//
// The `(transport × serializer)` fan-out that used to be two nested function
// templates instantiated here. It is a flat switch now because each arm is a
// call into a translation unit that already holds exactly one instantiation —
// the templates themselves live in `host_stacks.hpp` and are instantiated in
// `run_*.cpp`. Behaviourally identical, including which combinations exist.
[[nodiscard]] auto dispatch(const node_options& opt) -> int {
    switch (opt._serializer) {
        case wire_serializer::json:
            switch (opt._transport) {
                case node_transport::httplib:
                    return kythira::bench::host::run_httplib_json(opt);
                case node_transport::beast:
#if defined(KYTHIRA_BENCH_HAS_BEAST)
                    return kythira::bench::host::run_beast_json(opt);
#else
                    std::cerr << "multi_raft_node: --transport beast was not compiled in\n";
                    return 2;
#endif
                case node_transport::proxygen:
                    return refuse_proxygen();
            }
            return 2;
        case wire_serializer::cbor:
            switch (opt._transport) {
                case node_transport::httplib:
                    return kythira::bench::host::run_httplib_cbor(opt);
                case node_transport::beast:
#if defined(KYTHIRA_BENCH_HAS_BEAST)
                    return kythira::bench::host::run_beast_cbor(opt);
#else
                    std::cerr << "multi_raft_node: --transport beast was not compiled in\n";
                    return 2;
#endif
                case node_transport::proxygen:
                    return refuse_proxygen();
            }
            return 2;
    }
    return 2;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    // **Parsed before `folly::Init`, and folly is then given an argv holding
    // only the program name.** `folly::Init` hands the real one to gflags,
    // which rejects every flag it does not recognise — and every flag here is
    // one it does not recognise. `chaos_node` sidesteps this by taking its
    // configuration from the environment; this binary has a command line
    // because Requirement 1.2 asks for one, so it keeps gflags away from it.
    node_options opt;
    try {
        opt = kythira::bench::parse_node_options(argc, argv);
    } catch (const std::invalid_argument&) {
        std::cout << kythira::bench::node_usage();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n\n" << kythira::bench::node_usage();
        return 2;
    }

    // Before folly::Init and before any transport: discovery only fills in the
    // peer map, and everything downstream must see a map that is already
    // final.
    try {
        resolve_peers_by_discovery(opt);
    } catch (const std::exception& e) {
        std::cerr << "multi_raft_node: discovery failed: " << e.what() << "\n";
        return 3;
    }

    int folly_argc = 1;
    char* folly_argv_storage[] = {argv[0], nullptr};
    char** folly_argv = folly_argv_storage;
    folly::Init init(&folly_argc, &folly_argv);

    std::signal(SIGINT, kythira::bench::host::on_signal);
    std::signal(SIGTERM, kythira::bench::host::on_signal);

    try {
        return dispatch(opt);
    } catch (const std::exception& e) {
        std::cerr << "multi_raft_node: " << e.what() << "\n";
        return 1;
    }
}
