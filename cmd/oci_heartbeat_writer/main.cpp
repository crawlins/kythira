/// @file main.cpp
/// @brief The on-instance heartbeat process (Requirement 4.4).
///
/// Runs ON an OCI compute instance, under Instance Principal auth, stamping
/// the instance's own `kythira-last-heartbeat` freeform tag on an interval —
/// the application-level liveness signal
/// `oci_instance_pool_quorum_manager::assess_quorum` reads (Requirement 5.3).
/// This binary is what a real kythira node embeds `oci_heartbeat_writer` for;
/// as a standalone process it exists so the heartbeat path can be exercised
/// and operated without standing up a full raft node, and it is the artifact
/// the CI pipeline ships to a pool instance for the first on-instance
/// verification of `oci_federation.hpp`'s Instance Principal signer.
///
/// Configuration is deliberately thin: everything an instance needs beyond
/// its own metadata service is the region (spares the signer a metadata
/// round-trip) and the cadence.
///
///   --region <name>       else $KYTHIRA_OCI_REGION, else derived from the
///                         metadata service by the signer
///   --interval <seconds>  default 10 — three missed beats inside
///                         assess_quorum's default 30s heartbeat_timeout
///   --instance-id <ocid>  default: discovered from /opc/v2/instance/id
///   --once                one synchronous write, exit 0/1 by its outcome
///
/// Runs until SIGTERM/SIGINT, printing a status line every 30s so the boot
/// log shows liveness without needing the control plane. Exit codes: 0 clean
/// shutdown (or a successful --once), 1 a failed --once or bad usage.

#include <raft/oci_heartbeat_writer.hpp>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

namespace {

auto usage(const char* argv0) -> void {
    std::cerr << "usage: " << argv0
              << " [--region <name>] [--interval <seconds>] [--instance-id <ocid>] [--once]\n";
}

}  // namespace

auto main(int argc, char** argv) -> int {
    kythira::oci_heartbeat_writer_config cfg;
    cfg.client.use_instance_principal = true;
    if (const char* region = std::getenv("KYTHIRA_OCI_REGION"); region != nullptr) {
        cfg.client.region = region;
    }
    bool once = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--region" && i + 1 < argc) {
            cfg.client.region = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            cfg.interval = std::chrono::seconds{std::atoi(argv[++i])};
            if (cfg.interval.count() <= 0) {
                usage(argv[0]);
                return 1;
            }
        } else if (arg == "--instance-id" && i + 1 < argc) {
            cfg.instance_id = argv[++i];
        } else if (arg == "--once") {
            once = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    const auto interval_seconds = cfg.interval.count();
    kythira::oci_heartbeat_writer writer(std::move(cfg));

    if (once) {
        try {
            const auto stamped = writer.write_once();
            std::cout << "[oci-heartbeat] wrote kythira-last-heartbeat=" << stamped << "\n";
            return 0;
        } catch (const std::exception& ex) {
            std::cerr << "[oci-heartbeat] write failed: " << ex.what() << "\n";
            return 1;
        }
    }

    // Block the shutdown signals BEFORE the writer thread starts, so the
    // mask is inherited and sigtimedwait() below is the only consumer --
    // a signal delivered to the writer's own thread would otherwise kill
    // the process with the default disposition.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    writer.start();
    std::cout << "[oci-heartbeat] started, interval=" << interval_seconds << "s\n" << std::flush;

    for (;;) {
        timespec wait{.tv_sec = 30, .tv_nsec = 0};
        const int sig = sigtimedwait(&signals, nullptr, &wait);
        if (sig == SIGTERM || sig == SIGINT) {
            break;
        }
        // Timeout: emit the liveness line. writes()/write_failures() are the
        // writer's own counters; last_error() empties on any later success.
        std::cout << "[oci-heartbeat] writes=" << writer.writes()
                  << " failures=" << writer.write_failures();
        if (const auto err = writer.last_error(); !err.empty()) {
            std::cout << " last_error=" << err;
        }
        std::cout << "\n" << std::flush;
    }

    writer.stop();
    std::cout << "[oci-heartbeat] stopped cleanly, total writes=" << writer.writes() << "\n";
    return 0;
}
