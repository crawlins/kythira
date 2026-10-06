// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: grpc-tls-reload
//
// Tests for grpc_detail::grpc_tls_bridge (.kiro/specs/grpc-tls-reload/,
// Task 3) against a real grpc::Server. What the server presents is read with
// a raw OpenSSL handshake on its port, comparing leaf serial numbers, so the
// checks see what a peer sees rather than what the bridge says it did:
//
// - new handshakes see applied material within the refresh bound (Property 2)
// - invalid material is refused and nothing changes (Property 3)
// - rapid reloads never pair a certificate with the wrong key (Property 4)
// - root rotation changes which client certificates are accepted
// - the staging directory is private and cleaned up

#define BOOST_TEST_MODULE GrpcTlsBridgeTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/grpc_tls_bridge.hpp>

#include "tls_handshake_probe.hpp"

#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/grpcpp.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kythira::grpc_detail::grpc_tls_bridge;
using kythira::testing::handshake;
using kythira::testing::serial_of;

auto issue(raft::testing::certificate_authority& ca, const std::string& cn, bool server)
    -> raft::testing::pem_material {
    raft::testing::leaf_certificate_options opts;
    opts.subject.common_name = cn;
    opts.dns_names = {cn, "localhost"};
    opts.ip_addresses = {"127.0.0.1"};
    opts.server_auth = server;
    opts.client_auth = !server;
    return ca.issue(opts);
}

auto material(const raft::testing::pem_material& leaf, const std::string& roots)
    -> kythira::tls_material {
    return {.certificate_chain_pem = leaf.certificate_pem,
            .private_key_pem = leaf.private_key_pem,
            .root_certificates_pem = roots};
}

// A server with no real service: gRPC needs one registered to start, and the
// default callback generic service answers UNIMPLEMENTED to everything.
struct running_server {
    grpc::CallbackGenericService generic;
    std::unique_ptr<grpc::Server> server;
    std::uint16_t port{0};
    explicit running_server(const std::shared_ptr<grpc::ServerCredentials>& creds) {
        grpc::ServerBuilder builder;
        builder.RegisterCallbackGenericService(&generic);
        int selected = 0;
        builder.AddListeningPort("127.0.0.1:0", creds, &selected);
        server = builder.BuildAndStart();
        BOOST_REQUIRE(server);
        BOOST_REQUIRE(selected != 0);
        port = static_cast<std::uint16_t>(selected);
    }
    ~running_server() { server->Shutdown(std::chrono::system_clock::now() + 1s); }
};

template<typename Pred>
auto eventually(Pred pred, std::chrono::milliseconds deadline = 5s) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return pred();
}

}  // namespace

// Property 2: after apply(), new handshakes present the new leaf within one
// refresh interval (plus slack for a loaded runner).
BOOST_AUTO_TEST_CASE(applied_identity_reaches_new_handshakes) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a", true);
    auto b = issue(ca, "node-b", true);
    grpc_tls_bridge bridge(grpc_tls_bridge::role::server, false, 1s, material(a, ""));
    running_server srv(bridge.server_credentials());

    auto first = handshake(srv.port);
    BOOST_REQUIRE(first.handshake_ok);
    BOOST_TEST(first.accepted);
    BOOST_TEST(first.serial == serial_of(a.certificate_pem));

    BOOST_TEST(bridge.apply(material(b, "")));
    BOOST_TEST(
        eventually([&] { return handshake(srv.port).serial == serial_of(b.certificate_pem); }));

    // Applying the same material again changes nothing.
    BOOST_TEST(!bridge.apply(material(b, "")));
}

// Property 3: invalid material throws and the server keeps presenting what
// it had, across several watcher passes.
BOOST_AUTO_TEST_CASE(invalid_material_is_refused_and_nothing_changes) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a", true);
    auto b = issue(ca, "node-b", true);
    grpc_tls_bridge bridge(grpc_tls_bridge::role::server, false, 1s, material(a, ""));
    running_server srv(bridge.server_credentials());

    auto mismatched = material(b, "");
    mismatched.private_key_pem = a.private_key_pem;
    BOOST_CHECK_THROW(bridge.apply(mismatched), kythira::grpc_tls_configuration_error);
    // Removing the identity would change what gRPC watches: refused too.
    BOOST_CHECK_THROW(bridge.apply(kythira::tls_material{}), kythira::grpc_tls_configuration_error);

    std::this_thread::sleep_for(2500ms);
    auto r = handshake(srv.port);
    BOOST_REQUIRE(r.handshake_ok);
    BOOST_TEST(r.serial == serial_of(a.certificate_pem));
}

// Root rotation on an mTLS server: once the trust bundle moves from CA-1 to
// CA-2, a CA-1 client is refused on new connections and a CA-2 client is
// accepted.
BOOST_AUTO_TEST_CASE(root_rotation_changes_which_clients_are_accepted) {
    raft::testing::certificate_authority ca1;
    raft::testing::certificate_authority ca2;
    auto server1 = issue(ca1, "node-a", true);
    auto server2 = issue(ca2, "node-a", true);
    auto client1 = issue(ca1, "client", false);
    auto client2 = issue(ca2, "client", false);

    grpc_tls_bridge bridge(grpc_tls_bridge::role::server, true, 1s,
                           material(server1, ca1.root_certificate_pem()));
    running_server srv(bridge.server_credentials());

    BOOST_TEST(handshake(srv.port, &client1).accepted);
    BOOST_TEST(!handshake(srv.port, &client2).accepted);
    BOOST_TEST(!handshake(srv.port).accepted);  // A certificate is required.

    bridge.apply(material(server2, ca2.root_certificate_pem()));
    BOOST_TEST(eventually([&] {
        return handshake(srv.port, &client2).accepted && !handshake(srv.port, &client1).accepted;
    }));

    // A server requiring client certificates cannot drop its roots.
    BOOST_CHECK_THROW(bridge.apply(material(server2, "")), kythira::grpc_tls_configuration_error);
}

// Property 4: 200 back-to-back reloads alternating two identities while 8
// threads open fresh connections. Every handshake completes -- a certificate
// served with the other identity's key would fail it -- and every serial seen
// is one of the two. gRPC's watcher re-reads only once a second, so this
// exercises the handshake side of a swap far more than the watcher's read
// window; staging_directory_is_private_pruned_and_removed pins the mtime
// invariant that protects that window.
BOOST_AUTO_TEST_CASE(rapid_reloads_never_tear_identity) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a", true);
    auto b = issue(ca, "node-b", true);
    grpc_tls_bridge bridge(grpc_tls_bridge::role::server, false, 1s, material(a, ""));
    running_server srv(bridge.server_credentials());

    const std::set<std::string> expected{serial_of(a.certificate_pem),
                                         serial_of(b.certificate_pem)};
    std::atomic<bool> done{false};
    std::atomic<int> handshakes{0};
    std::atomic<int> failures{0};
    std::atomic<int> unexpected{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 8; ++t) {
        clients.emplace_back([&] {
            while (!done.load()) {
                auto r = handshake(srv.port);
                ++handshakes;
                if (!r.handshake_ok || !r.accepted) {
                    ++failures;
                } else if (!expected.contains(r.serial)) {
                    ++unexpected;
                }
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        bridge.apply(material(i % 2 == 0 ? b : a, ""));
        std::this_thread::sleep_for(10ms);
    }
    // Let the watcher make a few more passes over the final generation.
    std::this_thread::sleep_for(2s);
    done = true;
    for (auto& c : clients) {
        c.join();
    }

    BOOST_TEST_MESSAGE("handshakes: " << handshakes.load());
    BOOST_TEST(handshakes.load() > 0);
    BOOST_TEST(failures.load() == 0);
    BOOST_TEST(unexpected.load() == 0);
}

// The key only ever sits in a private directory, generations are pruned, and
// the directory is gone after destruction (Requirement 6.5).
BOOST_AUTO_TEST_CASE(staging_directory_is_private_pruned_and_removed) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a", true);
    auto b = issue(ca, "node-b", true);
    std::filesystem::path staging;
    {
        grpc_tls_bridge bridge(grpc_tls_bridge::role::server, false, 1s, material(a, ""));
        staging = bridge.staging_directory();
        struct stat st{};
        BOOST_REQUIRE(::stat(staging.c_str(), &st) == 0);
        BOOST_TEST((st.st_mode & 0777) == 0700U);
        BOOST_REQUIRE(::stat((staging / "current" / "key.pem").c_str(), &st) == 0);
        BOOST_TEST((st.st_mode & 0777) == 0600U);

        for (int i = 0; i < 5; ++i) {
            bridge.apply(material(i % 2 == 0 ? b : a, ""));
        }
        std::set<std::string> gens;
        for (const auto& e : std::filesystem::directory_iterator(staging)) {
            auto name = e.path().filename().string();
            if (name.starts_with("gen-")) {
                gens.insert(name);
            }
        }
        BOOST_TEST((gens == std::set<std::string>{"gen-5", "gen-6"}));
        BOOST_TEST(std::filesystem::read_symlink(staging / "current") == "gen-6");

        // gRPC's watcher spots a key and chain from different generations
        // only by comparing whole-second mtimes, so back-to-back generations
        // must never share a second. (The stress test above rarely lands a
        // watcher pass inside the window this protects, so pin it directly.)
        auto mtime_sec = [&](const char* gen, const char* file) {
            struct stat fst{};
            BOOST_REQUIRE(::stat((staging / gen / file).c_str(), &fst) == 0);
            return fst.st_mtim.tv_sec;
        };
        BOOST_TEST(mtime_sec("gen-6", "key.pem") > mtime_sec("gen-5", "key.pem"));
        BOOST_TEST(mtime_sec("gen-6", "chain.pem") > mtime_sec("gen-5", "chain.pem"));
        BOOST_TEST(mtime_sec("gen-6", "key.pem") == mtime_sec("gen-6", "chain.pem"));
    }
    BOOST_TEST(!std::filesystem::exists(staging));
}

BOOST_AUTO_TEST_CASE(construction_refuses_bad_material_and_intervals) {
    raft::testing::certificate_authority ca;
    auto a = issue(ca, "node-a", true);
    auto b = issue(ca, "node-b", true);
    using role = grpc_tls_bridge::role;
    BOOST_CHECK_THROW(grpc_tls_bridge(role::server, false, 0s, material(a, "")),
                      kythira::grpc_tls_configuration_error);
    BOOST_CHECK_THROW(grpc_tls_bridge(role::server, false, 1s, kythira::tls_material{}),
                      kythira::grpc_tls_configuration_error);
    BOOST_CHECK_THROW(grpc_tls_bridge(role::server, true, 1s, material(a, "")),
                      kythira::grpc_tls_configuration_error);
    auto mismatched = material(a, "");
    mismatched.private_key_pem = b.private_key_pem;
    BOOST_CHECK_THROW(grpc_tls_bridge(role::server, false, 1s, mismatched),
                      kythira::grpc_tls_configuration_error);
    // A client with nothing to watch has no use for a bridge.
    BOOST_CHECK_THROW(grpc_tls_bridge(role::client, false, 1s, kythira::tls_material{}),
                      kythira::grpc_tls_configuration_error);
    // A roots-only client is fine.
    kythira::tls_material roots_only;
    roots_only.root_certificates_pem = ca.root_certificate_pem();
    BOOST_CHECK_NO_THROW(grpc_tls_bridge(role::client, false, 1s, roots_only));
}
