// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: grpc-tls-reload
//
// End-to-end tests of TLS reload on a real grpc_server and grpc_client
// (.kiro/specs/grpc-tls-reload/, Task 5). What a server presents is read
// with a raw OpenSSL handshake (tls_handshake_probe.hpp), so each check sees
// what a new peer would see:
//
// - an explicit server reload reaches new handshakes while the existing
//   channel keeps working (Properties 1 and 2)
// - an invalid reload throws and changes nothing (Property 3)
// - root rotation changes which clients the server accepts
// - a client's reloaded identity is what its cached channel presents on the
//   next handshake
// - auto-reload picks up atomically replaced files, and stops when disabled
// - one material source feeds a server and a client
// - an issuing source (certificate-provider hook) can back a server
// - the configuration and logic errors the spec requires

#define BOOST_TEST_MODULE GrpcTlsReloadIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_transport_impl.hpp>
#include <raft/issuing_tls_material_source.hpp>
#include <raft/tls_material_source.hpp>

#include "recording_metrics.hpp"
#include "tls_handshake_probe.hpp"

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

using namespace std::chrono_literals;
using kythira::testing::handshake;
using kythira::testing::recording_metrics;
using kythira::testing::serial_of;

struct recording_types {
    template<typename T> using future_template = kythira::Future<T>;
    using metrics_type = recording_metrics;
    using executor_type = folly::CPUThreadPoolExecutor;
};
using server_t = kythira::grpc_server<recording_types>;
using client_t = kythira::grpc_client<recording_types>;

auto issue(raft::testing::certificate_authority& ca, const std::string& cn, bool server,
           bool client) -> raft::testing::pem_material {
    raft::testing::leaf_certificate_options opts;
    opts.subject.common_name = cn;
    opts.dns_names = {"localhost"};
    opts.ip_addresses = {"127.0.0.1"};
    opts.server_auth = server;
    opts.client_auth = client;
    return ca.issue(opts);
}

// A directory of cert.pem, key.pem and ca.pem, replaced atomically the way an
// external renewal agent is expected to, and removed when the test ends.
struct cert_dir {
    std::filesystem::path dir;
    cert_dir() {
        char tmpl[] = "/tmp/kythira_grpc_reload_XXXXXX";
        BOOST_REQUIRE(::mkdtemp(tmpl) != nullptr);
        dir = tmpl;
    }
    ~cert_dir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    [[nodiscard]] auto cert() const -> std::string { return (dir / "cert.pem").string(); }
    [[nodiscard]] auto key() const -> std::string { return (dir / "key.pem").string(); }
    [[nodiscard]] auto ca() const -> std::string { return (dir / "ca.pem").string(); }
    auto replace(const std::string& path, const std::string& contents) const -> void {
        auto tmp = path + ".tmp";
        std::ofstream(tmp, std::ios::binary | std::ios::trunc) << contents;
        std::filesystem::rename(tmp, path);
    }
    auto write(const raft::testing::pem_material& leaf, const std::string& roots) const -> void {
        replace(cert(), leaf.certificate_pem);
        replace(key(), leaf.private_key_pem);
        replace(ca(), roots);
    }
};

template<typename Pred>
auto eventually(Pred pred, std::chrono::milliseconds deadline = 8s) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return pred();
}

auto vote_handler(const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
    return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
}

auto vote(client_t& client) -> bool {
    kythira::request_vote_request<> req{
        ._term = 3, ._candidate_id = 2, ._last_log_index = 0, ._last_log_term = 0};
    try {
        return client.send_request_vote(1, req, 2000ms).get().vote_granted();
    } catch (const std::exception&) {
        return false;
    }
}

auto target(std::uint16_t port) -> std::unordered_map<std::uint64_t, std::string> {
    return {{1, "127.0.0.1:" + std::to_string(port)}};
}

// Server TLS from files; client TLS from PEM strings trusting `roots`.
auto file_server_config(const cert_dir& files, bool require_client_cert)
    -> kythira::grpc_server_config {
    kythira::grpc_server_config cfg;
    cfg.enable_tls = true;
    cfg.server_cert_path = files.cert();
    cfg.server_key_path = files.key();
    if (require_client_cert) {
        cfg.require_client_cert = true;
        cfg.ca_cert_path = files.ca();
    }
    return cfg;
}

auto pem_client_config(const std::string& roots, const raft::testing::pem_material* identity)
    -> kythira::grpc_client_config {
    kythira::grpc_client_config cfg;
    cfg.enable_tls = true;
    cfg.ca_cert_pem = roots;
    cfg.target_name_override = "localhost";
    if (identity != nullptr) {
        cfg.client_cert_pem = identity->certificate_pem;
        cfg.client_key_pem = identity->private_key_pem;
    }
    return cfg;
}

}  // namespace

// Properties 1 and 2: after reload_tls_material(), new handshakes present the
// new leaf within the refresh bound, and the existing client's channel keeps
// serving RPCs throughout.
BOOST_AUTO_TEST_CASE(server_explicit_reload_reaches_new_handshakes) {
    raft::testing::certificate_authority ca;
    auto gen1 = issue(ca, "server-1", true, false);
    auto gen2 = issue(ca, "server-2", true, false);
    cert_dir files;
    files.write(gen1, ca.root_certificate_pem());

    folly::CPUThreadPoolExecutor exec(4);
    recording_metrics metrics;
    server_t server("127.0.0.1", 0, file_server_config(files, false), metrics, exec);
    server.register_request_vote_handler(vote_handler);
    server.start();

    client_t client(target(server.bound_port()),
                    pem_client_config(ca.root_certificate_pem(), nullptr), recording_metrics{},
                    exec);
    BOOST_REQUIRE(vote(client));
    BOOST_TEST(handshake(server.bound_port()).serial == serial_of(gen1.certificate_pem));

    std::atomic<bool> stop{false};
    std::atomic<int> failed_calls{0};
    std::thread caller([&] {
        while (!stop.load()) {
            if (!vote(client)) {
                ++failed_calls;
            }
        }
    });

    files.write(gen2, ca.root_certificate_pem());
    server.reload_tls_material();
    BOOST_TEST(eventually(
        [&] { return handshake(server.bound_port()).serial == serial_of(gen2.certificate_pem); }));
    stop = true;
    caller.join();

    BOOST_TEST(failed_calls.load() == 0);
    BOOST_TEST(vote(client));
    BOOST_TEST(metrics.recorder()->count_named("grpc.server.tls_reload.succeeded") == 1U);
    BOOST_TEST(metrics.recorder()->dimension_values("grpc.server.tls_reload.succeeded",
                                                    "generation") == std::vector<std::string>{"2"},
               boost::test_tools::per_element());
    server.stop();
}

// Property 3: a half-written update (new certificate, old key) is refused,
// the failure is counted once, and the server keeps presenting the old leaf.
BOOST_AUTO_TEST_CASE(invalid_reload_keeps_serving_old_material) {
    raft::testing::certificate_authority ca;
    auto gen1 = issue(ca, "server-1", true, false);
    auto gen2 = issue(ca, "server-2", true, false);
    cert_dir files;
    files.write(gen1, ca.root_certificate_pem());

    folly::CPUThreadPoolExecutor exec(2);
    recording_metrics metrics;
    server_t server("127.0.0.1", 0, file_server_config(files, false), metrics, exec);
    server.register_request_vote_handler(vote_handler);
    server.start();

    files.replace(files.cert(), gen2.certificate_pem);
    BOOST_CHECK_THROW(server.reload_tls_material(), kythira::grpc_tls_configuration_error);
    BOOST_TEST(metrics.recorder()->count_named("grpc.server.tls_reload.failed") == 1U);
    BOOST_TEST(metrics.recorder()->count_named("grpc.server.tls_reload.succeeded") == 0U);

    std::this_thread::sleep_for(2500ms);  // Past two refresh intervals.
    BOOST_TEST(handshake(server.bound_port()).serial == serial_of(gen1.certificate_pem));
    server.stop();
}

// Root rotation: once the server trusts CA-2 instead of CA-1, a client with a
// CA-1 identity is refused and a client with a CA-2 identity is accepted.
BOOST_AUTO_TEST_CASE(server_root_rotation) {
    raft::testing::certificate_authority server_ca;
    raft::testing::certificate_authority ca1;
    raft::testing::certificate_authority ca2;
    auto server_leaf = issue(server_ca, "server", true, false);
    auto client1 = issue(ca1, "client-1", false, true);
    auto client2 = issue(ca2, "client-2", false, true);
    cert_dir files;
    files.write(server_leaf, ca1.root_certificate_pem());

    folly::CPUThreadPoolExecutor exec(4);
    server_t server("127.0.0.1", 0, file_server_config(files, true), recording_metrics{}, exec);
    server.register_request_vote_handler(vote_handler);
    server.start();
    const auto& roots = server_ca.root_certificate_pem();

    {
        client_t c1(target(server.bound_port()), pem_client_config(roots, &client1),
                    recording_metrics{}, exec);
        BOOST_TEST(vote(c1));
    }

    files.write(server_leaf, ca2.root_certificate_pem());
    server.reload_tls_material();
    BOOST_TEST(eventually([&] {
        client_t c2(target(server.bound_port()), pem_client_config(roots, &client2),
                    recording_metrics{}, exec);
        client_t c1(target(server.bound_port()), pem_client_config(roots, &client1),
                    recording_metrics{}, exec);
        return vote(c2) && !vote(c1);
    }));
    server.stop();
}

// A client's reloaded identity is what its existing, cached channel presents
// the next time it handshakes. The first server trusts only CA-1; after the
// client reloads to a CA-2 identity, it is replaced on the same port by one
// that trusts only CA-2, and the same client object reconnects to it.
BOOST_AUTO_TEST_CASE(client_identity_reload_reaches_cached_channels) {
    raft::testing::certificate_authority server_ca;
    raft::testing::certificate_authority ca1;
    raft::testing::certificate_authority ca2;
    auto server_leaf = issue(server_ca, "server", true, false);
    auto client1 = issue(ca1, "client-1", false, true);
    auto client2 = issue(ca2, "client-2", false, true);

    auto server_cfg = [&](const std::string& client_roots) {
        kythira::grpc_server_config cfg;
        cfg.enable_tls = true;
        cfg.server_cert_pem = server_leaf.certificate_pem;
        cfg.server_key_pem = server_leaf.private_key_pem;
        cfg.require_client_cert = true;
        cfg.ca_cert_pem = client_roots;
        return cfg;
    };

    folly::CPUThreadPoolExecutor exec(4);
    auto first = std::make_unique<server_t>("127.0.0.1", 0, server_cfg(ca1.root_certificate_pem()),
                                            recording_metrics{}, exec);
    first->register_request_vote_handler(vote_handler);
    first->start();
    const auto port = first->bound_port();

    cert_dir client_files;
    client_files.write(client1, server_ca.root_certificate_pem());
    kythira::grpc_client_config client_cfg;
    client_cfg.enable_tls = true;
    client_cfg.client_cert_path = client_files.cert();
    client_cfg.client_key_path = client_files.key();
    client_cfg.ca_cert_path = client_files.ca();
    client_cfg.target_name_override = "localhost";
    recording_metrics client_metrics;
    client_t client(target(port), client_cfg, client_metrics, exec);
    BOOST_REQUIRE(vote(client));

    client_files.write(client2, server_ca.root_certificate_pem());
    client.reload_tls_material();
    BOOST_TEST(client_metrics.recorder()->count_named("grpc.client.tls_reload.succeeded") == 1U);
    std::this_thread::sleep_for(2500ms);  // Let the provider pick it up.

    first->stop();
    first.reset();
    server_t second("127.0.0.1", port, server_cfg(ca2.root_certificate_pem()), recording_metrics{},
                    exec);
    second.register_request_vote_handler(vote_handler);
    second.start();
    // gRPC backs off between reconnects, so allow a few attempts.
    BOOST_TEST(eventually([&] { return vote(client); }, 15s));
    second.stop();
}

// Auto-reload notices atomically replaced files within poll + refresh, and
// stops noticing once disabled.
BOOST_AUTO_TEST_CASE(server_auto_reload_follows_files_until_disabled) {
    raft::testing::certificate_authority ca;
    auto gen1 = issue(ca, "server-1", true, false);
    auto gen2 = issue(ca, "server-2", true, false);
    auto gen3 = issue(ca, "server-3", true, false);
    cert_dir files;
    files.write(gen1, ca.root_certificate_pem());

    folly::CPUThreadPoolExecutor exec(2);
    recording_metrics metrics;
    server_t server("127.0.0.1", 0, file_server_config(files, false), metrics, exec);
    server.register_request_vote_handler(vote_handler);
    server.start();
    server.enable_auto_reload(1s);

    files.write(gen2, ca.root_certificate_pem());
    BOOST_TEST(eventually(
        [&] { return handshake(server.bound_port()).serial == serial_of(gen2.certificate_pem); }));
    BOOST_TEST(metrics.recorder()->count_named("grpc.server.tls_reload.succeeded") >= 1U);

    server.disable_auto_reload();
    files.write(gen3, ca.root_certificate_pem());
    std::this_thread::sleep_for(3s);
    BOOST_TEST(handshake(server.bound_port()).serial == serial_of(gen2.certificate_pem));

    // stop() turns auto-reload off too, and the destructor copes either way.
    server.enable_auto_reload(1s);
    server.stop();
}

// One self-refreshing source feeds a server and a client, so a node presents
// one identity both ways, and both apply each publish.
BOOST_AUTO_TEST_CASE(shared_source_feeds_server_and_client) {
    raft::testing::certificate_authority ca;
    auto gen1 = issue(ca, "node-1", true, true);
    auto gen2 = issue(ca, "node-1", true, true);
    cert_dir files;
    files.write(gen1, ca.root_certificate_pem());
    auto source = std::make_shared<kythira::file_tls_material_source>(
        kythira::tls_material_paths{.certificate_chain_path = files.cert(),
                                    .private_key_path = files.key(),
                                    .root_certificates_path = files.ca()},
        200ms);

    folly::CPUThreadPoolExecutor exec(4);
    recording_metrics server_metrics;
    kythira::grpc_server_config server_cfg;
    server_cfg.enable_tls = true;
    server_cfg.require_client_cert = true;
    server_cfg.material_source = source;
    server_t server("127.0.0.1", 0, server_cfg, server_metrics, exec);
    server.register_request_vote_handler(vote_handler);
    server.start();

    recording_metrics client_metrics;
    kythira::grpc_client_config client_cfg;
    client_cfg.enable_tls = true;
    client_cfg.material_source = source;
    client_cfg.target_name_override = "localhost";
    client_t client(target(server.bound_port()), client_cfg, client_metrics, exec);
    BOOST_REQUIRE(vote(client));

    // The source owns change detection.
    BOOST_CHECK_THROW(server.enable_auto_reload(1s), std::logic_error);
    BOOST_CHECK_THROW(client.enable_auto_reload(1s), std::logic_error);

    files.write(gen2, ca.root_certificate_pem());
    BOOST_TEST(eventually(
        [&] { return handshake(server.bound_port()).serial == serial_of(gen2.certificate_pem); }));
    BOOST_TEST(eventually([&] {
        return server_metrics.recorder()->count_named("grpc.server.tls_reload.succeeded") >= 1U &&
               client_metrics.recorder()->count_named("grpc.client.tls_reload.succeeded") >= 1U;
    }));
    BOOST_TEST(vote(client));
    server.stop();
}

// The certificate-provider hook: a server whose identity is issued, and
// renewed, by an issuing_tls_material_source over local_certificate_provider.
BOOST_AUTO_TEST_CASE(issuing_source_backs_a_server) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<raft::testing::local_certificate_provider>(ca);
    kythira::issuing_tls_material_options opts;
    opts.subject.common_name = "node-1";
    opts.signing.dns_names = {"localhost"};
    opts.signing.ip_addresses = {"127.0.0.1"};
    opts.signing.validity = 4s;
    auto source = std::make_shared<
        kythira::issuing_tls_material_source<raft::testing::local_certificate_provider>>(provider,
                                                                                         opts);
    BOOST_REQUIRE(source->generation() == 1U);

    folly::CPUThreadPoolExecutor exec(2);
    kythira::grpc_server_config cfg;
    cfg.enable_tls = true;
    cfg.material_source = source;
    server_t server("127.0.0.1", 0, cfg, recording_metrics{}, exec);
    server.register_request_vote_handler(vote_handler);
    server.start();

    client_t client(target(server.bound_port()),
                    pem_client_config(ca.root_certificate_pem(), nullptr), recording_metrics{},
                    exec);
    BOOST_TEST(vote(client));
    auto first = handshake(server.bound_port()).serial;
    // Renewal at two thirds of 4s publishes a new certificate, which the
    // server then presents.
    BOOST_TEST(eventually([&] { return handshake(server.bound_port()).serial != first; }, 10s));
    BOOST_TEST(vote(client));
    server.stop();
}

BOOST_AUTO_TEST_CASE(configuration_errors_and_logic_errors) {
    raft::testing::certificate_authority ca;
    auto leaf = issue(ca, "server", true, true);
    cert_dir files;
    files.write(leaf, ca.root_certificate_pem());
    folly::CPUThreadPoolExecutor exec(1);

    // Plaintext transports have nothing to reload.
    server_t plain_server("127.0.0.1", 0, {}, recording_metrics{}, exec);
    BOOST_CHECK_THROW(plain_server.reload_tls_material(), std::logic_error);
    BOOST_CHECK_THROW(plain_server.enable_auto_reload(1s), std::logic_error);
    client_t plain_client(target(1), {}, recording_metrics{}, exec);
    BOOST_CHECK_THROW(plain_client.reload_tls_material(), std::logic_error);

    // PEM strings: reload is a successful no-op, but there is no file to watch.
    kythira::grpc_server_config pem_cfg;
    pem_cfg.enable_tls = true;
    pem_cfg.server_cert_pem = leaf.certificate_pem;
    pem_cfg.server_key_pem = leaf.private_key_pem;
    server_t pem_server("127.0.0.1", 0, pem_cfg, recording_metrics{}, exec);
    BOOST_CHECK_NO_THROW(pem_server.reload_tls_material());
    BOOST_CHECK_THROW(pem_server.enable_auto_reload(1s), std::logic_error);

    // Both a PEM field and its path field.
    auto both = pem_cfg;
    both.server_cert_path = files.cert();
    BOOST_CHECK_THROW(server_t("127.0.0.1", 0, both, recording_metrics{}, exec),
                      kythira::grpc_tls_configuration_error);

    // A source together with any other input.
    auto source = std::make_shared<kythira::static_tls_material_source>(
        kythira::tls_material{.certificate_chain_pem = leaf.certificate_pem,
                              .private_key_pem = leaf.private_key_pem,
                              .root_certificates_pem = {}});
    auto source_and_pem = pem_cfg;
    source_and_pem.material_source = source;
    BOOST_CHECK_THROW(server_t("127.0.0.1", 0, source_and_pem, recording_metrics{}, exec),
                      kythira::grpc_tls_configuration_error);

    // A source that has not published yet (an issuing source whose provider
    // is down at startup).
    struct down_provider {
        auto root_certificate_pem() -> kythira::future_default<std::string> {
            throw std::runtime_error("down");
        }
        auto sign_csr(std::string, raft::testing::csr_signing_options)
            -> kythira::future_default<raft::testing::pem_material> {
            throw std::runtime_error("down");
        }
    };
    kythira::issuing_tls_material_options opts;
    opts.subject.common_name = "node-1";
    auto empty_source = std::make_shared<kythira::issuing_tls_material_source<down_provider>>(
        std::make_shared<down_provider>(), opts);
    BOOST_REQUIRE(empty_source->generation() == 0U);
    kythira::grpc_server_config gen0;
    gen0.enable_tls = true;
    gen0.material_source = empty_source;
    BOOST_CHECK_THROW(server_t("127.0.0.1", 0, gen0, recording_metrics{}, exec),
                      kythira::grpc_tls_configuration_error);

    // A file-backed server whose files are unreadable fails closed.
    auto missing = file_server_config(files, false);
    missing.server_key_path = (files.dir / "absent.pem").string();
    BOOST_CHECK_THROW(server_t("127.0.0.1", 0, missing, recording_metrics{}, exec),
                      kythira::grpc_tls_configuration_error);
}
