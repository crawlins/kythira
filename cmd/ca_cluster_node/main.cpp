// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

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
//                    [--rpc-address <addr>] [--http-address <addr>] [--bootstrap-ca]
//                    [--tls-cert <path> --tls-key <path>] [--peer-tls-ca <path>]
//                    [--rpc-tls-cert <path> --rpc-tls-key <path>]
//                    [--allow-plaintext-rpc] [--allow-plaintext-http]
//
// The client bearer token comes from $CA_SERVICE_AUTH_TOKEN (--auth-token
// still works, but leaves the token in /proc/<pid>/cmdline).
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
// design.md for the full two-phase bootstrap. A node that has already
// completed that cutover in a prior run (a persisted peer certificate
// exists under --data-dir) rejoins using that identity directly — no
// bootstrap credential is needed at all after the first successful cutover.
//
// With neither the flags nor a persisted certificate, the RPC channel would
// be plain, unauthenticated TCP. The node refuses to start in that state
// unless --rpc-address is loopback (127.0.0.0/8, ::1, or a name such as
// "localhost" resolving only to those; single-host use) or the operator
// passes --allow-plaintext-rpc / CA_CLUSTER_ALLOW_PLAINTEXT_RPC=1.
//
// The client-facing HTTP API follows the same rule: without --tls-cert/
// --tls-key the node refuses to start unless --http-address is loopback or
// the operator passes --allow-plaintext-http / CA_CLUSTER_ALLOW_PLAINTEXT_HTTP=1.
// Calls to a peer's https:// address verify its certificate (chain and
// hostname) against --peer-tls-ca, or this node's own --tls-cert bundle, so
// every node's listener certificate must chain to one shared root and name
// the host its peers dial; an http:// peer address must be loopback unless
// plaintext HTTP is opted in.

#include "config.hpp"

#include <raft/ca_http_helpers.hpp>
#include <raft/ca_state_machine.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/httplib_listeners.hpp>
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <set>
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

std::atomic<kythira::net_bind::httplib_listeners<>*> g_http_listeners{nullptr};
void on_http_signal(int) {
    auto* listeners = g_http_listeners.load();
    if (listeners != nullptr) listeners->request_stop();
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
// Present once this node has seen every cluster member enroll a CA-issued
// peer identity. A restart then trusts the cluster root alone instead of
// re-admitting the shared bootstrap credential (which identifies no
// particular node, so a holder of it could speak as any node).
auto rpc_cutover_marker_path(const std::string& data_dir) -> std::string {
    return data_dir + "/rpc_cutover_finalized";
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
    std::string name = raft::testing::peer_identity_dns_name(node_id);
    opts.subject.common_name = name;
    opts.dns_names = {name};
    opts.server_auth = true;
    opts.client_auth = true;
    return opts;
}

// Every configured node's reserved peer name, mapped to its node id — the
// only CA-chained certificates RPC TLS accepts, each bound to the node it
// names (tls_rpc_trust_policy::peer_node_ids).
auto cluster_peer_node_ids(const ca_cluster_node::ca_cluster_node_config& cfg)
    -> std::map<std::string, std::uint64_t> {
    std::map<std::string, std::uint64_t> ids;
    for (auto id : cfg.all_node_ids()) ids[raft::testing::peer_identity_dns_name(id)] = id;
    return ids;
}

// Never present (or persist) a certificate this cluster's own peers would
// reject: it must chain to the (MAC-verified) cluster root and carry this
// node's reserved peer name. The leader's response may arrive over plaintext
// HTTP (loopback or --allow-plaintext-http), so it is not trusted on its own.
auto issued_peer_cert_ok(const std::string& cert_pem, const std::string& root_pem,
                         std::uint64_t node_id) -> bool {
    BIO* bio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
    X509* issued = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (issued == nullptr) return false;
    bool ok = raft::testing::cert_chains_to_root(issued, root_pem) &&
              raft::testing::cert_has_dns_san_in(issued,
                                                 {raft::testing::peer_identity_dns_name(node_id)});
    X509_free(issued);
    return ok;
}

// An HTTP client for a peer's client-facing API. An https:// peer is
// verified, chain and hostname, against peer_tls_ca_file(): this link
// carries peer enrollment and the RPC trust state, and with verification
// off anyone on-path could impersonate the leader. An http:// peer was
// already limited to loopback or --allow-plaintext-http at startup
// (peer_http_security_error). `client_cert`/`client_key`, when given,
// present this node's RPC peer identity for the mTLS /renew route.
auto make_peer_client(const std::string& http_address,
                      const ca_cluster_node::ca_cluster_node_config& cfg,
                      const std::string& client_cert = {}, const std::string& client_key = {})
    -> std::unique_ptr<httplib::Client> {
    auto url = ca_cluster_node::parse_peer_url(http_address);
    auto host = url.host.find(':') == std::string::npos ? url.host : "[" + url.host + "]";
    auto origin = url.scheme + "://" + host + ":" + std::to_string(url.port);
    auto client = client_cert.empty()
                      ? std::make_unique<httplib::Client>(origin)
                      : std::make_unique<httplib::Client>(origin, client_cert, client_key);
    if (url.scheme == "https") {
        client->enable_server_certificate_verification(true);
        client->set_ca_cert_path(ca_cluster_node::peer_tls_ca_file(cfg));
    }
    return client;
}

// The RPC trust facts derived from replicated CA state: the root, whether
// every configured node has enrolled its CA-issued peer identity, and which
// peer certificates have been revoked.
auto rpc_trust_state_of(const raft::testing::ca_state_machine& state,
                        const std::vector<std::uint64_t>& node_ids)
    -> raft::testing::peer_rpc_trust_state {
    raft::testing::peer_rpc_trust_state out;
    out.root_pem = state.root_certificate_pem();
    auto ready = state.rpc_tls_ready_node_ids();
    out.cutover_complete =
        std::ranges::all_of(node_ids, [&](std::uint64_t id) { return ready.contains(id); });
    for (const auto& entry : state.ledger()) {
        if (entry.revoked_at.has_value() &&
            std::ranges::any_of(entry.dns_names, [](const std::string& name) {
                return raft::testing::is_reserved_peer_dns_name(name);
            })) {
            out.revoked_peer_serials.insert(entry.serial);
        }
    }
    return out;
}

// Best-effort GET of one address's /v1/peer/rpc-trust. A non-leader peer
// answers 308/503 (require_leader_or_redirect()) or refuses the connection,
// which this treats as "not this one, try the next". The request carries a
// MAC over a fresh nonce instead of the client bearer token, and the
// response must carry a MAC over (nonce, body): the state becomes this
// node's RPC trust anchor, so an impostor must not be able to supply it.
[[nodiscard]] auto try_fetch_rpc_trust_from(const std::string& http_address,
                                            const ca_cluster_node::ca_cluster_node_config& cfg)
    -> std::optional<raft::testing::peer_rpc_trust_state> {
    std::unique_ptr<httplib::Client> client;
    try {
        client = make_peer_client(http_address, cfg);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    client->set_connection_timeout(5, 0);
    client->set_read_timeout(10, 0);
    auto nonce = raft::testing::random_nonce_hex();
    auto res =
        client->Get(raft::testing::k_peer_rpc_trust_path,
                    {{raft::testing::k_peer_nonce_header, nonce},
                     {raft::testing::k_peer_request_mac_header,
                      raft::testing::peer_trust_request_mac(cfg.peer_enrollment_key, nonce)}});
    if (!res || res->status != 200 || res->body.empty()) return std::nullopt;
    auto mac = res->get_header_value(raft::testing::k_peer_trust_mac_header);
    if (!raft::testing::constant_time_equals(
            mac, raft::testing::peer_trust_mac(cfg.peer_enrollment_key, nonce, res->body))) {
        std::cerr << "[warn] ca_cluster_node: rejecting RPC trust state from " << http_address
                  << " — missing or invalid peer MAC\n";
        return std::nullopt;
    }
    try {
        return raft::testing::decode_peer_rpc_trust_state(res->body);
    } catch (const std::invalid_argument& ex) {
        std::cerr << "[warn] ca_cluster_node: malformed RPC trust state from " << http_address
                  << ": " << ex.what() << "\n";
        return std::nullopt;
    }
}

// The cluster's current RPC trust state, or std::nullopt while the CA root
// does not exist yet or no leader can be reached. node<Types>::read_state()
// only works on the leader (a follower's call is rejected at once, with no
// forwarding), so the leader reads in-process and a follower asks the
// leader's client-facing API — a separate transport from RPC TLS, which
// keeps working whatever state this node's or the leader's RPC TLS is in.
template<typename Types>
auto fetch_rpc_trust_state(kythira::node<Types>& raft_node,
                           const ca_cluster_node::ca_cluster_node_config& cfg)
    -> std::optional<raft::testing::peer_rpc_trust_state> {
    if (raft_node.is_leader()) {
        try {
            auto state = read_ca_state(raft_node, std::chrono::milliseconds(5000));
            if (!state.has_root_material()) return std::nullopt;
            return rpc_trust_state_of(state, cfg.all_node_ids());
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    // Prefer the Raft-learned leader when known: one HTTP call.
    if (auto leader_id = raft_node.known_leader(); leader_id.has_value()) {
        if (auto leader_http = cfg.http_address_for(*leader_id); leader_http.has_value()) {
            if (auto state = try_fetch_rpc_trust_from(*leader_http, cfg)) {
                return state;
            }
        }
    }

    // Fall back to asking every configured peer directly. known_leader() can
    // stay empty for a node whose RPC TLS accept policy does not yet match
    // its peers' presented identities (not yet widened, or a replacement
    // node still holding only the bootstrap credential after the rest of
    // the cluster cut over): the leader's AppendEntries, which is how
    // known_leader() is learned, never gets through. That is a real
    // deadlock, observed on AWS as a node's data directory staying empty
    // while its log filled with "peer certificate rejected by trust policy".
    // Only the actual leader answers 200; every other peer answers 308/503
    // or refuses the connection and is skipped.
    for (const auto& p : cfg.peers) {
        if (auto state = try_fetch_rpc_trust_from(p.http_address, cfg)) {
            return state;
        }
    }
    return std::nullopt;
}

// The ledger record for a certificate this node just signed with
// `options`. Shared by every issuing path so peer certificates are recorded
// (and so revocable) exactly like client ones.
auto ledger_entry_for(const raft::testing::pem_material& material,
                      const raft::testing::csr_signing_options& options)
    -> raft::testing::ca_ledger_entry {
    raft::testing::ca_ledger_entry entry;
    entry.serial = material.serial;
    entry.subject = extract_subject_cn(material.certificate_pem);
    entry.dns_names = options.dns_names;
    entry.ip_addresses = options.ip_addresses;
    entry.certificate_pem = material.certificate_pem;
    entry.not_before = std::chrono::system_clock::now();
    entry.not_after = entry.not_before + options.validity;
    return entry;
}

// Requirement 5.1: obtains a signed certificate for this node's own RPC
// identity via the cluster's own /v1/certificates route — directly
// in-process if leader (via the already-open `signer`), otherwise over the
// leader's client-facing API, authenticated by the peer-enrollment MAC (not
// the client bearer token) and, for an https:// peer, over TLS verified
// against peer_tls_ca_file().
// Also commits record_rpc_tls_ready(cfg.node_id) — Requirement 5.3 — as
// part of the same call. node<Types>::submit_command() only works on the
// node that is leader right now (it has no forwarding, like read_state(),
// which is why require_leader_or_redirect() exists for the HTTP routes), so
// a follower cannot commit it for itself; piggybacking its id on the CSR
// request lets the leader commit it right after the issuance. That part is
// best-effort in both branches: the follower retries the whole flow next
// tick while have_valid_persisted_peer_cert() is false.
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
        // Record the leader's own peer certificate in the ledger like every
        // other issuance. It used to be signed and used without a ledger
        // entry, so it could never be revoked. Not best-effort: an
        // unrecorded certificate is discarded and the next tick retries.
        raft_node
            .submit_command(raft::testing::encode_record_issuance_command(
                                ledger_entry_for(material, sign_opts)),
                            k_command_timeout)
            .get();
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

    boost::json::object body;
    body["csr_pem"] = csr_pem;
    boost::json::array dns_arr;
    for (const auto& d : sign_opts.dns_names) dns_arr.push_back(boost::json::string(d));
    body["dns_names"] = dns_arr;
    body["server_auth"] = sign_opts.server_auth;
    body["client_auth"] = sign_opts.client_auth;
    // Requirement 5.3: the leader commits record_rpc_tls_ready for this id
    // right after this CSR's issuance (see the function comment).
    body["rpc_tls_ready_node_id"] = cfg.node_id;
    const auto request_body = boost::json::serialize(body);

    // The Raft-known leader first, then every configured peer: once the
    // rest of the cluster has cut over, a node that holds only the bootstrap
    // credential never hears from the leader over RPC, so known_leader()
    // stays empty (see fetch_rpc_trust_state()). Non-leaders answer 308/503.
    std::vector<std::string> candidates;
    if (auto leader_id = raft_node.known_leader(); leader_id.has_value()) {
        if (auto leader_http = cfg.http_address_for(*leader_id)) candidates.push_back(*leader_http);
    }
    for (const auto& p : cfg.peers) candidates.push_back(p.http_address);

    std::string last_error = "no reachable leader";
    for (const auto& address : candidates) {
        std::unique_ptr<httplib::Client> client;
        try {
            client = make_peer_client(address, cfg);
        } catch (const std::exception& ex) {
            last_error = ex.what();
            continue;
        }
        client->set_connection_timeout(5, 0);
        client->set_read_timeout(30, 0);
        auto res = client->Post(
            "/v1/certificates",
            // No bearer token: the enrollment MAC alone authenticates this.
            {{raft::testing::k_peer_enrollment_header,
              raft::testing::peer_enrollment_mac(cfg.peer_enrollment_key, cfg.node_id, csr_pem)}},
            request_body, "application/json");
        if (!res) {
            last_error = address + ": " + httplib::to_string(res.error());
            continue;
        }
        if (res->status != 200) {
            last_error = address + " returned " + std::to_string(res->status) + ": " + res->body;
            continue;
        }
        auto parsed = boost::json::parse(res->body).as_object();
        raft::testing::pem_material material;
        material.certificate_pem = std::string(parsed.at("certificate_pem").as_string());
        if (auto* v = parsed.if_contains("chain_pem")) {
            material.chain_pem = std::string(v->as_string());
        }
        return material;
    }
    throw std::runtime_error("acquire_rpc_peer_certificate: " + last_error);
}

// This node's persisted peer certificate serial, if it has one.
auto persisted_peer_cert_serial(const std::string& data_dir) -> std::optional<std::uint64_t> {
    auto cert_pem = read_whole_file(rpc_peer_cert_path(data_dir));
    if (!cert_pem.has_value()) return std::nullopt;
    BIO* bio = BIO_new_mem_buf(cert_pem->data(), static_cast<int>(cert_pem->size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) return std::nullopt;
    auto serial = raft::testing::cert_serial_u64(cert);
    X509_free(cert);
    return serial;
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
            return typename Types::network_server_type(cfg.rpc_port, cfg.rpc_tls_config,
                                                       cfg.rpc_address);
        } else {
            return typename Types::network_server_type(cfg.rpc_port, cfg.rpc_address);
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
            return typename Types::network_server_type(cfg.rpc_port, cfg.rpc_address);
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
    // The facts this node's RPC trust policy is built from. The policy
    // starts as whatever main() configured (bootstrap fingerprint only, or
    // the persisted root after a restart) and is rebuilt by
    // apply_rpc_trust_policy() whenever one of these changes.
    const std::optional<std::string> bootstrap_fingerprint =
        cfg.rpc_tls_config.trust_policy.bootstrap_fingerprint_hex;
    std::optional<std::string> trusted_root;
    // Requirement 6.2 cutover: set once every node has enrolled, on every
    // node (leader and followers alike), and persisted so a restart does
    // not re-admit the bootstrap credential.
    bool cutover_finalized = std::filesystem::exists(rpc_cutover_marker_path(cfg.data_dir));
    std::set<std::uint64_t> revoked_peer_serials;
    bool trust_widened = false;
    std::optional<std::chrono::steady_clock::time_point> last_trust_refresh;
    std::optional<std::chrono::steady_clock::time_point> root_first_seen_at;
    // Lower bound on how long a node waits, after first observing the CA
    // root exists, before it PRESENTS its own CA-issued identity —
    // deliberately separate from (and larger than) one maintenance-thread
    // tick. Confirmed necessary during this spec's implementation: on a
    // contended host, the node that acquires fastest (typically the
    // leader, whose fetch_rpc_trust_state() is an in-process read) can
    // finish widening-then-acquiring-then-switching within the SAME
    // maintenance tick the root commits on, while a follower's own widen
    // still needs a real HTTP round trip to the leader's /v1/peer/rpc-trust to
    // even begin — and that endpoint itself requires a quorum-confirmed
    // leader read (node<Types>::read_state()'s read-index heartbeat),
    // which depends on RPC connectivity to a majority of followers. If
    // the leader switches its presented identity before ANY follower has
    // widened, every follower starts rejecting the leader's connections,
    // which breaks the very read-index heartbeats read_state() needs —
    // which in turn breaks /v1/peer/rpc-trust for every follower still trying to
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

    // How often a node re-reads the trust state after it has widened, so a
    // revoked peer certificate stops being accepted and a follower notices
    // the cutover completing.
    constexpr auto k_trust_refresh_interval = std::chrono::seconds(5);

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
    //
    // The same refresh carries the rest of the trust state: once every node
    // has enrolled (cutover_complete), the bootstrap credential is dropped
    // from the policy — on followers too, not only on whichever node is
    // leader at that moment, so no node keeps accepting the shared
    // credential as an any-node-id peer — and revoked peer certificates are
    // refused through the CA path (tls_rpc_trust_policy::revoked_serials).
    auto apply_rpc_trust_policy = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            if (!trusted_root.has_value()) return;
            kythira::tls_rpc_trust_policy policy;
            policy.ca_root_pem = *trusted_root;
            if (!cutover_finalized) policy.bootstrap_fingerprint_hex = bootstrap_fingerprint;
            policy = policy.binding_peer_node_ids(cluster_peer_node_ids(cfg))
                         .revoking_serials(revoked_peer_serials);
            rpc_server_handle.reload_trust_policy(policy);
            rpc_client_handle.reload_trust_policy(policy);
        }
    };

    auto maybe_refresh_rpc_trust_policy = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            auto now = std::chrono::steady_clock::now();
            // Every tick until the root is first seen, then (successful or
            // not, so an unreachable leader is not polled every tick) once
            // per k_trust_refresh_interval.
            if (trust_widened) {
                if (last_trust_refresh.has_value() &&
                    now - *last_trust_refresh < k_trust_refresh_interval) {
                    return;
                }
                last_trust_refresh = now;
            }
            auto state = fetch_rpc_trust_state(raft_node, cfg);
            if (!state.has_value()) return;
            if (!root_first_seen_at.has_value()) root_first_seen_at = now;

            bool changed = !trust_widened;
            if (trusted_root != state->root_pem) {
                trusted_root = state->root_pem;
                changed = true;
            }
            if (revoked_peer_serials != state->revoked_peer_serials) {
                revoked_peer_serials = state->revoked_peer_serials;
                changed = true;
            }
            if (state->cutover_complete && !cutover_finalized) {
                try {
                    write_whole_file(rpc_cutover_marker_path(cfg.data_dir), "1\n");
                } catch (const std::exception& ex) {
                    // Still narrow now; only a restart would re-widen.
                    std::cerr << "[warn] ca_cluster_node: failed to persist RPC TLS cutover: "
                              << ex.what() << "\n";
                }
                cutover_finalized = true;
                changed = true;
                std::cerr << "[info] ca_cluster_node: RPC TLS cutover finalized — bootstrap "
                             "credential no longer accepted for new connections\n";
            }
            if (changed) apply_rpc_trust_policy();
            trust_widened = true;
        }
    };

    auto maybe_acquire_rpc_identity = [&] {
        if constexpr (!k_rpc_tls) {
            return;
        } else {
            // A revoked identity is replaced with a fresh key: revocation
            // answers a leaked peer key, and this node can still prove
            // membership through the peer-enrollment key.
            auto own_serial = persisted_peer_cert_serial(cfg.data_dir);
            bool own_revoked = own_serial.has_value() && revoked_peer_serials.contains(*own_serial);
            if (have_valid_persisted_peer_cert(cfg.data_dir) && !own_revoked) return;
            if (!root_first_seen_at.has_value() ||
                std::chrono::steady_clock::now() - *root_first_seen_at < k_identity_acquire_grace) {
                return;
            }
            if (!trusted_root.has_value()) return;
            const std::string root_pem = *trusted_root;

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

            if (!issued_peer_cert_ok(material.certificate_pem, root_pem, cfg.node_id)) {
                std::cerr << "[warn] ca_cluster_node: issued RPC peer certificate does not chain "
                             "to the cluster root or lacks this node's peer name — discarding, "
                             "will retry\n";
                return;
            }

            try {
                persist_rpc_peer_identity(cfg.data_dir, material.certificate_pem,
                                          csr.private_key_pem, root_pem);
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
            // /renew authenticates by mTLS, which a plaintext peer address
            // cannot carry.
            if (!leader_http->starts_with("https://")) return;

            auto leaf_opts = rpc_peer_identity_options(cfg.node_id);
            auto csr = raft::testing::generate_key_and_csr(leaf_opts);

            std::unique_ptr<httplib::Client> renew_client;
            try {
                renew_client = make_peer_client(*leader_http, cfg, rpc_peer_cert_path(cfg.data_dir),
                                                rpc_peer_key_path(cfg.data_dir));
            } catch (const std::exception&) {
                return;
            }
            renew_client->set_connection_timeout(5, 0);
            renew_client->set_read_timeout(30, 0);

            boost::json::object body;
            body["csr_pem"] = csr.csr_pem;
            auto res = renew_client->Post("/v1/certificates/renew", boost::json::serialize(body),
                                          "application/json");
            if (!res || res->status != 200) {
                std::cerr << "[warn] ca_cluster_node: RPC peer identity renewal failed, will "
                             "retry\n";
                return;
            }

            auto trust = fetch_rpc_trust_state(raft_node, cfg);
            if (!trust.has_value()) return;

            try {
                auto parsed = boost::json::parse(res->body).as_object();
                std::string new_cert_pem = std::string(parsed.at("certificate_pem").as_string());
                if (!issued_peer_cert_ok(new_cert_pem, trust->root_pem, cfg.node_id)) {
                    std::cerr << "[warn] ca_cluster_node: renewed RPC peer certificate does not "
                                 "chain to the cluster root or lacks this node's peer name — "
                                 "discarding, will retry\n";
                    return;
                }
                persist_rpc_peer_identity(cfg.data_dir, new_cert_pem, csr.private_key_pem,
                                          trust->root_pem);
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
                maybe_refresh_rpc_trust_policy();
                maybe_acquire_rpc_identity();
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
    // One httplib server per address --http-address resolves to ("*" is IPv4
    // and IPv6; see net_bind::httplib_listeners), each built by
    // make_http_server and given the same routes by configure_http_server.
    std::function<std::unique_ptr<httplib::Server>()> make_http_server;

    if (!cfg.tls_cert_path.empty()) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        make_http_server = [&]() -> std::unique_ptr<httplib::Server> {
            auto ssl_server = std::make_unique<httplib::SSLServer>(cfg.tls_cert_path.c_str(),
                                                                   cfg.tls_key_path.c_str());
            if (!ssl_server->is_valid()) {
                throw std::runtime_error("failed to initialize TLS with cert " + cfg.tls_cert_path +
                                         " / key " + cfg.tls_key_path);
            }
            SSL_CTX_set_verify(ssl_server->ssl_context(), SSL_VERIFY_PEER,
                               raft::testing::accept_any_peer_certificate);
            return ssl_server;
        };
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
        // main() already refused this unless plaintext_http_permitted().
        make_http_server = [] { return std::make_unique<httplib::Server>(); };
        std::cerr << "ca_cluster_node: WARNING: client HTTP API is PLAINTEXT on "
                  << cfg.http_bind_address << ":" << cfg.http_port
                  << (cfg.allow_plaintext_http ? " (plaintext opted in)" : " (loopback only)")
                  << "; the bearer token and issued certificates cross it in the clear\n";
    }

    kythira::net_bind::httplib_listeners<> http_listeners;
    g_http_listeners.store(&http_listeners);
    std::signal(SIGINT, on_http_signal);
    std::signal(SIGTERM, sigterm_handler);
    // SIGTERM triggers both: sigterm_handler() wakes the main wait below, and
    // that same shutdown path stops the HTTP server explicitly (see below) —
    // SIGINT is wired to stop the HTTP server directly for interactive use.

    // Everything the route handlers capture by reference lives out here, not
    // inside configure_http_server, so it outlives every server it configures.
    const std::string bearer_prefix = "Bearer ";
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

    auto configure_http_server = [&](httplib::Server& srv) {
        auto* server = &srv;
        server->set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
            if (req.method == "POST" && req.path == "/v1/certificates/renew") {
                return httplib::Server::HandlerResponse::Unhandled;  // authenticates via mTLS in
                                                                     // its own handler
            }
            if (req.path == "/healthz") {
                return httplib::Server::HandlerResponse::Unhandled;  // health checks must work with
                                                                     // no credentials
            }
            // Peer-to-peer calls authenticate with the peer-enrollment key
            // instead of the client bearer token, which followers never send
            // to a peer:
            //  - GET /v1/peer/rpc-trust needs a valid request MAC over its
            //    nonce, and nothing else gets in: a bearer token holder must
            //    not be able to collect response MACs to test passphrase
            //    guesses against;
            //  - POST /v1/certificates carrying an enrollment MAC — admitted here
            //    only provisionally; the handler refuses it unless it verifies
            //    as a complete peer enrollment (classify_peer_enrollment).
            if (req.path == raft::testing::k_peer_rpc_trust_path) {
                auto nonce = req.get_header_value(raft::testing::k_peer_nonce_header);
                if (req.method == "GET" && raft::testing::is_well_formed_nonce(nonce) &&
                    raft::testing::constant_time_equals(
                        req.get_header_value(raft::testing::k_peer_request_mac_header),
                        raft::testing::peer_trust_request_mac(cfg.peer_enrollment_key, nonce))) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                res.status = 401;
                res.set_content(json_error("unauthorized"), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method == "POST" && req.path == "/v1/certificates" &&
                req.has_header(raft::testing::k_peer_enrollment_header)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            auto auth = req.get_header_value("Authorization");
            if (!raft::testing::constant_time_equals(auth, bearer_prefix + cfg.auth_token)) {
                res.status = 401;
                res.set_content(json_error("unauthorized"), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

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

        // Peer-only (pre-routing admits nothing but a valid request MAC):
        // the RPC trust state a follower builds its RPC TLS policy from.
        server->Get(raft::testing::k_peer_rpc_trust_path, [&](const httplib::Request& req,
                                                              httplib::Response& res) {
            if (!require_leader_or_redirect(req, res)) return;
            try {
                auto state = read_ca_state(raft_node, k_command_timeout);
                if (!state.has_root_material()) {
                    res.status = 503;
                    res.set_content(json_error("not_bootstrapped"), "application/json");
                    return;
                }
                auto body = raft::testing::encode_peer_rpc_trust_state(
                    rpc_trust_state_of(state, cfg.all_node_ids()));
                res.set_header(raft::testing::k_peer_trust_mac_header,
                               raft::testing::peer_trust_mac(
                                   cfg.peer_enrollment_key,
                                   req.get_header_value(raft::testing::k_peer_nonce_header), body));
                res.set_content(body, "application/json");
            } catch (const std::exception& ex) {
                res.status = 503;
                res.set_content(json_error(ex.what()), "application/json");
            }
        });

        server->Post("/v1/certificates", [&](const httplib::Request& req, httplib::Response& res) {
            // Pre-routing lets a request without the bearer token through only
            // if it carries an enrollment MAC; such a request must then verify
            // as a peer enrollment below, or it is refused.
            const bool bearer_ok = raft::testing::constant_time_equals(
                req.get_header_value("Authorization"), bearer_prefix + cfg.auth_token);
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

                // Reserved "ca-cluster-node-*" names and rpc_tls_ready_node_id
                // are Raft peer enrollment, not ordinary issuance: they need the
                // peer-enrollment MAC, which the client bearer token alone
                // cannot produce (see classify_peer_enrollment).
                std::optional<std::uint64_t> ready_node_id;
                if (auto* v = body.if_contains("rpc_tls_ready_node_id")) {
                    if (!v->is_number()) {
                        throw std::invalid_argument("rpc_tls_ready_node_id must be a number");
                    }
                    ready_node_id = v->to_number<std::uint64_t>();
                }
                auto ids = cfg.all_node_ids();
                auto enrollment = raft::testing::classify_peer_enrollment(
                    options, ready_node_id,
                    req.get_header_value(raft::testing::k_peer_enrollment_header),
                    std::set<std::uint64_t>(ids.begin(), ids.end()), cfg.peer_enrollment_key,
                    csr_pem);
                if (!enrollment.allowed) {
                    res.status = bearer_ok ? 403 : 401;
                    res.set_content(json_error(bearer_ok ? enrollment.error : "unauthorized"),
                                    "application/json");
                    return;
                }
                if (!bearer_ok && !enrollment.node_id.has_value()) {
                    res.status = 401;
                    res.set_content(json_error("unauthorized"), "application/json");
                    return;
                }

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

                auto entry = ledger_entry_for(material, options);

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
                if (enrollment.node_id.has_value()) {
                    try {
                        raft_node
                            .submit_command(raft::testing::encode_record_rpc_tls_ready_command(
                                                *enrollment.node_id),
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

                res.set_content(
                    boost::json::serialize(raft::testing::pem_material_to_json(material)),
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
                res.set_content(json_error("renew requires an mTLS connection"),
                                "application/json");
                return;
            }
            X509* peer_cert = SSL_get1_peer_certificate(req.ssl);
            if (peer_cert == nullptr) {
                res.status = 401;
                res.set_content(json_error("no client certificate presented"), "application/json");
                return;
            }
            raft::testing::x509_ptr_generic peer_cert_owner{peer_cert};
            try {
                auto state = read_ca_state(raft_node, k_command_timeout);
                if (!raft::testing::cert_chains_to_root(peer_cert, state.root_certificate_pem())) {
                    res.status = 401;
                    res.set_content(
                        json_error("presented certificate does not chain to this CA's root"),
                        "application/json");
                    return;
                }
                // A revoked certificate must not be able to renew itself into a
                // fresh, unrevoked serial — that would make revocation a no-op.
                auto presented_serial = raft::testing::cert_serial_u64(peer_cert);
                bool revoked = !presented_serial.has_value();
                for (const auto& e : state.ledger()) {
                    if (presented_serial.has_value() && e.serial == *presented_serial &&
                        e.revoked_at.has_value()) {
                        revoked = true;
                    }
                }
                if (revoked) {
                    res.status = 401;
                    res.set_content(json_error("presented certificate has been revoked"),
                                    "application/json");
                    return;
                }
                auto options = raft::testing::options_from_presented_cert(peer_cert);

                auto body = boost::json::parse(req.body).as_object();
                auto* csr_val = body.if_contains("csr_pem");
                if (csr_val == nullptr || !csr_val->is_string()) {
                    res.status = 400;
                    res.set_content(json_error("csr_pem is required"), "application/json");
                    return;
                }
                std::string csr_pem = std::string(csr_val->as_string());
                // Renewal keeps the presented subject: the CSR may not change it.
                bool same_subject = raft::testing::csr_subject_matches_cert(peer_cert, csr_pem);
                if (!same_subject) {
                    res.status = 400;
                    res.set_content(
                        json_error("renewal CSR subject must match the presented certificate"),
                        "application/json");
                    return;
                }
                if (auto validity = raft::testing::parse_validity_days(body)) {
                    options.validity = *validity;
                }

                std::unique_lock signer_lock(signer_mu);
                if (signer == nullptr) {
                    res.status = 503;
                    res.set_content(json_error("not_ready"), "application/json");
                    return;
                }
                auto material = signer->sign_csr(csr_pem, options);
                signer_lock.unlock();

                auto entry = ledger_entry_for(material, options);

                raft_node
                    .submit_command(raft::testing::encode_record_issuance_command(entry),
                                    k_command_timeout)
                    .get();

                res.set_content(
                    boost::json::serialize(raft::testing::pem_material_to_json(material)),
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

        server->Post(
            "/v1/certificates/revoke", [&](const httplib::Request& req, httplib::Response& res) {
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

                    auto result =
                        raft_node
                            .submit_command(
                                raft::testing::encode_record_revocation_command(serial, revoked_at),
                                k_command_timeout)
                            .get();
                    if (!result.empty()) {
                        std::string result_str(reinterpret_cast<const char*>(result.data()),
                                               result.size());
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
    };

    try {
        http_listeners.bind(cfg.http_bind_address, cfg.http_port, configure_http_server,
                            "ca_cluster_node: --http-address", make_http_server);
    } catch (const std::exception& ex) {
        std::cerr << "ca_cluster_node: " << ex.what() << "\n";
        g_stop = true;
        raft_node.stop();
        election_timer.join();
        heartbeat_timer.join();
        maintenance_thread.request_stop();
        maintenance_thread.join();
        return 1;
    }
    std::cerr << "[info] ca_cluster_node: HTTP API listening on " << cfg.http_bind_address << ":"
              << cfg.http_port << "\n";
    http_listeners.start();

    {
        std::unique_lock lock(g_stop_mu);
        g_stop_cv.wait(lock, [] { return g_stop.load(); });
    }

    std::cerr << "[info] ca_cluster_node shutting down\n";
    g_stop = true;
    // Stop accepting now; joining the accept loops waits for in-flight
    // handlers, so that comes after raft_node.stop() below.
    http_listeners.request_stop();

    // raft_node.stop() unconditionally rejects every pending submit_command()/
    // read_state() future (CommitWaiter::cancel_all_operations) and MUST run
    // before joining any thread that might currently be blocked inside one of
    // those calls' .get() — maintenance_thread's leader-transition no-op
    // commit (ensure_signer/maybe_bootstrap), or an in-flight /v1/certificates
    // HTTP handler. The only other thing that ever resolves such a call is
    // check_heartbeat_timeout()'s CommitWaiter::cancel_timed_out_operations(),
    // which fires solely while heartbeat_timer is still ticking — and
    // heartbeat_timer is itself one of the threads joined below. Previously
    // this call came LAST, after every join(): a thread blocked in .get() on
    // a commit that can never land (this node has just lost the quorum it
    // needs, precisely because it and/or its peers are shutting down) had
    // nothing left running to either complete or time out its operation,
    // deadlocking maintenance_thread.join() forever and, with it, this whole
    // process's exit. That is the documented intermittent hang tracked in
    // doc/TODO.md ("ca_cluster_node_test intermittent hang") — reproduced
    // there as the test's own waitpid() on this process never returning.
    // Calling stop() here, before any of those joins, guarantees such a call
    // is force-rejected immediately instead of racing an already-stopped
    // timeout mechanism.
    raft_node.stop();

    http_listeners.stop();
    g_http_listeners.store(nullptr);
    election_timer.join();
    heartbeat_timer.join();
    maintenance_thread.request_stop();
    maintenance_thread.join();

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

    if (cfg.auth_token_from_argv) {
        std::cerr << "ca_cluster_node: WARNING: --auth-token puts the bearer token in the process "
                     "command line, readable by any local user via /proc or ps; set "
                     "$CA_SERVICE_AUTH_TOKEN instead\n";
    }

    // The bearer token and every issued certificate cross the client HTTP
    // API. Without TLS, refuse unless it is loopback-only or the operator
    // explicitly opted in (the same rule as plaintext Raft RPC below).
    if (cfg.tls_cert_path.empty() && !ca_cluster_node::plaintext_http_permitted(cfg)) {
        std::cerr << "ca_cluster_node: refusing to start: the client HTTP API would be plaintext "
                     "on "
                  << cfg.http_bind_address << ":" << cfg.http_port
                  << " (no --tls-cert/--tls-key given).\n"
                     "  Pass --tls-cert/--tls-key, bind --http-address to loopback for "
                     "single-host use, or pass --allow-plaintext-http (or set "
                     "CA_CLUSTER_ALLOW_PLAINTEXT_HTTP=1) to accept a plaintext API on a trusted "
                     "network.\n";
        return 1;
    }

    std::string unseal_passphrase;
    try {
        unseal_passphrase = read_unseal_key(cfg.unseal_key_file);
        cfg.peer_enrollment_key = raft::testing::derive_peer_enrollment_key(unseal_passphrase);
    } catch (const std::exception& e) {
        std::cerr << "ca_cluster_node: " << e.what() << "\n";
        return 1;
    }

#ifdef KYTHIRA_HAS_OPENSSL
    // Requirement 2.3 / 3.2 / 3.3: RPC TLS is "enabled" if the operator gave
    // --rpc-tls-cert/--rpc-tls-key OR this node already has a persisted peer
    // certificate from a prior cutover (Property 5 — the bootstrap
    // credential flags are not required on every subsequent restart).
    // Neither present: plain TCP, but only where plaintext_rpc_permitted()
    // allows it (see below). --rpc-tls-cert given but unreadable/invalid, with no
    // persisted fallback: fail closed (Requirement 2.3), surfaced naturally
    // by tls_tcp_rpc_server's constructor throwing, caught below.
    bool have_persisted = have_valid_persisted_peer_cert(cfg.data_dir);
    bool use_rpc_tls = !cfg.rpc_tls_cert_path.empty() || have_persisted;

    if (use_rpc_tls) {
        kythira::tls_tcp_rpc_config rpc_tls_config;
        if (have_persisted) {
            rpc_tls_config.cert_path = rpc_peer_cert_path(cfg.data_dir);
            rpc_tls_config.key_path = rpc_peer_key_path(cfg.data_dir);
            // Requirement 6.1: still dual-trust at startup unless this node
            // recorded the cutover (rpc_cutover_marker_path) before it went
            // down. Without that record it doesn't know whether every peer
            // had reached rpc_tls_ready, and maybe_refresh_rpc_trust_policy()
            // re-narrows within one maintenance tick once the leader reports
            // the full ready set. Using ca_root_only immediately would risk
            // rejecting a peer that itself hasn't cut over yet.
            auto root_pem = read_whole_file(rpc_peer_root_path(cfg.data_dir));
            kythira::tls_rpc_trust_policy policy;
            policy.ca_root_pem = root_pem;
            policy = policy.binding_peer_node_ids(cluster_peer_node_ids(cfg));
            // Once this node has seen the whole cluster cut over, a restart
            // trusts the root alone: re-admitting the bootstrap credential
            // here would let its holder speak as any node again.
            const bool cutover_done =
                std::filesystem::exists(rpc_cutover_marker_path(cfg.data_dir));
            if (!cfg.rpc_tls_cert_path.empty() && !cutover_done) {
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

#endif  // KYTHIRA_HAS_OPENSSL

    // No RPC TLS material: plain, unauthenticated TCP would let anyone who
    // can reach --rpc-port drive Raft. Refuse unless the listener is
    // loopback-only or the operator explicitly opted in.
    if (!ca_cluster_node::plaintext_rpc_permitted(cfg)) {
        std::cerr << "ca_cluster_node: refusing to start: Raft RPC would be plaintext on "
                  << cfg.rpc_address << ":" << cfg.rpc_port
                  << " (no --rpc-tls-cert/--rpc-tls-key given, and no persisted peer certificate "
                     "found under --data-dir).\n"
                     "  Provision the RPC bootstrap credential and pass --rpc-tls-cert/"
                     "--rpc-tls-key, bind --rpc-address to loopback for single-host use, or pass "
                     "--allow-plaintext-rpc (or set CA_CLUSTER_ALLOW_PLAINTEXT_RPC=1) to accept "
                     "unauthenticated Raft RPC on a trusted network.\n";
        return 1;
    }
    std::cerr << "ca_cluster_node: WARNING: Raft RPC is PLAINTEXT and UNAUTHENTICATED on "
              << cfg.rpc_address << ":" << cfg.rpc_port
              << (cfg.allow_plaintext_rpc ? " (plaintext opted in)" : " (loopback only)")
              << "; any process that can reach this port can drive Raft\n";

    return run_ca_cluster_node<ca_cluster_raft_types_plain>(std::move(cfg),
                                                            std::move(unseal_passphrase));
}
