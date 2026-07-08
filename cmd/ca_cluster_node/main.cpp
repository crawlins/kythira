// ca_cluster_node — a Kythira Raft cluster member replicating a CA's root
// material, issuance ledger, and revocation list via `ca_state_machine`, and
// exposing the same client-facing HTTP API as `ca_service --serve`
// (/healthz, /v1/root-ca, /v1/certificates(+/renew), /v1/certificates/revoke,
// /v1/crl) on a port separate from the Raft-internal RPC port.
//
// Usage:
//   ca_cluster_node --node-id <n> --rpc-port <n> --http-port <n>
//                    --data-dir <path> --unseal-key-file <path>
//                    --peers <id>:<rpc_host>:<rpc_port>@<http_address>[,...]
//                    [--rpc-address <addr>] [--bootstrap-ca]
//                    [--auth-token <token>] [--tls-cert <path> --tls-key <path>]
//
// Every node in the cluster SHALL be started with the SAME --unseal-key-file
// contents (Requirement 17.4) — losing it makes the persisted CA key
// unrecoverable. --bootstrap-ca SHALL be given to exactly one node, exactly
// once per cluster lifetime; every other node discovers the CA's root
// material via ordinary Raft log replication / snapshot installation.
//
// Non-leader nodes respond 308 (redirecting to the current leader's
// http_address, preserving method and body) or 503 ({"error":
// "no_known_leader"}) to every /v1/* request (Requirement 17.7).

#include "config.hpp"

#include <raft/ca_http_helpers.hpp>
#include <raft/ca_state_machine.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/ssl.h>
#endif

#include <folly/init/Init.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

// Extends tcp_raft_types (real TCP RPC transport, file-backed persistence,
// JSON serialisation — the same components cmd/chaos_node uses) with the
// CA-specific state machine.
struct ca_cluster_raft_types : kythira::tcp_raft_types {
    using state_machine_type = raft::testing::ca_state_machine;
};

using raft_node_t = kythira::node<ca_cluster_raft_types>;

std::atomic<bool> g_stop{false};
std::mutex g_stop_mu;
std::condition_variable g_stop_cv;
void sigterm_handler(int) {
    g_stop = true;
    g_stop_cv.notify_all();
}

std::atomic<httplib::Server*> g_http_server{nullptr};
void on_http_signal(int) {
    auto* srv = g_http_server.load();
    if (srv != nullptr) srv->stop();
}

auto read_unseal_key(const std::string& path) -> std::string {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open --unseal-key-file " + path);
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) throw std::runtime_error("--unseal-key-file " + path + " is empty");
    return line;
}

// Performs a linearisable read of the replicated CA state and deserializes it
// into a standalone ca_state_machine, so callers can inspect
// has_root_material()/root_certificate_pem()/encrypted_bootstrap_material()/
// ledger() without node<Types> exposing its private _state_machine member —
// this is exactly the seam read_state() exists for.
auto read_ca_state(raft_node_t& node, std::chrono::milliseconds timeout)
    -> raft::testing::ca_state_machine {
    auto bytes = node.read_state(timeout).get();
    raft::testing::ca_state_machine sm;
    sm.restore_from_snapshot(bytes, 0);
    return sm;
}

auto json_error(const std::string& error) -> std::string {
    return boost::json::serialize(boost::json::object{{"error", error}});
}

// Extracts the certificate's subject common name, for populating
// ca_ledger_entry.subject (the CSR-signing path never sees a distinguished
// name struct directly — only the resulting certificate PEM).
auto extract_subject_cn(const std::string& cert_pem) -> std::string {
    BIO* bio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) return {};
    char buf[256] = {};
    X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, buf, sizeof(buf));
    X509_free(cert);
    return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
    // folly::Init registers process-wide singletons (Timekeeper, used by
    // tcp_rpc_client's retry/backoff logic under real network conditions —
    // without it, a failed RPC's retry path aborts the process outright).
    // Called with just the program name so gflags' command-line parser never
    // sees this binary's own --node-id/--peers/etc. flags, which it doesn't
    // recognize (ca_cluster_node parses those itself, below).
    int folly_argc = 1;
    char* folly_argv_storage[] = {argv[0]};
    char** folly_argv = folly_argv_storage;
    folly::Init folly_init(&folly_argc, &folly_argv, false);

    ca_cluster_node::ca_cluster_node_config cfg;
    try {
        cfg = ca_cluster_node::config_from_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "ca_cluster_node: " << e.what() << "\n";
        return 1;
    }

    if (cfg.print_root_fingerprint) {
        // Requirement 19.2: prints and exits without binding any port or
        // touching Raft/CA state.
        std::ifstream f(cfg.tls_cert_path, std::ios::binary);
        if (!f) {
            std::cerr << "ca_cluster_node: cannot open --tls-cert " << cfg.tls_cert_path << "\n";
            return 1;
        }
        std::string bundle((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        try {
            auto root = raft::testing::root_cert_from_pem_bundle(bundle);
            std::cout << "ca_cluster_node: root certificate SHA-256 fingerprint: "
                      << raft::testing::sha256_fingerprint_hex(root.get()) << "\n";
        } catch (const std::exception& ex) {
            std::cerr << "ca_cluster_node: failed to compute root fingerprint: " << ex.what()
                      << "\n";
            return 1;
        }
        return 0;
    }

    std::string unseal_passphrase;
    try {
        unseal_passphrase = read_unseal_key(cfg.unseal_key_file);
    } catch (const std::exception& e) {
        std::cerr << "ca_cluster_node: " << e.what() << "\n";
        return 1;
    }

    kythira::raft_configuration raft_cfg;
    raft_cfg._election_timeout_min = cfg.election_timeout_min;
    raft_cfg._election_timeout_max = cfg.election_timeout_max;
    raft_cfg._heartbeat_interval = cfg.heartbeat_interval;

    kythira::tcp_rpc_server rpc_server(cfg.rpc_port);
    kythira::tcp_rpc_client rpc_client;
    for (const auto& p : cfg.peers) rpc_client.add_peer(p.node_id, p.rpc_host, p.rpc_port);
    kythira::file_persistence_engine<> persistence(cfg.data_dir);

    kythira::node_config<ca_cluster_raft_types> ncfg{
        .node_id = cfg.node_id,
        .network_client = std::move(rpc_client),
        .network_server = std::move(rpc_server),
        .persistence = std::move(persistence),
        .logger = kythira::console_logger{},
        .metrics = kythira::noop_metrics{},
        .membership = kythira::default_membership_manager<std::uint64_t>{},
        .config = raft_cfg,
    };

    raft_node_t raft_node(std::move(ncfg));
    raft_node.set_cluster_configuration(cfg.all_node_ids());

    std::cerr << "[info] ca_cluster_node starting: id=" << cfg.node_id << " rpc=" << cfg.rpc_port
              << " http=" << cfg.http_port << " peers=" << cfg.peers.size()
              << " bootstrap_ca=" << (cfg.bootstrap_ca ? "true" : "false") << "\n";

    raft_node.start();

    // ── Leader-held in-memory signer (Requirement 17.9) ─────────────────────
    // Reconstructed on becoming leader (or at startup, if already leader from
    // a prior run whose in-memory signer was lost to a restart), cleared on
    // losing leadership. The whole point of keeping non-deterministic
    // cryptography out of apply(): this signer is built ONCE per leadership
    // term from already-replicated, already-encrypted material.
    std::mutex signer_mu;
    std::unique_ptr<raft::testing::certificate_authority> signer;

    auto ensure_signer = [&] {
        {
            std::lock_guard lock(signer_mu);
            if (signer != nullptr) return;
        }
        if (!raft_node.is_leader()) return;

        raft::testing::ca_state_machine state;
        try {
            state = read_ca_state(raft_node, std::chrono::milliseconds(5000));
        } catch (const std::exception&) {
            return;  // lost leadership mid-read, or no quorum yet — retry next tick
        }
        if (!state.has_root_material()) return;

        std::string key_pem;
        try {
            key_pem = raft::testing::decrypt_ca_private_key(state.encrypted_bootstrap_material(),
                                                            unseal_passphrase);
        } catch (const std::invalid_argument& ex) {
            // Requirement 17.4/17.5: a decryption/authentication-tag failure
            // means either a misconfigured --unseal-key-file or a corrupted
            // replicated log — neither is recoverable by retrying, and
            // silently continuing with no signer would let this node sit as
            // an apparently-healthy leader that can never actually sign
            // anything. Fail loud.
            std::cerr << "[fatal] ca_cluster_node: failed to decrypt CA bootstrap material — wrong "
                         "--unseal-key-file or corrupted replicated state: "
                      << ex.what() << "\n";
            std::exit(1);
        }

        auto new_signer = std::make_unique<raft::testing::certificate_authority>(
            raft::testing::certificate_authority::from_existing(state.root_certificate_pem(),
                                                                key_pem));
        // The freshly-reconstructed signer has no memory of certificates a
        // prior leader issued or revoked — replay the replicated ledger's
        // revocations so crl_pem() is correct immediately, not just for
        // revocations this leadership term happens to make itself.
        for (const auto& entry : state.ledger()) {
            if (entry.revoked_at.has_value()) {
                new_signer->mark_revoked_externally(entry.serial, *entry.revoked_at);
            }
        }

        std::lock_guard lock(signer_mu);
        if (signer == nullptr) signer = std::move(new_signer);
    };

    // ── Bootstrap (Requirement 17.10): submitted at most once per cluster
    //    lifetime, only by the flagged node, only once it is leader and sees
    //    no root material yet. ────────────────────────────────────────────
    std::atomic<bool> bootstrap_done_or_unnecessary{false};

    auto maybe_bootstrap = [&] {
        if (!cfg.bootstrap_ca || bootstrap_done_or_unnecessary) return;
        if (!raft_node.is_leader()) return;

        raft::testing::ca_state_machine state;
        try {
            state = read_ca_state(raft_node, std::chrono::milliseconds(5000));
        } catch (const std::exception&) {
            return;
        }
        if (state.has_root_material()) {
            bootstrap_done_or_unnecessary =
                true;  // already done — by us in a prior tick, or by a prior leader
            return;
        }

        std::cerr << "[info] ca_cluster_node: bootstrapping fresh CA root (node " << cfg.node_id
                  << ")\n";
        raft::testing::certificate_authority fresh_ca;
        auto root_material = fresh_ca.export_root_material();
        auto encrypted_key =
            raft::testing::encrypt_ca_private_key(root_material.private_key_pem, unseal_passphrase);
        auto cmd = raft::testing::encode_bootstrap_ca_command(root_material.certificate_pem,
                                                              encrypted_key);
        try {
            raft_node.submit_command(cmd, std::chrono::milliseconds(30000)).get();
            std::cerr << "[info] ca_cluster_node: bootstrap_ca committed\n";
            bootstrap_done_or_unnecessary = true;
        } catch (const std::exception& ex) {
            std::cerr << "[warn] ca_cluster_node: bootstrap_ca submission failed, will retry: "
                      << ex.what() << "\n";
        }
    };

    // ── Background maintenance: leadership-triggered signer lifecycle ──────
    std::jthread maintenance_thread([&](std::stop_token st) {
        bool was_leader = false;
        while (!st.stop_requested() && !g_stop.load()) {
            bool is_leader_now = raft_node.is_leader();
            if (is_leader_now) {
                if (!was_leader) {
                    // Just became leader: commit one entry in this term first
                    // (Raft paper §5.4.2/Figure 8) so any already-committed
                    // entries left un-reapplied by a restart (or by winning
                    // an election over entries only a prior leader
                    // committed) are retroactively applied before this node
                    // starts answering client-facing requests as leader.
                    try {
                        raft_node
                            .submit_command(raft::testing::encode_noop_command(),
                                            std::chrono::milliseconds(30000))
                            .get();
                    } catch (const std::exception&) {
                        // Lost leadership before the no-op committed — fall
                        // through; the next tick re-checks is_leader().
                    }
                }
                maybe_bootstrap();
                ensure_signer();
            } else if (was_leader) {
                std::lock_guard lock(signer_mu);
                signer.reset();
            }
            was_leader = is_leader_now;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    });

    // ── Raft-internal timers (same pattern as cmd/chaos_node) ───────────────
    std::thread election_timer([&] {
        while (!g_stop) {
            std::this_thread::sleep_for(cfg.election_timeout_min / 2);
            if (!g_stop) raft_node.check_election_timeout();
        }
    });
    std::thread heartbeat_timer([&] {
        while (!g_stop) {
            std::this_thread::sleep_for(cfg.heartbeat_interval);
            if (!g_stop) raft_node.check_heartbeat_timeout();
        }
    });

    // ── Client-facing HTTP API ───────────────────────────────────────────────
    std::unique_ptr<httplib::Server> plain_server;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    std::unique_ptr<httplib::SSLServer> ssl_server;
#endif
    httplib::Server* server = nullptr;

    if (!cfg.tls_cert_path.empty()) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        ssl_server = std::make_unique<httplib::SSLServer>(cfg.tls_cert_path.c_str(),
                                                          cfg.tls_key_path.c_str());
        if (!ssl_server->is_valid()) {
            std::cerr << "ca_cluster_node: failed to initialize TLS with cert " << cfg.tls_cert_path
                      << " / key " << cfg.tls_key_path << "\n";
            return 1;
        }
        SSL_CTX_set_verify(ssl_server->ssl_context(), SSL_VERIFY_PEER,
                           raft::testing::accept_any_peer_certificate);
        server = ssl_server.get();
        // Requirement 19.1: printed once at startup — see ca_service's
        // identical rationale for why this matters for ca_bootstrap_client's
        // first-contact trust check.
        {
            std::ifstream fp_f(cfg.tls_cert_path, std::ios::binary);
            std::string fp_bundle((std::istreambuf_iterator<char>(fp_f)),
                                  std::istreambuf_iterator<char>());
            try {
                auto fp_root = raft::testing::root_cert_from_pem_bundle(fp_bundle);
                std::cerr << "ca_cluster_node: root certificate SHA-256 fingerprint: "
                          << raft::testing::sha256_fingerprint_hex(fp_root.get()) << "\n";
            } catch (const std::exception& ex) {
                std::cerr << "ca_cluster_node: WARNING: failed to compute root fingerprint: "
                          << ex.what() << "\n";
            }
        }
#else
        std::cerr
            << "ca_cluster_node: built without TLS support — cannot honor --tls-cert/--tls-key\n";
        return 1;
#endif
    } else {
        plain_server = std::make_unique<httplib::Server>();
        server = plain_server.get();
        std::cerr << "ca_cluster_node: WARNING: running without TLS (no --tls-cert/--tls-key "
                     "given) — suitable "
                     "only for a private network\n";
    }

    g_http_server.store(server);
    std::signal(SIGINT, on_http_signal);
    std::signal(SIGTERM, sigterm_handler);
    // SIGTERM triggers both: sigterm_handler() wakes the main wait below, and
    // that same shutdown path stops the HTTP server explicitly (see below) —
    // SIGINT is wired to stop the HTTP server directly for interactive use.

    const std::string bearer_prefix = "Bearer ";
    server->set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "POST" && req.path == "/v1/certificates/renew") {
            return httplib::Server::HandlerResponse::Unhandled;  // authenticates via mTLS in its
                                                                 // own handler
        }
        if (req.path == "/healthz") {
            return httplib::Server::HandlerResponse::Unhandled;  // health checks must work with no
                                                                 // credentials
        }
        auto auth = req.get_header_value("Authorization");
        if (auth != bearer_prefix + cfg.auth_token) {
            res.status = 401;
            res.set_content(json_error("unauthorized"), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Returns true iff this node should proceed to handle the request as
    // leader. Otherwise `res` has already been populated with a 308 redirect
    // or 503 no_known_leader per Requirement 17.7, and the caller must return.
    auto require_leader_or_redirect = [&](const httplib::Request& req,
                                          httplib::Response& res) -> bool {
        if (raft_node.is_leader()) return true;
        auto leader_id = raft_node.known_leader();
        if (leader_id.has_value()) {
            auto leader_http = cfg.http_address_for(*leader_id);
            if (leader_http.has_value()) {
                res.status = 308;
                res.set_header("Location", *leader_http + req.path);
                return false;
            }
        }
        res.status = 503;
        res.set_content(json_error("no_known_leader"), "application/json");
        return false;
    };

    server->Get("/healthz", [&](const httplib::Request&, httplib::Response& res) {
        res.status = raft_node.is_running() ? 200 : 503;
    });

    server->Get("/v1/root-ca", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_leader_or_redirect(req, res)) return;
        try {
            auto state = read_ca_state(raft_node, std::chrono::milliseconds(30000));
            if (!state.has_root_material()) {
                res.status = 503;
                res.set_content(json_error("not_bootstrapped"), "application/json");
                return;
            }
            res.set_content(state.root_certificate_pem(), "application/x-pem-file");
        } catch (const std::exception& ex) {
            res.status = 503;
            res.set_content(json_error(ex.what()), "application/json");
        }
    });

    server->Post("/v1/certificates", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_leader_or_redirect(req, res)) return;
        try {
            auto body = boost::json::parse(req.body).as_object();
            auto* csr_val = body.if_contains("csr_pem");
            if (csr_val == nullptr || !csr_val->is_string()) {
                res.status = 400;
                res.set_content(json_error("csr_pem is required"), "application/json");
                return;
            }
            std::string csr_pem = std::string(csr_val->as_string());
            auto options = raft::testing::parse_csr_signing_options(body);

            std::unique_lock signer_lock(signer_mu);
            if (signer == nullptr) {
                res.status = 503;
                res.set_content(json_error("not_ready"), "application/json");
                return;
            }
            // sign_csr() performs the non-deterministic OpenSSL signing here,
            // outside apply() — the state machine only ever commits the
            // already-computed result (Requirement 17.1/17.8).
            auto material = signer->sign_csr(csr_pem, options);
            signer_lock.unlock();

            raft::testing::ca_ledger_entry entry;
            entry.serial = material.serial;
            entry.subject = extract_subject_cn(material.certificate_pem);
            entry.dns_names = options.dns_names;
            entry.ip_addresses = options.ip_addresses;
            entry.certificate_pem = material.certificate_pem;
            entry.not_before = std::chrono::system_clock::now();
            entry.not_after = entry.not_before + options.validity;

            // HTTP response SHALL NOT be sent until submit_command()'s future
            // resolves (Requirement 17.8) — so a client never observes an
            // issuance a subsequent leader failover could "forget."
            raft_node
                .submit_command(raft::testing::encode_record_issuance_command(entry),
                                std::chrono::milliseconds(30000))
                .get();

            res.set_content(boost::json::serialize(raft::testing::pem_material_to_json(material)),
                            "application/json");
        } catch (const std::invalid_argument& ex) {
            res.status = 400;
            res.set_content(json_error(ex.what()), "application/json");
        } catch (const std::exception& ex) {
            res.status = 503;
            res.set_content(json_error(ex.what()), "application/json");
        }
    });

    server->Post("/v1/certificates/renew", [&](const httplib::Request& req,
                                               httplib::Response& res) {
        if (!require_leader_or_redirect(req, res)) return;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (req.ssl == nullptr) {
            res.status = 401;
            res.set_content(json_error("renew requires an mTLS connection"), "application/json");
            return;
        }
        X509* peer_cert = SSL_get1_peer_certificate(req.ssl);
        if (peer_cert == nullptr) {
            res.status = 401;
            res.set_content(json_error("no client certificate presented"), "application/json");
            return;
        }
        try {
            auto state = read_ca_state(raft_node, std::chrono::milliseconds(30000));
            if (!raft::testing::cert_chains_to_root(peer_cert, state.root_certificate_pem())) {
                X509_free(peer_cert);
                res.status = 401;
                res.set_content(
                    json_error("presented certificate does not chain to this CA's root"),
                    "application/json");
                return;
            }
            auto options = raft::testing::options_from_presented_cert(peer_cert);
            X509_free(peer_cert);

            auto body = boost::json::parse(req.body).as_object();
            auto* csr_val = body.if_contains("csr_pem");
            if (csr_val == nullptr || !csr_val->is_string()) {
                res.status = 400;
                res.set_content(json_error("csr_pem is required"), "application/json");
                return;
            }
            std::string csr_pem = std::string(csr_val->as_string());
            if (auto* v = body.if_contains("validity_days"); v && v->is_number()) {
                options.validity = std::chrono::hours(24 * v->to_number<int>());
            }

            std::unique_lock signer_lock(signer_mu);
            if (signer == nullptr) {
                res.status = 503;
                res.set_content(json_error("not_ready"), "application/json");
                return;
            }
            auto material = signer->sign_csr(csr_pem, options);
            signer_lock.unlock();

            raft::testing::ca_ledger_entry entry;
            entry.serial = material.serial;
            entry.subject = extract_subject_cn(material.certificate_pem);
            entry.dns_names = options.dns_names;
            entry.ip_addresses = options.ip_addresses;
            entry.certificate_pem = material.certificate_pem;
            entry.not_before = std::chrono::system_clock::now();
            entry.not_after = entry.not_before + options.validity;

            raft_node
                .submit_command(raft::testing::encode_record_issuance_command(entry),
                                std::chrono::milliseconds(30000))
                .get();

            res.set_content(boost::json::serialize(raft::testing::pem_material_to_json(material)),
                            "application/json");
        } catch (const std::invalid_argument& ex) {
            res.status = 400;
            res.set_content(json_error(ex.what()), "application/json");
        } catch (const std::exception& ex) {
            res.status = 503;
            res.set_content(json_error(ex.what()), "application/json");
        }
#else
        res.status = 401;
        res.set_content(json_error("built without TLS support — renew is unavailable"), "application/json");
#endif
    });

    server->Post("/v1/certificates/revoke", [&](const httplib::Request& req,
                                                httplib::Response& res) {
        if (!require_leader_or_redirect(req, res)) return;
        try {
            auto body = boost::json::parse(req.body).as_object();
            auto* serial_val = body.if_contains("serial");
            if (serial_val == nullptr) {
                res.status = 400;
                res.set_content(json_error("serial is required"), "application/json");
                return;
            }
            std::uint64_t serial = serial_val->is_string()
                                       ? std::stoull(std::string(serial_val->as_string()))
                                       : serial_val->to_number<std::uint64_t>();
            auto revoked_at = std::chrono::system_clock::now();

            auto result = raft_node
                              .submit_command(raft::testing::encode_record_revocation_command(
                                                  serial, revoked_at),
                                              std::chrono::milliseconds(30000))
                              .get();
            if (!result.empty()) {
                std::string result_str(reinterpret_cast<const char*>(result.data()), result.size());
                res.status = 404;
                res.set_content(result_str, "application/json");
                return;
            }

            std::lock_guard signer_lock(signer_mu);
            if (signer != nullptr) signer->mark_revoked_externally(serial, revoked_at);

            res.status = 200;
            res.set_content(R"({"revoked":true})", "application/json");
        } catch (const std::invalid_argument& ex) {
            res.status = 400;
            res.set_content(json_error(ex.what()), "application/json");
        } catch (const std::exception& ex) {
            res.status = 503;
            res.set_content(json_error(ex.what()), "application/json");
        }
    });

    server->Get("/v1/crl", [&](const httplib::Request& req, httplib::Response& res) {
        if (!require_leader_or_redirect(req, res)) return;
        std::lock_guard signer_lock(signer_mu);
        if (signer == nullptr) {
            res.status = 503;
            res.set_content(json_error("not_ready"), "application/json");
            return;
        }
        res.set_content(signer->crl_pem(), "application/x-pem-file");
    });

    std::thread http_thread([&] {
        std::cerr << "[info] ca_cluster_node: HTTP API listening on :" << cfg.http_port << "\n";
        server->listen("0.0.0.0", cfg.http_port);
    });

    {
        std::unique_lock lock(g_stop_mu);
        g_stop_cv.wait(lock, [] { return g_stop.load(); });
    }

    std::cerr << "[info] ca_cluster_node shutting down\n";
    g_stop = true;
    server->stop();
    http_thread.join();
    election_timer.join();
    heartbeat_timer.join();
    maintenance_thread.request_stop();
    maintenance_thread.join();

    raft_node.stop();
    std::cerr << "[info] ca_cluster_node shut down cleanly\n";
    return 0;
}
