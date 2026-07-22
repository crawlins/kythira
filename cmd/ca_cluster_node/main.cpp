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
//                    [--rpc-tls-cert <path> --rpc-tls-key <path>]
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
//
// --rpc-tls-cert/--rpc-tls-key (`.kiro/specs/ca-cluster-rpc-mtls/`) enable
// mutual TLS on the Raft-internal RPC channel, bootstrapped by the given
// static, operator-provisioned credential (byte-identical across every
// node, same distribution channel as --unseal-key-file) and automatically
// cut over to the cluster's own CA root once it exists — see that spec's
// design.md for the full two-phase bootstrap. Omitting both flags falls
// back to plain, unauthenticated TCP for the RPC channel (this spec makes
// RPC TLS available and recommended, not mandatory), UNLESS this node has
// already completed that cutover in a prior run (a persisted peer
// certificate exists under --data-dir), in which case it rejoins using
// that identity directly — no bootstrap credential is needed at all after
// the first successful cutover.

#include "config.hpp"

#include <raft/ca_http_helpers.hpp>
#include <raft/ca_state_machine.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/tls_tcp_rpc.hpp>

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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

// Extends tcp_raft_types (real TCP RPC transport, file-backed persistence,
// JSON serialisation — the same components cmd/chaos_node uses) with the
// CA-specific state machine. The default, plain-TCP RPC configuration
// (Requirement 3.3's fallback).
struct ca_cluster_raft_types_plain : kythira::tcp_raft_types {
    using state_machine_type = raft::testing::ca_state_machine;
};

#ifdef KYTHIRA_HAS_OPENSSL
// Same as above, but with the RPC-internal transport swapped for the
// mutual-TLS-wrapped one (`.kiro/specs/ca-cluster-rpc-mtls/`, Requirement
// 3.2). node<Types> never needs to know which of these two Types it was
// instantiated with — both satisfy network_client/network_server
// identically.
struct ca_cluster_raft_types_tls : kythira::tcp_raft_types {
    using state_machine_type = raft::testing::ca_state_machine;
    using network_client_type = kythira::tls_tcp_rpc_client;
    using network_server_type = kythira::tls_tcp_rpc_server;
};
#endif

// Per-request submit_command()/read_state() timeout on the client-facing
// HTTP path. 60s (rather than a tighter value) gives a real margin for
// commit latency under host contention — CI runners and shared dev
// machines routinely run this alongside dozens of other parallel test
// binaries, and a 3-node Raft election/replication round-trip that would
// finish in well under a second on an idle host can occasionally exceed a
// tighter timeout purely from CPU scheduling delay, not any actual protocol
// problem.
constexpr auto k_command_timeout = std::chrono::milliseconds(60000);

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
// ledger()/rpc_tls_ready_node_ids() without node<Types> exposing its private
// _state_machine member — this is exactly the seam read_state() exists for.
template<typename Types>
auto read_ca_state(kythira::node<Types>& node, std::chrono::milliseconds timeout)
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

#ifdef KYTHIRA_HAS_OPENSSL

// ── RPC peer identity persistence (Requirement 7.1) ─────────────────────────
//
// Persisted under --data-dir alongside the existing Raft log/snapshot
// storage, in the same directory tree file_persistence_engine already
// writes to. Three files, not two: the CA root PEM is persisted alongside
// the node's own cert/key because a restarted node's RPC transport must be
// able to verify OTHER peers' CA-issued certificates immediately — before
// any Raft communication has succeeded, since establishing that
// communication is exactly what the transport's trust policy gates. This
// is necessary implementation detail beyond Requirement 7.1's literal text
// ("certificate/key"), not scope creep: without it, a restarted
// already-cutover node would have its own valid identity but no way to
// evaluate a peer's, defeating Property 5.
auto rpc_peer_cert_path(const std::string& data_dir) -> std::string {
    return data_dir + "/rpc_peer_cert.pem";
}
auto rpc_peer_key_path(const std::string& data_dir) -> std::string {
    return data_dir + "/rpc_peer_key.pem";
}
auto rpc_peer_root_path(const std::string& data_dir) -> std::string {
    return data_dir + "/rpc_ca_root.pem";
}

auto read_whole_file(const std::string& path) -> std::optional<std::string> {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) return std::nullopt;
    return content;
}

auto write_whole_file(const std::string& path, const std::string& content) -> void {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("ca_cluster_node: cannot write " + path);
    f << content;
    if (!f) throw std::runtime_error("ca_cluster_node: failed writing " + path);
}

// Requirement 5.1's "still-valid" check: the persisted cert exists, parses,
// and has not yet expired. Renewal well before actual expiry is
// maybe_renew_rpc_identity()'s job (Requirement 7.2) — this is only the
// "is it usable at all right now" gate for maybe_acquire_rpc_identity()'s
// no-op check and for deciding the node's startup identity.
auto have_valid_persisted_peer_cert(const std::string& data_dir) -> bool {
    auto cert_pem = read_whole_file(rpc_peer_cert_path(data_dir));
    auto key_pem = read_whole_file(rpc_peer_key_path(data_dir));
    auto root_pem = read_whole_file(rpc_peer_root_path(data_dir));
    if (!cert_pem.has_value() || !key_pem.has_value() || !root_pem.has_value()) return false;

    BIO* bio = BIO_new_mem_buf(cert_pem->data(), static_cast<int>(cert_pem->size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) return false;
    bool not_expired = X509_cmp_current_time(X509_get0_notAfter(cert)) > 0;
    X509_free(cert);
    return not_expired;
}

auto persist_rpc_peer_identity(const std::string& data_dir, const std::string& cert_pem,
                               const std::string& key_pem, const std::string& root_pem) -> void {
    std::filesystem::create_directories(data_dir);
    write_whole_file(rpc_peer_cert_path(data_dir), cert_pem);
    write_whole_file(rpc_peer_key_path(data_dir), key_pem);
    write_whole_file(rpc_peer_root_path(data_dir), root_pem);
}

// This node's own RPC identity options (Requirement 5.2) — a CN that
// unambiguously identifies the node, sufficient for peer-to-peer mutual
// verification and not intended for any external client-facing use.
auto rpc_peer_identity_options(std::uint64_t node_id) -> raft::testing::leaf_certificate_options {
    raft::testing::leaf_certificate_options opts;
    std::string name = "ca-cluster-node-" + std::to_string(node_id);
    opts.subject.common_name = name;
    opts.dns_names = {name};
    opts.server_auth = true;
    opts.client_auth = true;
    return opts;
}

// "scheme://host:port" -> (host, port), for constructing an httplib client
// against cfg.http_address_for(leader_id) (already stored in that combined
// form — config.hpp's own doc comment on ca_cluster_peer_info::http_address).
auto split_host_port(const std::string& url) -> std::pair<std::string, int> {
    auto scheme_end = url.find("://");
    auto rest = scheme_end == std::string::npos ? url : url.substr(scheme_end + 3);
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    auto colon = rest.rfind(':');
    if (colon == std::string::npos) return {rest, 443};
    return {rest.substr(0, colon), std::stoi(rest.substr(colon + 1))};
}

// Requirement 5.1's own "has_root_material() at all" check, NOT the
// separate CSR-signing step below. node<Types>::read_state() rejects
// immediately with "not leader" for a follower — it does not forward to
// the leader (confirmed during this spec's implementation: a follower's
// read_state() call returns in the same log timestamp as the request,
// with no RPC to any peer in between). Using it unconditionally (as
// design.md's own maybe_acquire_rpc_identity sketch does) means a
// follower can NEVER learn whether the CA root exists via that path,
// which is exactly the deadlock this helper avoids: a leader's read is
// in-process (works, using whatever RPC-TLS identity is currently
// active); a follower instead asks the leader's *client-facing* HTTP API
// (/v1/root-ca, bearer-token authenticated) — a completely separate
// transport and trust boundary from RPC TLS, so it keeps working
// regardless of whatever state this node's or the leader's RPC-TLS
// transport is currently in. Mirrors the same leader/follower split
// acquire_rpc_peer_certificate() below already uses for CSR signing.
// Best-effort GET of one address's own /v1/root-ca. Used both for the
// Raft-known leader (the common case) and for the blind per-peer fallback
// below - a non-leader peer just answers 308/503 (require_leader_or_
// redirect()) or refuses the connection outright, which this treats
// identically to "not this one, try the next".
[[nodiscard]] inline auto try_fetch_root_cert_pem_from(const std::string& http_address,
                                                       const std::string& auth_token)
    -> std::optional<std::string> {
    auto [host, port] = split_host_port(http_address);
    httplib::Client client(host, port);
    client.enable_server_certificate_verification(false);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);
    auto res = client.Get("/v1/root-ca", {{"Authorization", "Bearer " + auth_token}});
    if (!res || res->status != 200 || res->body.empty()) return std::nullopt;
    return res->body;
}

template<typename Types>
auto fetch_root_cert_pem(kythira::node<Types>& raft_node,
                         const ca_cluster_node::ca_cluster_node_config& cfg)
    -> std::optional<std::string> {
    if (raft_node.is_leader()) {
        try {
            auto state = read_ca_state(raft_node, std::chrono::milliseconds(5000));
            if (!state.has_root_material()) return std::nullopt;
            return state.root_certificate_pem();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    // Prefer the Raft-learned leader when known - one HTTP call, same as
    // this function's original (leader-only) behavior.
    if (auto leader_id = raft_node.known_leader(); leader_id.has_value()) {
        if (auto leader_http = cfg.http_address_for(*leader_id); leader_http.has_value()) {
            if (auto pem = try_fetch_root_cert_pem_from(*leader_http, cfg.auth_token)) {
                return pem;
            }
        }
    }

    // Fall back to asking every configured peer directly by its static
    // client-facing address, not only whichever leader this node has
    // learned via Raft RPC. known_leader() can stay permanently empty for
    // a node that hasn't yet widened its own RPC-TLS accept policy (see
    // maybe_widen_rpc_trust_policy()): once any peer switches its
    // PRESENTED identity to a CA-issued cert (maybe_acquire_rpc_identity,
    // after k_identity_acquire_grace), a not-yet-widened node rejects that
    // peer's connections in both directions - including the leader's own
    // AppendEntries, which is how known_leader() would normally get
    // populated in the first place. That is a real deadlock on real AWS
    // infrastructure (observed: a node's data directory staying completely
    // empty - no persisted Raft term/voted_for, let alone a peer cert -
    // while its log filled with "peer certificate rejected by trust
    // policy" against every peer, indefinitely), not merely a slow
    // convergence. Every peer's client-facing HTTP API is a separate
    // transport and trust boundary from RPC TLS (bearer-token
    // authenticated, TLS verification deliberately disabled - see
    // acquire_rpc_peer_certificate()'s own comment) and keeps working
    // regardless of RPC-TLS state, so trying each configured peer's
    // address directly breaks the deadlock: only the actual leader answers
    // 200, every other peer answers 308/503 or refuses the connection and
    // is simply skipped.
    for (const auto& p : cfg.peers) {
        if (auto pem = try_fetch_root_cert_pem_from(p.http_address, cfg.auth_token)) {
            return pem;
        }
    }
    return std::nullopt;
}

// Requirement 5.1: obtains a signed certificate for this node's own RPC
// identity via the cluster's own /v1/certificates route — directly
// in-process if leader (via the already-open `signer`), otherwise over
// HTTPS to the leader's client-facing address, bearer-token authenticated.
// Verification of the leader's client-facing TLS listener is deliberately
// disabled (matching the existing internal-caller precedent in
// tests/ca_service_serve_integration_test.cpp): the bearer token, not the
// listener's certificate, is the trust factor for this already-a-cluster-
// member-by-virtue-of-Raft-membership call.
// Also submits record_rpc_tls_ready(cfg.node_id) — Requirement 5.3 — as
// part of the same call, since that's the ONE thing both branches below
// (in-process leader signing vs. HTTP-to-leader for a follower) can
// always do reliably: node<Types>::submit_command() only works when
// called on the node that's actually leader right now (confirmed during
// this spec's implementation — it has no built-in forwarding, matching
// read_state()'s identical behavior, which is why require_leader_or_
// redirect() exists at all for the ordinary HTTP routes below), so a
// follower calling it directly for itself would always fail. Piggybacking
// this node's id onto the SAME CSR request that's already reaching the
// real leader lets that leader call submit_command() successfully on
// this follower's behalf, right after committing the same CSR's issuance
// — both happen inside the one process that can actually do it. Best-
// effort in both branches: a failure here does not throw (the certificate
// itself was still obtained successfully either way) — the caller simply
// retries the whole acquire flow next tick if have_valid_persisted_peer_
// cert() is still false, or (if the cert was already persisted from a
// prior successful call) is expected to eventually converge via this
// same best-effort path succeeding on some later call while the CA
// client-facing API remains reachable.
template<typename Types>
auto acquire_rpc_peer_certificate(kythira::node<Types>& raft_node,
                                  const ca_cluster_node::ca_cluster_node_config& cfg,
                                  std::mutex& signer_mu,
                                  std::unique_ptr<raft::testing::certificate_authority>& signer,
                                  const std::string& csr_pem,
                                  const raft::testing::csr_signing_options& sign_opts)
    -> raft::testing::pem_material {
    if (raft_node.is_leader()) {
        raft::testing::pem_material material;
        {
            std::unique_lock signer_lock(signer_mu);
            if (signer == nullptr) {
                throw std::runtime_error(
                    "acquire_rpc_peer_certificate: local signer not ready yet");
            }
            material = signer->sign_csr(csr_pem, sign_opts);
        }
        try {
            raft_node
                .submit_command(raft::testing::encode_record_rpc_tls_ready_command(cfg.node_id),
                                k_command_timeout)
                .get();
        } catch (const std::exception&) {
            // Best-effort — see function comment above.
        }
        return material;
    }

    auto leader_id = raft_node.known_leader();
    if (!leader_id.has_value())
        throw std::runtime_error("acquire_rpc_peer_certificate: no known leader");
    auto leader_http = cfg.http_address_for(*leader_id);
    if (!leader_http.has_value()) {
        throw std::runtime_error("acquire_rpc_peer_certificate: leader http address unknown");
    }

    auto [host, port] = split_host_port(*leader_http);
    httplib::Client client(host, port);
    client.enable_server_certificate_verification(false);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(30, 0);

    boost::json::object body;
    body["csr_pem"] = csr_pem;
    boost::json::array dns_arr;
    for (const auto& d : sign_opts.dns_names) dns_arr.push_back(boost::json::string(d));
    body["dns_names"] = dns_arr;
    body["server_auth"] = sign_opts.server_auth;
    body["client_auth"] = sign_opts.client_auth;
    // Requirement 5.3's record_rpc_tls_ready(self) submission: this node
    // is a follower here (the in-process branch above handles the leader
    // case directly), and node<Types>::submit_command() has no built-in
    // forwarding to the actual leader — it just fails outright with "not
    // leader" for anyone who calls it while not currently leading
    // (confirmed during this spec's implementation, matching how
    // read_state() behaves and why require_leader_or_redirect() exists at
    // all for the ordinary HTTP routes below). Piggybacking this node's
    // own id on the SAME request that's already reaching the real leader
    // lets the leader submit record_rpc_tls_ready(cfg.node_id) on this
    // follower's behalf immediately after it commits this CSR's issuance
    // — both happen inside the same leader process that can actually
    // call submit_command() successfully.
    body["rpc_tls_ready_node_id"] = cfg.node_id;

    auto res = client.Post("/v1/certificates", {{"Authorization", "Bearer " + cfg.auth_token}},
                           boost::json::serialize(body), "application/json");
    if (!res) {
        throw std::runtime_error("acquire_rpc_peer_certificate: request to leader failed: " +
                                 httplib::to_string(res.error()));
    }
    if (res->status != 200) {
        throw std::runtime_error("acquire_rpc_peer_certificate: leader returned " +
                                 std::to_string(res->status) + ": " + res->body);
    }
    auto parsed = boost::json::parse(res->body).as_object();
    raft::testing::pem_material material;
    material.certificate_pem = std::string(parsed.at("certificate_pem").as_string());
    if (auto* v = parsed.if_contains("chain_pem")) material.chain_pem = std::string(v->as_string());
    return material;
}

#endif  // KYTHIRA_HAS_OPENSSL

}  // namespace

// Everything from Raft node construction through shutdown, templated on
// Types so it can be instantiated once for plain-TCP RPC and once for
// TLS-wrapped RPC (Requirement 3.2/3.3) without duplicating this ~500-line
// body — node<Types> and every lambda capturing raft_node below are
// oblivious to which transport they were actually given.
template<typename Types>
auto run_ca_cluster_node(ca_cluster_node::ca_cluster_node_config cfg, std::string unseal_passphrase)
    -> int {
    using raft_node_t = kythira::node<Types>;
    // ca_cluster_node/CMakeLists.txt only builds this binary at all when the
    // certificate_authority target exists, which is precisely what defines
    // KYTHIRA_HAS_OPENSSL (root CMakeLists.txt) — so it is always defined in
    // this translation unit; k_rpc_tls is what actually distinguishes the
    // two Types this function is instantiated with.
    constexpr bool k_rpc_tls =
        std::is_same_v<typename Types::network_client_type, kythira::tls_tcp_rpc_client>;

    kythira::raft_configuration raft_cfg;
    raft_cfg._election_timeout_min = cfg.election_timeout_min;
    raft_cfg._election_timeout_max = cfg.election_timeout_max;
    raft_cfg._heartbeat_interval = cfg.heartbeat_interval;
    raft_cfg._rpc_timeout = cfg.rpc_timeout;

    typename Types::network_server_type rpc_server = [&] {
        if constexpr (k_rpc_tls) {
            return typename Types::network_server_type(cfg.rpc_port, cfg.rpc_tls_config);
        } else {
            return typename Types::network_server_type(cfg.rpc_port);
        }
    }();
    typename Types::network_client_type rpc_client = [&] {
        if constexpr (k_rpc_tls) {
            return typename Types::network_client_type(cfg.rpc_tls_config);
        } else {
            return typename Types::network_client_type();
        }
    }();
    for (const auto& p : cfg.peers) rpc_client.add_peer(p.node_id, p.rpc_host, p.rpc_port);

    // Requirement 1.3/5.3/6.2/7.2: independent handle copies kept alive here
    // so the maintenance thread can reconfigure the SAME live transport
    // node<Types> ends up using, even after the objects above are moved into
    // node_config (see tls_tcp_rpc.hpp's top-of-file comment for why copying
    // rpc_server/rpc_client works for the TLS Types — both are shared_ptr-
    // backed handles under the hood, so the copy shares the same live
    // transport). The plain tcp_rpc_client/tcp_rpc_server types are
    // move-only and have no such handle semantics, but they're also never
    // read through rpc_server_handle/rpc_client_handle when !k_rpc_tls
    // (every maintenance behavior below is itself gated on k_rpc_tls) — an
    // idle, never-started second instance is constructed instead, purely so
    // these variables have a consistent, valid type in both instantiations
    // without needing to be conditionally declared.
    typename Types::network_server_type rpc_server_handle = [&] {
        if constexpr (k_rpc_tls) {
            return rpc_server;
        } else {
            return typename Types::network_server_type(cfg.rpc_port);
        }
    }();
    typename Types::network_client_type rpc_client_handle = [&] {
        if constexpr (k_rpc_tls) {
            return rpc_client;
        } else {
            return typename Types::network_client_type();
        }
    }();

    kythira::file_persistence_engine<> persistence(cfg.data_dir);

    kythira::node_config<Types> ncfg{
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
              << " bootstrap_ca=" << (cfg.bootstrap_ca ? "true" : "false")
              << " rpc_tls=" << (k_rpc_tls ? "true" : "false") << "\n";

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
            raft_node.submit_command(cmd, k_command_timeout).get();
            std::cerr << "[info] ca_cluster_node: bootstrap_ca committed\n";
            bootstrap_done_or_unnecessary = true;
        } catch (const std::exception& ex) {
            std::cerr << "[warn] ca_cluster_node: bootstrap_ca submission failed, will retry: "
                      << ex.what() << "\n";
        }
    };

#ifdef KYTHIRA_HAS_OPENSSL
    // ── RPC TLS maintenance behaviors (Requirements 5, 6, 7) — only
    //    meaningful (and only compiled in a way that's reachable) when this
    //    Types instantiation actually uses tls_tcp_rpc_client/server. ──────
    bool cutover_finalized = false;
    bool trust_widened = false;
    std::optional<std::chrono::steady_clock::time_point> root_first_seen_at;
    // Lower bound on how long a node waits, after first observing the CA
    // root exists, before it PRESENTS its own CA-issued identity —
    // deliberately separate from (and larger than) one maintenance-thread
    // tick. Confirmed necessary during this spec's implementation: on a
    // contended host, the node that acquires fastest (typically the
    // leader, whose fetch_root_cert_pem() is an in-process read) can
    // finish widening-then-acquiring-then-switching within the SAME
    // maintenance tick the root commits on, while a follower's own widen
    // still needs a real HTTP round trip to the leader's /v1/root-ca to
    // even begin — and that endpoint itself requires a quorum-confirmed
    // leader read (node<Types>::read_state()'s read-index heartbeat),
    // which depends on RPC connectivity to a majority of followers. If
    // the leader switches its presented identity before ANY follower has
    // widened, every follower starts rejecting the leader's connections,
    // which breaks the very read-index heartbeats read_state() needs —
    // which in turn breaks /v1/root-ca for every follower still trying to
    // widen, since it can no longer get a quorum-confirmed read either.
    // That is a genuine deadlock, not a slow-and-eventually-resolves
    // race: it was reproduced reliably on GitHub Actions' shared runners
    // (though not on faster/idle hardware) as this spec's ca_cluster_node
    // _rpc_tls_test hanging past its /v1/certificates budget with every
    // follower stuck rejecting the leader's traffic indefinitely. This
    // grace period keeps RPC fully on the old (universally-trusted)
    // credential for a bounded window after the root is known, giving
    // every already-alive node's maintenance thread a realistic chance to
    // widen (a plain HTTP call, unaffected by RPC-TLS as long as nobody
    // has switched their presented identity yet) before anyone switches.
    constexpr auto k_identity_acquire_grace = std::chrono::seconds(3);

    // Widening what this node ACCEPTS (Requirement 6.1) is deliberately
    // decoupled from acquiring/PRESENTING this node's own CA-issued
    // identity below — they depend on different facts. Whether to accept
    // a CA-chain-verifiable peer only depends on whether the CA root
    // exists at all (a replicated fact every node observes at roughly the
    // same time, regardless of acquisition order). Confirmed necessary
    // during this spec's implementation: coupling the two (as design.md's
    // original sketch does — reload_trust_policy() called only inside the
    // acquire flow, at the same time as switching to presenting the new
    // cert) lets whichever node acquires fastest start PRESENTING its new
    // certificate to a peer whose OWN accept policy hasn't widened yet
    // (that peer is still pure pinned_fingerprint, since it hasn't
    // acquired anything itself yet) — that peer then rejects the
    // connection outright, since it has no ca_root_pem configured to
    // chain-verify against. Widening accept as soon as the root exists,
    // independent of this node's own acquisition progress, closes that
    // race: by the time ANY node finishes acquiring and switches its
    // PRESENTED identity, every peer that has already observed the same
    // replicated root material has already widened its OWN accept policy
    // too.
    auto maybe_widen_rpc_trust_policy = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            if (trust_widened) return;
            auto root_pem = fetch_root_cert_pem(raft_node, cfg);
            if (!root_pem.has_value()) return;
            if (!root_first_seen_at.has_value()) {
                root_first_seen_at = std::chrono::steady_clock::now();
            }

            auto dual_policy = kythira::either(
                cfg.rpc_tls_config.trust_policy.bootstrap_fingerprint_hex.value_or(""), *root_pem);
            if (!cfg.rpc_tls_config.trust_policy.bootstrap_fingerprint_hex.has_value()) {
                dual_policy.bootstrap_fingerprint_hex.reset();
            }
            rpc_server_handle.reload_trust_policy(dual_policy);
            rpc_client_handle.reload_trust_policy(dual_policy);
            trust_widened = true;
        }
    };

    auto maybe_acquire_rpc_identity = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            if (have_valid_persisted_peer_cert(cfg.data_dir)) return;
            if (!root_first_seen_at.has_value() ||
                std::chrono::steady_clock::now() - *root_first_seen_at < k_identity_acquire_grace) {
                return;
            }

            auto root_pem = fetch_root_cert_pem(raft_node, cfg);
            if (!root_pem.has_value()) return;

            auto leaf_opts = rpc_peer_identity_options(cfg.node_id);
            auto csr = raft::testing::generate_key_and_csr(leaf_opts);

            raft::testing::csr_signing_options sign_opts;
            sign_opts.dns_names = leaf_opts.dns_names;
            sign_opts.server_auth = leaf_opts.server_auth;
            sign_opts.client_auth = leaf_opts.client_auth;

            raft::testing::pem_material material;
            try {
                material = acquire_rpc_peer_certificate(raft_node, cfg, signer_mu, signer,
                                                        csr.csr_pem, sign_opts);
            } catch (const std::exception& ex) {
                std::cerr << "[warn] ca_cluster_node: failed to acquire RPC peer identity, will "
                             "retry: "
                          << ex.what() << "\n";
                return;
            }

            try {
                persist_rpc_peer_identity(cfg.data_dir, material.certificate_pem,
                                          csr.private_key_pem, *root_pem);
            } catch (const std::exception& ex) {
                std::cerr << "[warn] ca_cluster_node: failed to persist RPC peer identity: "
                          << ex.what() << "\n";
                return;
            }

            // Requirement 5.3: PRESENT the new certificate. This node's
            // OWN accept policy was already widened above (same tick or
            // earlier). Every OTHER already-alive node's accept policy is
            // assumed widened too, by now, because of the
            // k_identity_acquire_grace wait above — not merely because
            // the root became "replicated" (replication alone doesn't
            // bound how long a peer's maintenance thread takes to notice
            // and act on it).
            rpc_server_handle.reload_identity(rpc_peer_cert_path(cfg.data_dir),
                                              rpc_peer_key_path(cfg.data_dir));
            rpc_client_handle.reload_identity(rpc_peer_cert_path(cfg.data_dir),
                                              rpc_peer_key_path(cfg.data_dir));

            // record_rpc_tls_ready(cfg.node_id) was already best-effort
            // submitted as part of acquire_rpc_peer_certificate() above
            // (see that function's own comment for why it, not this
            // closure, owns that submission).
            std::cerr << "[info] ca_cluster_node: RPC peer identity acquired for node "
                      << cfg.node_id << "\n";
        }
    };

    auto maybe_finalize_rpc_tls_cutover = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            if (cutover_finalized) return;
            // rpc_tls_ready_node_ids() isn't exposed by any client-facing
            // HTTP route (unlike has_root_material()/root_certificate_pem(),
            // fetch_root_cert_pem()'s follower fallback has nothing to call
            // for this check) — so, like maybe_bootstrap()/ensure_signer()
            // above, finalization only runs on the leader, via the
            // in-process read that already works correctly once every node
            // has widened via maybe_widen_rpc_trust_policy(). A follower
            // that never happens to become leader simply never finalizes
            // itself and stays in the dual-trust policy indefinitely —
            // Requirement 6.4 explicitly tolerates this ("operators MAY
            // continue using [the bootstrap credential]... this requirement
            // governs an individual already-cutover node's own trust
            // policy"), so an asymmetric steady state where some nodes have
            // finalized and others haven't is safe, not a correctness bug.
            if (!raft_node.is_leader()) return;
            raft::testing::ca_state_machine state;
            try {
                state = read_ca_state(raft_node, std::chrono::milliseconds(5000));
            } catch (const std::exception&) {
                return;
            }
            auto ready = state.rpc_tls_ready_node_ids();
            for (auto id : cfg.all_node_ids()) {
                if (!ready.contains(id)) return;
            }
            auto root_only_policy = kythira::ca_root_only(state.root_certificate_pem());
            rpc_server_handle.reload_trust_policy(root_only_policy);
            rpc_client_handle.reload_trust_policy(root_only_policy);
            cutover_finalized = true;
            std::cerr << "[info] ca_cluster_node: RPC TLS cutover finalized — bootstrap "
                         "credential no longer accepted for new connections\n";
        }
    };

    // Requirement 7.2: renew a persisted peer certificate before expiry
    // (a fixed 7-day window — generous relative to the default 30-day
    // leaf validity elsewhere in this project, e.g. leaf_certificate_options'
    // own default, while still leaving multiple maintenance-thread ticks of
    // retry room if the leader is briefly unreachable).
    constexpr auto k_renewal_window = std::chrono::hours(24 * 7);

    auto maybe_renew_rpc_identity = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            auto cert_pem = read_whole_file(rpc_peer_cert_path(cfg.data_dir));
            if (!cert_pem.has_value()) return;  // nothing persisted yet — acquire's job

            BIO* bio = BIO_new_mem_buf(cert_pem->data(), static_cast<int>(cert_pem->size()));
            X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            BIO_free(bio);
            if (cert == nullptr) return;
            auto threshold = static_cast<std::time_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) +
                std::chrono::duration_cast<std::chrono::seconds>(k_renewal_window).count());
            bool near_expiry = X509_cmp_time(X509_get0_notAfter(cert), &threshold) < 0;
            X509_free(cert);
            if (!near_expiry) return;

            auto leader_id = raft_node.known_leader();
            if (!leader_id.has_value()) return;
            auto leader_http = cfg.http_address_for(*leader_id);
            if (!leader_http.has_value()) return;
            auto [host, port] = split_host_port(*leader_http);

            auto leaf_opts = rpc_peer_identity_options(cfg.node_id);
            auto csr = raft::testing::generate_key_and_csr(leaf_opts);

            httplib::SSLClient renew_client(host, port, rpc_peer_cert_path(cfg.data_dir),
                                            rpc_peer_key_path(cfg.data_dir));
            renew_client.enable_server_certificate_verification(false);
            renew_client.set_connection_timeout(5, 0);
            renew_client.set_read_timeout(30, 0);

            boost::json::object body;
            body["csr_pem"] = csr.csr_pem;
            auto res = renew_client.Post("/v1/certificates/renew", boost::json::serialize(body),
                                         "application/json");
            if (!res || res->status != 200) {
                std::cerr << "[warn] ca_cluster_node: RPC peer identity renewal failed, will "
                             "retry\n";
                return;
            }

            auto root_pem = fetch_root_cert_pem(raft_node, cfg);
            if (!root_pem.has_value()) return;

            try {
                auto parsed = boost::json::parse(res->body).as_object();
                std::string new_cert_pem = std::string(parsed.at("certificate_pem").as_string());
                persist_rpc_peer_identity(cfg.data_dir, new_cert_pem, csr.private_key_pem,
                                          *root_pem);
            } catch (const std::exception& ex) {
                std::cerr << "[warn] ca_cluster_node: failed to persist renewed RPC peer "
                             "identity: "
                          << ex.what() << "\n";
                return;
            }

            rpc_server_handle.reload_identity(rpc_peer_cert_path(cfg.data_dir),
                                              rpc_peer_key_path(cfg.data_dir));
            rpc_client_handle.reload_identity(rpc_peer_cert_path(cfg.data_dir),
                                              rpc_peer_key_path(cfg.data_dir));
            std::cerr << "[info] ca_cluster_node: RPC peer identity renewed\n";
        }
    };
#endif  // KYTHIRA_HAS_OPENSSL

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
                            .submit_command(raft::testing::encode_noop_command(), k_command_timeout)
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
#ifdef KYTHIRA_HAS_OPENSSL
            if constexpr (k_rpc_tls) {
                maybe_widen_rpc_trust_policy();
                maybe_acquire_rpc_identity();
                maybe_finalize_rpc_tls_cutover();
                maybe_renew_rpc_identity();
            }
#endif
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
            auto state = read_ca_state(raft_node, k_command_timeout);
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
                                k_command_timeout)
                .get();

            // Requirement 5.3 (.kiro/specs/ca-cluster-rpc-mtls/): a follower
            // acquiring its own RPC peer identity (acquire_rpc_peer_certificate,
            // above) has no way to commit its own record_rpc_tls_ready(self)
            // — submit_command() only works when called on the actual
            // leader, which is exactly where THIS handler is already
            // running. Best-effort: a failure here doesn't fail the
            // certificate issuance itself (the follower already has its
            // valid certificate either way) and is simply retried by the
            // follower's own next maintenance tick's request.
            if (auto* ready_id = body.if_contains("rpc_tls_ready_node_id")) {
                try {
                    raft_node
                        .submit_command(raft::testing::encode_record_rpc_tls_ready_command(
                                            ready_id->to_number<std::uint64_t>()),
                                        k_command_timeout)
                        .get();
                } catch (const std::exception&) {
                    // Logged implicitly via the requester's own retry path
                    // (maybe_acquire_rpc_identity's HTTP call will simply
                    // repeat the whole request, including this field, on
                    // its next tick) — not surfaced to this response since
                    // the certificate itself was issued successfully.
                }
            }

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
            auto state = read_ca_state(raft_node, k_command_timeout);
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
                                k_command_timeout)
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
                                              k_command_timeout)
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

#ifdef KYTHIRA_HAS_OPENSSL
    // Requirement 2.3 / 3.2 / 3.3: RPC TLS is "enabled" if the operator gave
    // --rpc-tls-cert/--rpc-tls-key OR this node already has a persisted peer
    // certificate from a prior cutover (Property 5 — the bootstrap
    // credential flags are not required on every subsequent restart).
    // Neither present: plain TCP (Requirement 3.3's documented, non-mandatory
    // default). --rpc-tls-cert given but unreadable/invalid, with no
    // persisted fallback: fail closed (Requirement 2.3), surfaced naturally
    // by tls_tcp_rpc_server's constructor throwing, caught below.
    bool have_persisted = have_valid_persisted_peer_cert(cfg.data_dir);
    bool use_rpc_tls = !cfg.rpc_tls_cert_path.empty() || have_persisted;

    if (use_rpc_tls) {
        kythira::tls_tcp_rpc_config rpc_tls_config;
        if (have_persisted) {
            rpc_tls_config.cert_path = rpc_peer_cert_path(cfg.data_dir);
            rpc_tls_config.key_path = rpc_peer_key_path(cfg.data_dir);
            // Requirement 6.1: still dual-trust at startup, not straight to
            // ca_root_only — a restart doesn't know whether every peer had
            // already reached rpc_tls_ready before this node went down, and
            // maybe_finalize_rpc_tls_cutover() will re-narrow this within one
            // maintenance tick once quorum confirms the full ready set
            // again. Using ca_root_only immediately here risks this node
            // rejecting a peer that itself hasn't cut over yet.
            auto root_pem = read_whole_file(rpc_peer_root_path(cfg.data_dir));
            kythira::tls_rpc_trust_policy policy;
            policy.ca_root_pem = root_pem;
            if (!cfg.rpc_tls_cert_path.empty()) {
                try {
                    auto bundle = read_whole_file(cfg.rpc_tls_cert_path);
                    if (bundle.has_value()) {
                        auto root = raft::testing::root_cert_from_pem_bundle(*bundle);
                        policy.bootstrap_fingerprint_hex =
                            raft::testing::ca_bootstrap_detail::sha256_fingerprint_hex_bare(
                                root.get());
                    }
                } catch (const std::exception&) {
                    // Bootstrap credential no longer needed (Property 5) —
                    // an unreadable/invalid one here is not fatal, just
                    // unused.
                }
            }
            rpc_tls_config.trust_policy = policy;
        } else {
            rpc_tls_config.cert_path = cfg.rpc_tls_cert_path;
            rpc_tls_config.key_path = cfg.rpc_tls_key_path;
            try {
                auto bundle = read_whole_file(cfg.rpc_tls_cert_path);
                if (!bundle.has_value()) {
                    std::cerr << "ca_cluster_node: cannot read --rpc-tls-cert "
                              << cfg.rpc_tls_cert_path << "\n";
                    return 1;
                }
                auto root = raft::testing::root_cert_from_pem_bundle(*bundle);
                rpc_tls_config.trust_policy = kythira::pinned_fingerprint(
                    raft::testing::ca_bootstrap_detail::sha256_fingerprint_hex_bare(root.get()));
            } catch (const std::exception& ex) {
                std::cerr << "ca_cluster_node: failed to compute bootstrap credential fingerprint "
                             "from --rpc-tls-cert: "
                          << ex.what() << "\n";
                return 1;
            }
        }
        cfg.rpc_tls_config = rpc_tls_config;

        try {
            return run_ca_cluster_node<ca_cluster_raft_types_tls>(std::move(cfg),
                                                                  std::move(unseal_passphrase));
        } catch (const std::exception& ex) {
            std::cerr << "ca_cluster_node: failed to start with RPC TLS enabled: " << ex.what()
                      << "\n";
            return 1;
        }
    }

    std::cerr << "ca_cluster_node: WARNING: running without RPC TLS (no --rpc-tls-cert/"
                 "--rpc-tls-key given, and no persisted peer certificate found under --data-dir) "
                 "— suitable only for a private network\n";
#endif  // KYTHIRA_HAS_OPENSSL

    return run_ca_cluster_node<ca_cluster_raft_types_plain>(std::move(cfg),
                                                            std::move(unseal_passphrase));
}
