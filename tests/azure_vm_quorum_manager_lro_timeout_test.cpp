// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// Regression test for the per-request bound inside
/// `azure_vm_quorum_manager::poll_lro`.
///
/// `poll_lro` tests its own deadline only between iterations, so an
/// `Azure::Core::Context` with no deadline let a single ARM poll that never
/// answered block forever — the loop's `timeout` was unreachable and the call
/// hung. That is how a stalled ARM call outlives the whole test binary's budget
/// and gets it SIGKILLed by ctest, which runs no destructor, stranding every VM
/// the in-flight case created. Three of them survived that way on run
/// 36426191450 and billed 25.5 hours on demand.
///
/// Driven through `decommission_node`, the only caller of `poll_lro`.
///
/// The test double speaks HTTPS, not HTTP: the managers authenticate with the
/// SDK's `BearerTokenAuthenticationPolicy`, which refuses a non-HTTPS endpoint
/// outright, so the plain-HTTP mock idiom `azure_blob_client`'s tests use cannot
/// work here. `azure_client_config::transport` supplies a curl transport whose
/// `CAInfo` trusts the test CA, which keeps peer verification and the real
/// bearer-token path in play rather than disabling either — a test that passed
/// against a connection production would refuse would be worth little.
///
/// Its own test target rather than a case inside `azure_quorum_manager_unit_test`
/// because it needs cpp-httplib's TLS server and the certificate_authority
/// fixture, neither of which that target links.

#define BOOST_TEST_MODULE azure_vm_quorum_manager_lro_timeout_test
#include <boost/test/unit_test.hpp>

#include <raft/azure_vm_quorum_manager.hpp>

#include <azure/core/http/curl_transport.hpp>

#include <httplib.h>

#include "ca_test_fixture.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

using namespace std::chrono_literals;

namespace {

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

/// Hands out a token without contacting anything, so the bearer policy is
/// satisfied without an Azure identity. Same approach as
/// `azure_key_vault_ca_provider_unit_test.cpp`'s.
class FakeTokenCredential : public Azure::Core::Credentials::TokenCredential {
public:
    FakeTokenCredential() : TokenCredential("FakeTokenCredential") {}

    [[nodiscard]] auto GetToken(const Azure::Core::Credentials::TokenRequestContext& /*request*/,
                                const Azure::Core::Context& /*context*/) const
        -> Azure::Core::Credentials::AccessToken override {
        Azure::Core::Credentials::AccessToken token;
        token.Token = "fake-token";
        token.ExpiresOn = Azure::DateTime(std::chrono::system_clock::now() + 1h);
        return token;
    }
};

/// ARM test double over TLS. The VM delete returns 202 with an
/// `Azure-AsyncOperation` header pointing back here; the *first* poll of that
/// operation never answers, and every later one reports `Succeeded`.
///
/// Stalling only the first poll is what makes this discriminate. A merely slow
/// poll would pass either way; one that never answers hangs the call outright
/// unless the request itself is bounded, and the retry then has to succeed for
/// the call to finish at all.
class ArmMock {
public:
    ArmMock(const std::string& cert_path, const std::string& key_path)
        : _server(cert_path.c_str(), key_path.c_str()) {
        _server.Delete(R"(/subscriptions/.+/virtualMachines/.+)",
                       [this](const httplib::Request& /*req*/, httplib::Response& res) {
                           res.status = 202;
                           res.set_header("Azure-AsyncOperation", origin() + "/lro");
                           res.set_content("{}", "application/json");
                       });

        // decommission_node deletes the NIC after the VM and ignores failures,
        // but answering keeps the log free of noise unrelated to what is under
        // test.
        _server.Delete(R"(/subscriptions/.+/networkInterfaces/.+)",
                       [](const httplib::Request& /*req*/, httplib::Response& res) {
                           res.status = 200;
                           res.set_content("{}", "application/json");
                       });

        _server.Get("/lro", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            if (_lro_polls.fetch_add(1) == 0) {
                // Never answer. The wait is on a condition variable purely so
                // teardown can release it instead of leaving a sleeping thread
                // behind; nothing signals it before then, so from the client's
                // side this request simply never completes.
                std::unique_lock lock(_mu);
                _cv.wait(lock, [this] { return _stopping; });
                res.status = 500;
                res.set_content("{}", "application/json");
                return;
            }
            res.status = 200;
            res.set_content(R"({"status":"Succeeded"})", "application/json");
        });

        _port = _server.bind_to_any_port("127.0.0.1");
        _thread = std::thread([this] { _server.listen_after_bind(); });
        while (!_server.is_running()) {
            std::this_thread::sleep_for(5ms);
        }
    }

    ArmMock(const ArmMock&) = delete;
    auto operator=(const ArmMock&) -> ArmMock& = delete;
    ArmMock(ArmMock&&) = delete;
    auto operator=(ArmMock&&) -> ArmMock& = delete;

    ~ArmMock() {
        {
            std::lock_guard lock(_mu);
            _stopping = true;
        }
        _cv.notify_all();
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    /// `localhost`, which the certificate carries as a SAN, so curl's hostname
    /// check has a name to match. The server also binds 127.0.0.1 and the
    /// certificate carries that as an IP SAN.
    [[nodiscard]] auto origin() const -> std::string {
        return "https://localhost:" + std::to_string(_port);
    }

    [[nodiscard]] auto lro_polls() const -> int { return _lro_polls.load(); }

    /// False when the certificate or key could not be loaded, in which case the
    /// server never serves and every request fails for a reason that has nothing
    /// to do with what is under test.
    [[nodiscard]] auto valid() const -> bool { return _server.is_valid(); }

private:
    httplib::SSLServer _server;
    std::thread _thread;
    int _port{};
    std::atomic<int> _lro_polls{0};
    std::mutex _mu;
    std::condition_variable _cv;
    bool _stopping{false};
};

auto make_config(const std::string& endpoint,
                 std::shared_ptr<Azure::Core::Http::HttpTransport> transport)
    -> kythira::azure_vm_quorum_manager_config {
    kythira::azure_vm_quorum_manager_config cfg;
    cfg.cluster_name = "test-cluster";
    cfg.azure.subscription_id = "00000000-0000-0000-0000-000000000000";
    cfg.azure.resource_group = "test-rg";
    cfg.azure.location = "eastus";
    cfg.azure.arm_endpoint_override = endpoint;
    // The bound the stalled poll has to hit. Small enough to keep the test
    // quick, but comfortably above the TLS handshake with the local double --
    // at 2s the very first ARM call was itself cancelled mid-handshake often
    // enough to make this flaky, since api_timeout covers the whole request.
    cfg.azure.api_timeout = 2s;
    cfg.azure.credential = std::make_shared<FakeTokenCredential>();
    cfg.azure.transport = std::move(transport);
    cfg.node_port = 7000;
    cfg.image_reference.publisher = "Canonical";
    cfg.image_reference.offer = "0001-com-ubuntu-server-jammy";
    cfg.image_reference.sku = "22_04-lts";
    cfg.image_reference.version = "latest";
    cfg.topology.groups.push_back({.group_id = "1", .target_count = 1});
    cfg.subnet_id_by_group["1"] =
        "/subscriptions/00000000-0000-0000-0000-000000000000/resourceGroups/test-rg/providers/"
        "Microsoft.Network/virtualNetworks/test-vnet/subnets/test-subnet";
    return cfg;
}

}  // namespace

// The case timeout is the backstop that turns a regression into a failure rather
// than a hung suite: without the per-request bound, decommission_node never
// returns and Boost ends the case here instead of ctest SIGKILLing the binary.
BOOST_AUTO_TEST_CASE(stalled_lro_poll_is_abandoned_and_retried, *boost::unit_test::timeout(120)) {
    raft::testing::ca_test_fixture ca;
    // A cert valid for both the name curl checks and the address it connects to.
    const auto& server_files = ca.bootstrap_client("localhost", {"localhost"}, {"127.0.0.1"});

    ArmMock mock(server_files.cert_path(), server_files.key_path());
    BOOST_REQUIRE_MESSAGE(mock.valid(), "the TLS test double failed to load its cert/key");

    // Verification stays on, against the test CA: what made a plain-HTTP mock
    // impossible is BearerTokenAuthenticationPolicy's https requirement, and
    // honouring it properly keeps the real auth and TLS path in play.
    raft::testing::temp_cert_files ca_root(
        raft::testing::pem_material{.certificate_pem = ca.root_certificate_pem()});
    Azure::Core::Http::CurlTransportOptions curl_options;
    curl_options.CAInfo = ca_root.cert_path();
    auto transport = std::make_shared<Azure::Core::Http::CurlTransport>(curl_options);

    kythira::azure_vm_quorum_manager<> mgr{make_config(mock.origin(), transport)};

    const auto start = std::chrono::steady_clock::now();
    try {
        std::move(mgr.decommission_node(1)).get();
    } catch (const std::exception& ex) {
        // Named rather than swallowed: "unexpected exception" on its own tells
        // the next reader nothing about which ARM call refused. The elapsed time
        // and poll count say whether it was the stalled poll or a call before it.
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        BOOST_FAIL(std::string("decommission_node threw after ") + std::to_string(ms) +
                   "ms (lro polls seen: " + std::to_string(mock.lro_polls()) + "): " + ex.what());
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    // Returning at all is the regression: the first poll never answers, so an
    // unbounded request meant poll_lro never came back.
    //
    // More than one poll proves the stalled one was abandoned rather than waited
    // on, and that the loop then recovered instead of giving up.
    BOOST_TEST(mock.lro_polls() >= 2);

    // It cost at least the one api_timeout the abandoned poll was allowed ...
    BOOST_TEST(elapsed_ms >= 2000);

    // ... and nothing like poll_lro's own 30s loop budget, which is what it would
    // spend if every poll were abandoned instead of the retry answering.
    BOOST_TEST(elapsed_ms < 25000);
}
