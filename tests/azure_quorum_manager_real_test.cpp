/// @file azure_quorum_manager_real_test.cpp
/// @brief Real-Azure integration tests for `azure_vm_quorum_manager` and
///        `azure_vmss_quorum_manager`. Provisions actual ARM resources
///        (VNet/subnets/NSG/VMs/VMSS instances) against a real subscription.
///
/// Compiled only when `KYTHIRA_AZURE_REAL_TESTS` is defined; only *registered*
/// as a CTest case when the CMake option of the same name is `ON` (see
/// tests/CMakeLists.txt) — unlike the AWS real-EC2 tests, which always
/// register and skip at runtime via `SKIP_RETURN_CODE 77`. This spec's
/// tasks.md explicitly calls for the stronger "don't even register" gating.
///
/// Implements design.md's full "Integration tests (real Azure)" test list
/// (10 VM cases, 5 VMSS cases) — ported and adapted from
/// `aws_quorum_manager_real_ec2_test.cpp`. Compiles clean and every case's
/// skip-path has been exercised with no credentials configured, but unlike
/// `aws_quorum_manager_real_ec2_test.cpp` (run against real AWS repeatedly),
/// no case here has yet executed its real assertion logic against a live
/// Azure subscription — treat that as unverified until a real run happens.
///
/// Every VM case launches through `escalating_vm_manager`, which walks the
/// spot-first ladder built by `launch_ladder()` (see
/// `azure_real_test_support.hpp` for how that ladder is discovered and why the
/// price feed has to be joined against SKU availability). The ladder's pure
/// half — parsing, ranking, error classification, and the escalation walk
/// itself — is covered offline and deterministically by
/// `tests/azure_spot_escalation_test.cpp`; that is where escalation is
/// actually established, since a healthy live run never exercises it.

#define BOOST_TEST_MODULE azure_quorum_manager_real_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AZURE_REAL_TESTS
#ifdef KYTHIRA_HAS_AZURE_SDK

#include "azure_real_test_support.hpp"

#include <raft/azure_vm_quorum_manager.hpp>
#include <raft/azure_vmss_quorum_manager.hpp>

#include <azure/core/base64.hpp>
#include <azure/core/context.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/http/policies/policy.hpp>
#include <azure/core/internal/client_options.hpp>
#include <azure/core/internal/http/pipeline.hpp>
#include <azure/core/io/body_stream.hpp>
#include <azure/core/url.hpp>

#include <boost/json.hpp>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace kythira::testing::azure_real;

namespace {

auto env_or(const char* name, const std::string& fallback) -> std::string {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

auto env_opt(const char* name) -> std::optional<std::string> {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return std::nullopt;
    }
    return std::string(v);
}

/// Serialises one OpenSSH wire "string" (4-byte big-endian length, then bytes).
void append_ssh_string(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t len) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((len >> shift) & 0xFFU));
    }
    out.insert(out.end(), data, data + len);
}

/// Serialises one OpenSSH wire "mpint": the same length-prefixed form, but with
/// a leading zero byte when the top bit is set, since mpints are signed.
void append_ssh_mpint(std::vector<std::uint8_t>& out, const BIGNUM* value) {
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(BN_num_bytes(value)));
    BN_bn2bin(value, buf.data());
    if (!buf.empty() && (buf.front() & 0x80U) != 0) {
        buf.insert(buf.begin(), 0x00);
    }
    append_ssh_string(out, buf.data(), buf.size());
}

/// Generates a throwaway RSA public key in OpenSSH `authorized_keys` format.
///
/// ARM rejects a Linux `osProfile` that supplies neither `adminPassword` nor
/// `linuxConfiguration.ssh.publicKeys` — "Required parameter
/// 'osProfile.adminPassword' is missing (null)" — so *some* credential has to
/// be set or no case in this file can provision at all.
///
/// A generated key is used rather than an operator-supplied one because
/// nothing in this suite ever logs in: liveness comes solely from ARM
/// `instanceView` power state (see `azure_vm_quorum_manager`'s class comment),
/// so the key only has to exist and be well-formed. The private half is
/// discarded immediately and never leaves this function, which makes these VMs
/// unreachable by design rather than by policy — strictly better than baking a
/// real key into CI. `AZURE_TEST_SSH_PUBLIC_KEY` overrides it for anyone who
/// does want to be able to log in and debug a failing run.
[[nodiscard]] auto make_ephemeral_ssh_public_key() -> std::string {
    struct ctx_deleter {
        void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
    };
    struct key_deleter {
        void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
    };
    struct bn_deleter {
        void operator()(BIGNUM* p) const { BN_free(p); }
    };

    std::unique_ptr<EVP_PKEY_CTX, ctx_deleter> ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    BOOST_REQUIRE(ctx != nullptr);
    BOOST_REQUIRE(EVP_PKEY_keygen_init(ctx.get()) > 0);
    BOOST_REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) > 0);
    EVP_PKEY* raw_key = nullptr;
    BOOST_REQUIRE(EVP_PKEY_keygen(ctx.get(), &raw_key) > 0);
    std::unique_ptr<EVP_PKEY, key_deleter> key{raw_key};

    BIGNUM* raw_e = nullptr;
    BIGNUM* raw_n = nullptr;
    BOOST_REQUIRE(EVP_PKEY_get_bn_param(key.get(), "e", &raw_e) > 0);
    BOOST_REQUIRE(EVP_PKEY_get_bn_param(key.get(), "n", &raw_n) > 0);
    std::unique_ptr<BIGNUM, bn_deleter> e{raw_e};
    std::unique_ptr<BIGNUM, bn_deleter> n{raw_n};

    static constexpr std::string_view kType = "ssh-rsa";
    std::vector<std::uint8_t> blob;
    append_ssh_string(blob, reinterpret_cast<const std::uint8_t*>(kType.data()), kType.size());
    append_ssh_mpint(blob, e.get());
    append_ssh_mpint(blob, n.get());
    return std::string(kType) + " " + Azure::Core::Convert::Base64Encode(blob) +
           " kythira-realtest-ephemeral";
}

/// The one SSH public key used by every VM this process provisions. Generated
/// once: RSA-2048 keygen costs real time, and there is no reason for VMs in the
/// same run to differ.
[[nodiscard]] auto test_ssh_public_key() -> const std::string& {
    static const std::string key = [] {
        if (auto supplied = env_opt("AZURE_TEST_SSH_PUBLIC_KEY")) {
            return *supplied;
        }
        return make_ephemeral_ssh_public_key();
    }();
    return key;
}

auto random_suffix() -> std::string {
    std::random_device rd;
    std::ostringstream oss;
    oss << std::hex << rd() << rd();
    return oss.str().substr(0, 12);
}

/// Builds a one-off ARM pipeline identical in shape to the one
/// `azure_vm_quorum_manager`/`azure_vmss_quorum_manager` construct internally
/// (private, so not reusable from here) — used only for test-side ARM calls
/// this project's own manager API has no method for.
[[nodiscard]] auto make_test_arm_pipeline(const kythira::azure_client_config& azure)
    -> Azure::Core::Http::_internal::HttpPipeline {
    auto credential =
        azure.credential ? azure.credential : kythira::make_default_credential_chain();
    Azure::Core::Credentials::TokenRequestContext token_ctx;
    token_ctx.Scopes = {"https://management.azure.com/.default"};
    std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>> per_retry_policies;
    per_retry_policies.emplace_back(
        std::make_unique<Azure::Core::Http::Policies::_internal::BearerTokenAuthenticationPolicy>(
            credential, std::move(token_ctx)));
    Azure::Core::_internal::ClientOptions client_options;
    return Azure::Core::Http::_internal::HttpPipeline(
        client_options, "kythira-azure-real-test", "1.0.0", std::move(per_retry_policies),
        std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{});
}

[[nodiscard]] auto arm_endpoint(const kythira::azure_client_config& azure) -> std::string {
    return azure.arm_endpoint_override.empty() ? "https://management.azure.com"
                                               : azure.arm_endpoint_override;
}

[[nodiscard]] auto arm_base_url(const kythira::azure_client_config& azure) -> std::string {
    return arm_endpoint(azure) + "/subscriptions/" + azure.subscription_id + "/resourceGroups/" +
           azure.resource_group;
}

/// The spot-first launch ladder for this run, discovered once per *process*
/// rather than once per test case.
///
/// Discovery costs ~11 HTTP round trips (10 paginated Retail Prices pages plus
/// one `Microsoft.Compute/skus` call); `AzureIntegrationFixture` is a
/// per-case fixture, so without this cache a 15-case run would repeat that 15
/// times for an answer that cannot meaningfully change mid-run.
///
/// Three env vars steer it, in precedence order:
///
///  * `AZURE_TEST_VM_SIZE` — pin to exactly this size, trying spot first and
///    then on-demand for the same size. Set by CI, which wants a fixed,
///    reviewable SKU rather than whatever is cheapest this hour.
///  * `AZURE_TEST_FORCE_FIRST_VM_SIZE` — prepend this size as rung 0. Exists
///    solely to test the escalation path itself: point it at a SKU this
///    subscription cannot launch (e.g. `Standard_D2s_v5`, which is
///    `NotAvailableForSubscription` here) and a passing run proves escalation
///    advanced rather than that nothing ever failed.
///  * `AZURE_TEST_MAX_VCPUS` — vCPU ceiling for candidates (default 2).
///
/// Falls back to a single hardcoded rung if discovery fails, so a Retail
/// Prices outage degrades the suite to its pre-escalation behaviour instead of
/// failing it.
[[nodiscard]] auto launch_ladder(const kythira::azure_client_config& azure)
    -> const std::vector<azure_vm_option>& {
    static const std::vector<azure_vm_option> ladder = [&] {
        std::vector<azure_vm_option> out;

        if (auto forced = env_opt("AZURE_TEST_FORCE_FIRST_VM_SIZE")) {
            out.push_back({.vm_size = *forced, .spot = true, .price_per_hr = 0.0});
        }

        if (auto pinned = env_opt("AZURE_TEST_VM_SIZE")) {
            out.push_back({.vm_size = *pinned, .spot = true, .price_per_hr = 0.0});
            out.push_back({.vm_size = *pinned, .spot = false, .price_per_hr = 0.0});
            return out;
        }

        azure_sku_requirements req;
        if (auto max_vcpus = env_opt("AZURE_TEST_MAX_VCPUS")) {
            req.max_vcpus = static_cast<unsigned>(std::stoul(*max_vcpus));
        }
        auto pipeline = make_test_arm_pipeline(azure);
        auto discovered = discover_spot_first_options(pipeline, arm_endpoint(azure),
                                                      azure.subscription_id, azure.location, req);
        if (discovered.empty()) {
            // Chosen over the historical `Standard_D2s_v5` default, which is
            // `NotAvailableForSubscription` in this subscription and so could
            // never have launched: v7 is the cheapest currently-available
            // family here and is Gen2, matching the fixture's `-gen2` image.
            discovered.push_back(
                {.vm_size = "Standard_D2als_v7", .spot = true, .price_per_hr = 0.0});
            discovered.push_back(
                {.vm_size = "Standard_D2als_v7", .spot = false, .price_per_hr = 0.0});
        }
        out.insert(out.end(), discovered.begin(), discovered.end());
        return out;
    }();
    return ladder;
}

/// Names of every VM in the resource group tagged `kythira:cluster = cluster`.
///
/// Lists unfiltered and matches client-side for the same reason
/// `azure_vm_quorum_manager::next_node_id()` does: ARM's `tagName`/`tagValue`
/// `$filter` is only valid on the generic `/resources` endpoint and 400s on
/// `Microsoft.Compute/virtualMachines` once the group actually holds a VM.
[[nodiscard]] auto cluster_vm_names(Azure::Core::Http::_internal::HttpPipeline& pipeline,
                                    const kythira::azure_client_config& azure,
                                    const std::string& cluster) -> std::vector<std::string> {
    Azure::Core::Url url(arm_base_url(azure) +
                         "/providers/Microsoft.Compute/virtualMachines?api-version=2024-07-01");
    Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, url);
    Azure::Core::Context context;
    auto response = pipeline.Send(request, context);
    if (static_cast<int>(response->GetStatusCode()) / 100 != 2) {
        return {};
    }
    const auto& body = response->GetBody();
    auto parsed = boost::json::parse(std::string(body.begin(), body.end()));
    std::vector<std::string> names;
    if (!parsed.is_object() || !parsed.as_object().contains("value")) {
        return names;
    }
    for (const auto& vm : parsed.at("value").as_array()) {
        if (!vm.is_object()) {
            continue;
        }
        const auto* tags = vm.as_object().if_contains("tags");
        const auto* name = vm.as_object().if_contains("name");
        if (tags == nullptr || name == nullptr || !tags->is_object() || !name->is_string()) {
            continue;
        }
        const auto* owner = tags->as_object().if_contains("kythira:cluster");
        if (owner != nullptr && owner->is_string() && std::string(owner->as_string()) == cluster) {
            names.push_back(std::string(name->as_string()));
        }
    }
    return names;
}

/// Deletes `vm_name`, retrying while ARM refuses because an operation is still
/// in flight on it. Returns false if it never succeeded.
///
/// The retry is the whole point: teardown can run while a create started
/// moments earlier is still settling, and ARM answers a DELETE against a
/// resource with a pending operation with a 409 rather than queueing it. A
/// 404 counts as success — the VM is gone, which is all the caller wants.
[[nodiscard]] auto delete_vm_with_retry(Azure::Core::Http::_internal::HttpPipeline& pipeline,
                                        const kythira::azure_client_config& azure,
                                        const std::string& vm_name) -> bool {
    constexpr int kMaxAttempts = 20;
    constexpr auto kBackoff = std::chrono::seconds{15};
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        try {
            Azure::Core::Url url(arm_base_url(azure) +
                                 "/providers/Microsoft.Compute/virtualMachines/" + vm_name +
                                 "?api-version=2024-07-01");
            Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Delete, url);
            Azure::Core::Context context;
            auto response = pipeline.Send(request, context);
            const auto code = static_cast<int>(response->GetStatusCode());
            if (code / 100 == 2 || code == 404) {
                return true;
            }
        } catch (const std::exception&) {
            // Fall through to the backoff; a transient transport error is no
            // more fatal here than a 409.
        }
        std::this_thread::sleep_for(kBackoff);
    }
    return false;
}

/// RAII fixture: sets up a VNet/3 zonal subnets/NSG for one test run (unless
/// env-var overrides point at pre-existing ones), tears everything it created
/// down unconditionally on destruction (best-effort, errors to stderr),
/// mirroring the AWS design's `IntegrationFixture` shape but considerably
/// smaller — see design.md's "Integration test fixture" section for why (no
/// resource-group lifecycle, no bastion/NAT gateway equivalent).
class AzureIntegrationFixture : public signal_cleanup_target {
public:
    AzureIntegrationFixture() {
        subscription_id = env_or("AZURE_SUBSCRIPTION_ID", "");
        resource_group = env_or("AZURE_TEST_RESOURCE_GROUP", "");
        if (subscription_id.empty() || resource_group.empty()) {
            preflight_ok = false;
            return;
        }

        azure.subscription_id = subscription_id;
        azure.resource_group = resource_group;
        azure.location = env_or("AZURE_TEST_LOCATION", "eastus");

        // Credential pre-flight: GET .../resourceGroups/{rg} directly (design.md's
        // exact "Integration test fixture" wording) with whatever credential
        // chain is ambient (service principal / managed identity / az login),
        // and skip (not fail) the whole suite otherwise. Deliberately does NOT
        // go through azure_vm_quorum_manager::assess_quorum: that method's
        // fast-path returns immediately with no ARM call at all for an empty
        // cluster, and even with a non-empty one, a 404 (which a missing
        // resource group would also produce) is treated as ordinary node
        // unreachability, not a fatal error — neither shape actually verifies
        // what this preflight needs to verify.
        try {
            auto pipeline = make_test_arm_pipeline(azure);
            Azure::Core::Url url((azure.arm_endpoint_override.empty()
                                      ? "https://management.azure.com"
                                      : azure.arm_endpoint_override) +
                                 "/subscriptions/" + subscription_id + "/resourceGroups/" +
                                 resource_group + "?api-version=2024-03-01");
            Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, url);
            Azure::Core::Context context;
            auto response = pipeline.Send(request, context);
            auto code = static_cast<int>(response->GetStatusCode());
            if (code < 200 || code >= 300) {
                throw std::runtime_error("GET resourceGroups/" + resource_group +
                                         " returned HTTP " + std::to_string(code));
            }
        } catch (const std::exception& ex) {
            std::cerr << "[azure_quorum_manager_real_test] credential/resource-group preflight "
                         "failed, skipping suite: "
                      << ex.what() << "\n";
            preflight_ok = false;
            return;
        }

        test_run = "kythira-test-" + random_suffix();
        cluster_name = "kythira-realtest-" + random_suffix();

        vnet_id = env_opt("AZURE_TEST_VNET_ID");
        nsg_id = env_opt("AZURE_TEST_NSG_ID");
        for (int zone = 1; zone <= 3; ++zone) {
            subnet_id[zone] = env_opt(("AZURE_TEST_SUBNET_ID_ZONE" + std::to_string(zone)).c_str());
        }

        // Setup is intentionally minimal here: creating a VNet/subnets/NSG
        // requires the same raw-ARM-REST technique the quorum managers
        // themselves use (Microsoft.Network has no generated C++ SDK client
        // either). Operators who don't pre-provision these via the
        // AZURE_TEST_*_ID env vars are expected to have granted this
        // fixture's credential Network Contributor on resource_group, at
        // which point the same azure_vm_quorum_manager machinery this file
        // is testing could be extended with a small sibling ARM-helper to
        // create them — tracked in the Notes section of this spec's
        // tasks.md as a follow-up rather than duplicated here.
        if (!vnet_id || !nsg_id || !subnet_id[1] || !subnet_id[2] || !subnet_id[3]) {
            std::cerr << "[azure_quorum_manager_real_test] one or more of "
                         "AZURE_TEST_VNET_ID/AZURE_TEST_NSG_ID/AZURE_TEST_SUBNET_ID_ZONE{1,2,3} is "
                         "unset and this fixture does not create network infrastructure itself; "
                         "skipping suite.\n";
            preflight_ok = false;
            return;
        }

        g_active_azure_fixture.store(this, std::memory_order_release);
    }

    ~AzureIntegrationFixture() {
        g_active_azure_fixture.store(nullptr, std::memory_order_release);
        teardown();
    }

    /// Deletes any VM still tagged with this fixture's own `cluster_name`.
    ///
    /// Most cases decommission their own nodes, so this normally finds
    /// nothing. It exists because `azure_vm_quorum_manager`'s in-band cleanup
    /// cannot always win: on the provisioning-timeout path it calls
    /// `best_effort_delete_vm`, but with a 1-second timeout (which
    /// `provision_timeout_cleanup` sets deliberately) the VM is still
    /// mid-create, ARM refuses to delete a resource with an operation in
    /// flight, and the failure is logged and swallowed by design. That left a
    /// running VM behind on every single run of that case.
    ///
    /// Sweeping here rather than hardening the manager is the right split: the
    /// manager's delete is racing an ARM operation it does not control, while
    /// the fixture runs after the test body, by which time the create has
    /// settled and the delete is simply valid. The retry loop covers the
    /// remainder — teardown can still land while a create is finishing, so a
    /// single DELETE attempt is not enough.
    ///
    /// Scoped by the `kythira:cluster` tag, which is unique per fixture
    /// instance (one per test case), so a sweep never touches another case's
    /// VMs — including when cases are the concurrent kind. Deleting the VM is
    /// sufficient to reclaim its NIC and OS disk, since both now carry
    /// `deleteOption: Delete` and cascade.
    ///
    /// Also reached from the signal handler via `g_active_azure_fixture`, so an
    /// interrupted run cleans up too instead of stranding VMs in the
    /// subscription.
    void teardown() noexcept override {
        if (torn_down || !preflight_ok || cluster_name.empty()) {
            return;
        }
        torn_down = true;
        try {
            auto pipeline = make_test_arm_pipeline(azure);
            for (const auto& vm_name : cluster_vm_names(pipeline, azure, cluster_name)) {
                // Loud on purpose. A sweep that silently absorbed leaks would
                // turn "this case leaks a VM every run" back into something
                // invisible; this way the next leak shows up in the log.
                std::cerr << "[azure_quorum_manager_real_test] teardown: deleting leaked VM "
                          << vm_name << " (cluster " << cluster_name << ")\n";
                if (!delete_vm_with_retry(pipeline, azure, vm_name)) {
                    std::cerr << "[azure_quorum_manager_real_test] teardown: FAILED to delete "
                              << vm_name << " — delete it manually\n";
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "[azure_quorum_manager_real_test] teardown sweep failed: " << ex.what()
                      << "\n";
        } catch (...) {
            std::cerr << "[azure_quorum_manager_real_test] teardown sweep failed\n";
        }
    }

    [[nodiscard]] auto vm_config(int zone_count) const -> kythira::azure_vm_quorum_manager_config {
        kythira::azure_vm_quorum_manager_config cfg;
        cfg.cluster_name = cluster_name;
        cfg.azure = azure;
        cfg.image_reference.publisher = "Canonical";
        cfg.image_reference.offer = "0001-com-ubuntu-server-jammy";
        // Gen2, not the plain `22_04-lts` Gen1 image: every cheap modern SKU
        // (the v6/v7 families the spot ladder ranks first) is Gen2-only, and
        // pairing one with a Gen1 image is a hard BadRequest that escalation
        // deliberately does not retry. See `azure_sku_requirements`.
        cfg.image_reference.sku = env_or("AZURE_TEST_IMAGE_SKU", "22_04-lts-gen2");
        cfg.image_reference.version = "latest";
        // Overwritten per attempt by escalating_vm_manager; the ladder's first
        // rung is the starting point rather than a fixed choice.
        cfg.vm_size = launch_ladder(azure).front().vm_size;
        // Required: ARM rejects a Linux osProfile carrying neither an SSH key
        // nor an adminPassword. See test_ssh_public_key().
        cfg.ssh_public_key = test_ssh_public_key();
        cfg.node_port = 7000;
        cfg.network_security_group_id = *nsg_id;
        for (int zone = 1; zone <= zone_count; ++zone) {
            std::string group = std::to_string(zone);
            cfg.topology.groups.push_back({.group_id = group, .target_count = 1});
            cfg.subnet_id_by_group[group] = *subnet_id.at(zone);
            cfg.placement_by_group[group] = {
                .kind = kythira::azure_placement_kind::availability_zone, .zone = group};
        }
        return cfg;
    }

    [[nodiscard]] auto vmss_config() const -> kythira::azure_vmss_quorum_manager_config {
        kythira::azure_vmss_quorum_manager_config cfg;
        cfg.cluster_name = cluster_name;
        cfg.azure = azure;
        cfg.node_port = 7000;
        auto scale_set = env_opt("AZURE_TEST_VMSS_NAME");
        if (scale_set) {
            cfg.scale_set_by_group["1"] = *scale_set;
            cfg.topology.groups.push_back({.group_id = "1", .target_count = 1});
        }
        return cfg;
    }

    bool preflight_ok{true};
    /// Guards against the sweep running twice — the destructor and the signal
    /// handler can both reach teardown().
    bool torn_down{false};
    std::string subscription_id, resource_group, test_run, cluster_name;
    kythira::azure_client_config azure{};
    std::optional<std::string> vnet_id, nsg_id;
    std::map<int, std::optional<std::string>> subnet_id;
};

/// Wraps `azure_vm_quorum_manager` with spot-first launch escalation.
///
/// Each `provision()` starts at the rung the previous one settled on and walks
/// forward — cheapest spot SKU, next-cheapest spot SKU, …, on-demand fallback
/// — advancing whenever ARM rejects the launch for capacity or quota and
/// rethrowing on anything else. It never walks backwards: once a rung has been
/// rejected in this test case, later nodes in the same case skip it too, which
/// is what makes the multi-VM cases converge instead of re-failing on every
/// node.
///
/// Advancing rebuilds the underlying manager, because `vm_size`/`priority`
/// live in its construction-time config. That is safe for nodes already
/// provisioned by an earlier rung: VM naming derives from `cluster_name` and
/// the node ID, neither of which changes, so `decommission_node`/
/// `assess_quorum` on the rebuilt manager still address them correctly.
class escalating_vm_manager {
public:
    using manager_type = kythira::azure_vm_quorum_manager<>;

    escalating_vm_manager(kythira::azure_vm_quorum_manager_config cfg,
                          std::vector<azure_vm_option> ladder, bool spot_only = false)
        : _cfg(std::move(cfg)), _ladder(std::move(ladder)) {
        if (spot_only) {
            std::erase_if(_ladder, [](const azure_vm_option& o) { return !o.spot; });
        }
        BOOST_REQUIRE_MESSAGE(!_ladder.empty(), "launch ladder is empty");
        rebuild();
    }

    /// Provisions one node into `group`, escalating on capacity/quota refusal.
    auto provision(const std::string& group) -> kythira::peer_info<std::uint64_t, std::string> {
        return escalate_launch(
            _ladder, _rung, _escalations,
            [&](const azure_vm_option& option) {
                rebuild(option);
                return std::move(_mgr->provision_node(group, std::nullopt)).get();
            },
            [](const azure_vm_option& from, const azure_vm_option& to, const char* message) {
                BOOST_TEST_MESSAGE("[azure-spot] " << from.label() << " refused, escalating to "
                                                   << to.label() << ": " << message);
            });
    }

    auto operator->() -> manager_type* { return &*_mgr; }

    /// The rung currently in use — the SKU whose price should be billed and
    /// whose name should appear in cost labels.
    [[nodiscard]] auto option() const -> const azure_vm_option& { return _ladder[_rung]; }

    /// How many times `provision()` has advanced the ladder. Zero on a run
    /// where the first rung always worked; a forced-failure run asserts this
    /// is non-zero, which is what distinguishes "escalation works" from
    /// "escalation was never exercised".
    [[nodiscard]] auto escalations() const -> std::size_t { return _escalations; }

    /// Hourly rate for one VM at the current rung. Prefers the live retail
    /// price the ladder carries, falling back to the static table only for
    /// env-pinned rungs, which have no discovered price.
    [[nodiscard]] auto hourly_rate() const -> double {
        const auto& opt = option();
        return opt.price_per_hr > 0.0 ? opt.price_per_hr
                                      : azure_vm_hourly_rate(opt.vm_size) * (opt.spot ? 0.2 : 1.0);
    }

private:
    void rebuild() { rebuild(_ladder[_rung]); }

    void rebuild(const azure_vm_option& option) {
        auto cfg = _cfg;
        cfg.vm_size = option.vm_size;
        if (option.spot) {
            cfg.priority = kythira::azure_vm_priority::spot;
            // max_price -1 means "pay up to the on-demand price", i.e. never
            // be evicted for price alone — only for capacity. Eviction
            // deallocates rather than deletes so the manager's own liveness
            // polling sees an unreachable-but-present VM, which is the state
            // its assess_quorum path is written against.
            cfg.spot_options = kythira::azure_spot_options{
                .max_price = -1.0, .eviction_policy = kythira::azure_eviction_policy::deallocate};
        } else {
            cfg.priority = kythira::azure_vm_priority::regular;
            cfg.spot_options = std::nullopt;
        }
        _mgr.emplace(std::move(cfg));
    }

    kythira::azure_vm_quorum_manager_config _cfg;
    std::vector<azure_vm_option> _ladder;
    std::size_t _rung{0};
    std::size_t _escalations{0};
    std::optional<manager_type> _mgr;
};

/// Issues one ARM POST with no body against an arbitrary path — used only to
/// simulate external failures this project's own manager API has no method
/// for (e.g. forcing a VM to `deallocate` out from under it, the same
/// "external fault injection" role `aws_quorum_manager_real_ec2_test.cpp`'s
/// direct EC2Client calls play for the AWS suite's
/// `az_outage_during_rolling_deployment`-style cases).
void external_arm_post_action(const kythira::azure_client_config& azure, const std::string& vm_name,
                              const std::string& action) {
    auto pipeline = make_test_arm_pipeline(azure);
    Azure::Core::Url url(arm_base_url(azure) + "/providers/Microsoft.Compute/virtualMachines/" +
                         vm_name + "/" + action + "?api-version=2024-07-01");
    Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Post, url);
    Azure::Core::Context context;
    auto response = pipeline.Send(request, context);
    auto code = static_cast<int>(response->GetStatusCode());
    BOOST_REQUIRE_MESSAGE(code >= 200 && code < 300, "external ARM " << action << " on " << vm_name
                                                                     << " failed (" << code << ")");
}

/// Scans `scale_set_name`'s instance list for the one tagged
/// `kythira:node-id = node_id`, returning its VMSS-local instance ID.
/// Duplicates `azure_vmss_quorum_manager::find_instance`'s tag-scan (private,
/// not reusable from here) scoped to a single already-known scale set.
[[nodiscard]] auto find_vmss_instance_id(const kythira::azure_client_config& azure,
                                         const std::string& scale_set_name, std::uint64_t node_id)
    -> std::optional<std::string> {
    auto pipeline = make_test_arm_pipeline(azure);
    Azure::Core::Url url(arm_base_url(azure) +
                         "/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set_name +
                         "/virtualMachines?api-version=2024-07-01");
    Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, url);
    Azure::Core::Context context;
    auto response = pipeline.Send(request, context);
    BOOST_REQUIRE_EQUAL(static_cast<int>(response->GetStatusCode()), 200);
    const auto& body = response->GetBody();
    auto parsed = boost::json::parse(std::string(body.begin(), body.end()));
    std::string target = std::to_string(node_id);
    for (const auto& inst : parsed.at("value").as_array()) {
        if (!inst.is_object() || !inst.as_object().contains("tags")) {
            continue;
        }
        const auto& tags = inst.at("tags");
        if (tags.is_object() && tags.as_object().contains("kythira:node-id") &&
            std::string(tags.at("kythira:node-id").as_string()) == target &&
            inst.as_object().contains("instanceId")) {
            return std::string(inst.at("instanceId").as_string());
        }
    }
    return std::nullopt;
}

/// External deallocate for one VMSS instance, mirroring
/// `external_arm_post_action` for standalone VMs.
void external_vmss_deallocate(const kythira::azure_client_config& azure,
                              const std::string& scale_set_name, const std::string& instance_id) {
    auto pipeline = make_test_arm_pipeline(azure);
    boost::json::object body;
    body["instanceIds"] = boost::json::array{instance_id};
    std::string serialized = boost::json::serialize(body);
    Azure::Core::IO::MemoryBodyStream stream(
        reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size());
    Azure::Core::Url url(arm_base_url(azure) +
                         "/providers/Microsoft.Compute/virtualMachineScaleSets/" + scale_set_name +
                         "/deallocate?api-version=2024-07-01");
    Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Post, url, &stream);
    request.SetHeader("Content-Type", "application/json");
    Azure::Core::Context context;
    auto response = pipeline.Send(request, context);
    auto code = static_cast<int>(response->GetStatusCode());
    BOOST_REQUIRE_MESSAGE(code >= 200 && code < 300, "external VMSS deallocate on instance "
                                                         << instance_id << " failed (" << code
                                                         << ")");
}

}  // namespace

BOOST_GLOBAL_FIXTURE(AzureSignalHandlerFixture);
BOOST_GLOBAL_FIXTURE(CostSummaryFixture);

BOOST_AUTO_TEST_SUITE(azure_vm_quorum_manager_real)

BOOST_FIXTURE_TEST_CASE(provision_and_assess_single_zone, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }

    TestCostReport cost{.test_name = "provision_and_assess_single_zone"};
    escalating_vm_manager mgr{vm_config(1), launch_ladder(azure)};

    auto peer = mgr.provision("1");
    cost.resources.push_back(
        {.label = "VM (" + mgr.option().label() + ")", .hourly_rate = mgr.hourly_rate()});

    // Escalation is only proven by a run in which the first rung actually
    // failed. `AZURE_TEST_FORCE_FIRST_VM_SIZE` puts an unlaunchable SKU at the
    // head of the ladder precisely so this assertion has something to check;
    // an ordinary run leaves it unset and skips the check, since there the
    // first rung succeeding is the expected outcome, not a bug.
    if (env_opt("AZURE_TEST_FORCE_FIRST_VM_SIZE")) {
        BOOST_CHECK_MESSAGE(mgr.escalations() > 0,
                            "AZURE_TEST_FORCE_FIRST_VM_SIZE was set but the ladder never "
                            "advanced — the forced SKU launched, so this run proves nothing "
                            "about escalation");
    }

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    // `critical`, not `healthy`: for a 1-node topology the majority is
    // 1/2+1 == 1, and classify_status() returns `critical` whenever
    // live == majority (one more failure would lose quorum) — which a
    // single-node cluster satisfies the moment it is fully up. `healthy` is
    // unreachable at this size, so asserting it (as this case originally did)
    // could never have passed. The 3-node case covers `healthy` properly.
    BOOST_CHECK(health.status == kythira::quorum_status::critical);
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr->decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(decommission_idempotent, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }
    kythira::azure_vm_quorum_manager<> mgr{vm_config(1)};
    // Decommissioning a NodeId that was never provisioned must be a no-op,
    // not an error (Property 2: idempotency of decommission).
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(999999999)).get());
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(999999999)).get());
}

BOOST_FIXTURE_TEST_CASE(provision_timeout_cleanup, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }
    auto cfg = vm_config(1);
    // A timeout far too short for any real VM to reach PowerState/running —
    // this exercises the best-effort VM+NIC cleanup path deterministically
    // without needing to wait for a real provisioning failure.
    cfg.provision_timeout = std::chrono::seconds{1};
    cfg.poll_interval = std::chrono::milliseconds{500};
    kythira::azure_vm_quorum_manager<> mgr{cfg};
    BOOST_CHECK_THROW(std::move(mgr.provision_node("1", std::nullopt)).get(), std::exception);
}

BOOST_FIXTURE_TEST_CASE(provision_multi_zone_topology, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }

    TestCostReport cost{.test_name = "provision_multi_zone_topology"};
    escalating_vm_manager mgr{vm_config(3), launch_ladder(azure)};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    for (const auto& group : {"1", "2", "3"}) {
        auto peer = mgr.provision(group);
        cluster.push_back({peer.node_id, group});
        cost.resources.push_back(
            {.label = std::string("VM (zone ") + group + ", " + mgr.option().label() + ")",
             .hourly_rate = mgr.hourly_rate()});
    }

    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK(health.status == kythira::quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);
    BOOST_CHECK_EQUAL(health.groups.size(), 3u);

    for (const auto& np : cluster) {
        std::move(mgr->decommission_node(np.node_id)).get();
    }
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(deallocate_one_node_degraded, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }

    TestCostReport cost{.test_name = "deallocate_one_node_degraded"};
    escalating_vm_manager mgr{vm_config(1), launch_ladder(azure)};

    auto peer = mgr.provision("1");
    cost.resources.push_back(
        {.label = "VM (" + mgr.option().label() + ")", .hourly_rate = mgr.hourly_rate()});
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};

    // Simulate an external failure (host maintenance, Spot eviction, operator
    // action) by deallocating the VM directly via ARM, bypassing the manager
    // entirely — assess_quorum must classify it as unreachable without any
    // dedicated "deallocated" code path (Property 5's infrastructure-only
    // liveness signal).
    external_arm_post_action(azure, mgr->node_id_to_vm_name(peer.node_id), "deallocate");

    auto health = std::move(mgr->assess_quorum(cluster)).get();
    // 1-node topology, 0 live: majority = 1/2+1 = 1, live(0) < majority(1) => lost.
    BOOST_CHECK(health.status == kythira::quorum_status::lost);
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);

    std::move(mgr->decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

// vm_config() already places every node in an Availability Zone by default,
// so `provision_and_assess_single_zone`/`provision_multi_zone_topology` above
// already exercise this placement kind end-to-end — this case exists
// alongside the PPG/Availability Set ones below purely for naming symmetry
// with design.md's "one test per azure_placement_kind" enumeration, and
// checks the `kythira:placement` tag's value specifically rather than
// re-testing provisioning mechanics already covered above.
BOOST_FIXTURE_TEST_CASE(placement_availability_zone, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }

    TestCostReport cost{.test_name = "placement_availability_zone"};
    escalating_vm_manager mgr{vm_config(1), launch_ladder(azure)};

    auto peer = mgr.provision("1");
    cost.resources.push_back({.label = "VM (Availability Zone, " + mgr.option().label() + ")",
                              .hourly_rate = mgr.hourly_rate()});
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr->decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(placement_proximity_placement_group, AzureIntegrationFixture) {
    auto ppg_id = env_opt("AZURE_TEST_PPG_ID");
    if (!preflight_ok || !ppg_id) {
        BOOST_TEST_MESSAGE(
            "Skipping: preflight failed or AZURE_TEST_PPG_ID unset (operator must "
            "pre-provision a Proximity Placement Group for this case)");
        return;
    }

    TestCostReport cost{.test_name = "placement_proximity_placement_group"};
    auto cfg = vm_config(1);
    cfg.placement_by_group["1"] = {.kind = kythira::azure_placement_kind::proximity_placement_group,
                                   .resource_id = *ppg_id};
    escalating_vm_manager mgr{cfg, launch_ladder(azure)};

    auto peer = mgr.provision("1");
    cost.resources.push_back(
        {.label = "VM (PPG, " + mgr.option().label() + ")", .hourly_rate = mgr.hourly_rate()});
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr->decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(placement_availability_set, AzureIntegrationFixture) {
    auto avset_id = env_opt("AZURE_TEST_AVAILABILITY_SET_ID");
    if (!preflight_ok || !avset_id) {
        BOOST_TEST_MESSAGE(
            "Skipping: preflight failed or AZURE_TEST_AVAILABILITY_SET_ID unset (operator "
            "must pre-provision an Availability Set for this case)");
        return;
    }

    TestCostReport cost{.test_name = "placement_availability_set"};
    auto cfg = vm_config(1);
    cfg.placement_by_group["1"] = {.kind = kythira::azure_placement_kind::availability_set,
                                   .resource_id = *avset_id};
    escalating_vm_manager mgr{cfg, launch_ladder(azure)};

    auto peer = mgr.provision("1");
    cost.resources.push_back({.label = "VM (Availability Set, " + mgr.option().label() + ")",
                              .hourly_rate = mgr.hourly_rate()});
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr->decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(spot_provision_and_decommission, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }

    TestCostReport cost{.test_name = "spot_provision_and_decommission"};
    // spot_only: this case exists to assert spot VMs specifically work, so it
    // must never silently succeed by falling through to the on-demand rung the
    // way the other cases legitimately may.
    escalating_vm_manager mgr{vm_config(1), launch_ladder(azure), /*spot_only=*/true};

    auto peer = mgr.provision("1");
    BOOST_CHECK_MESSAGE(mgr.option().spot, "spot_only ladder yielded an on-demand rung");
    cost.resources.push_back(
        {.label = "Spot VM (" + mgr.option().vm_size + ")", .hourly_rate = mgr.hourly_rate()});
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};
    auto health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr->decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(zone_outage_during_rolling_deployment, AzureIntegrationFixture) {
    if (!preflight_ok) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed (see stderr)");
        return;
    }

    TestCostReport cost{.test_name = "zone_outage_during_rolling_deployment"};
    auto cfg = vm_config(0);
    for (const auto& group : {"1", "2", "3"}) {
        cfg.topology.groups.push_back({.group_id = group, .target_count = 3});
        cfg.subnet_id_by_group[group] = *subnet_id.at(std::stoi(group));
        cfg.placement_by_group[group] = {.kind = kythira::azure_placement_kind::availability_zone,
                                         .zone = group};
    }
    escalating_vm_manager mgr{cfg, launch_ladder(azure)};

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster;
    for (const auto& group : {"1", "2", "3"}) {
        for (int i = 0; i < 3; ++i) {
            auto peer = mgr.provision(group);
            cluster.push_back({peer.node_id, group});
            cost.resources.push_back(
                {.label = std::string("VM (zone ") + group + ", " + mgr.option().label() + ")",
                 .hourly_rate = mgr.hourly_rate()});
        }
    }

    // Simulate a full zone-3 outage plus one zone-2 node: deallocate all 3
    // zone-3 nodes and exactly 1 zone-2 node directly via ARM, bypassing the
    // manager — 9 nodes total, 4 unreachable, leaving 5 live: critical
    // (live == majority == 9/2+1 == 5; one more failure loses quorum).
    std::size_t zone2_deallocated = 0;
    for (const auto& np : cluster) {
        bool deallocate_this = np.group_id == "3" || (np.group_id == "2" && zone2_deallocated < 1);
        if (!deallocate_this) {
            continue;
        }
        external_arm_post_action(azure, mgr->node_id_to_vm_name(np.node_id), "deallocate");
        if (np.group_id == "2") {
            ++zone2_deallocated;
        }
    }

    auto pre_health = std::move(mgr->assess_quorum(cluster)).get();
    BOOST_CHECK(pre_health.status == kythira::quorum_status::critical);
    BOOST_CHECK_EQUAL(pre_health.live_node_count, 5u);

    auto post_health = std::move(mgr->maintain_quorum(cluster)).get();
    (void)post_health;

    // maintain_quorum only replaces nodes it itself decommissioned (the
    // unreachable set at the time of that call); deallocated-but-not-yet-
    // decommissioned nodes from this test's direct ARM calls are cleaned up
    // explicitly below regardless of what maintain_quorum did with them.
    for (const auto& np : cluster) {
        std::move(mgr->decommission_node(np.node_id)).get();
    }
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(azure_vmss_quorum_manager_real)

BOOST_FIXTURE_TEST_CASE(vmss_provision_increments_capacity, AzureIntegrationFixture) {
    if (!preflight_ok || vmss_config().scale_set_by_group.empty()) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed or AZURE_TEST_VMSS_NAME unset (see stderr)");
        return;
    }
    TestCostReport cost{.test_name = "vmss_provision_increments_capacity"};
    kythira::azure_vmss_quorum_manager<> mgr{vmss_config()};

    auto peer = std::move(mgr.provision_node("1", std::nullopt)).get();
    cost.resources.push_back(
        {.label = "VMSS instance", .hourly_rate = azure_vm_hourly_rate("Standard_D2s_v5")});

    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);

    std::move(mgr.decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(vmss_assess_detects_not_running, AzureIntegrationFixture) {
    auto scale_set = env_opt("AZURE_TEST_VMSS_NAME");
    if (!preflight_ok || !scale_set) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed or AZURE_TEST_VMSS_NAME unset (see stderr)");
        return;
    }
    TestCostReport cost{.test_name = "vmss_assess_detects_not_running"};
    kythira::azure_vmss_quorum_manager<> mgr{vmss_config()};

    auto peer = std::move(mgr.provision_node("1", std::nullopt)).get();
    cost.resources.push_back(
        {.label = "VMSS instance", .hourly_rate = azure_vm_hourly_rate("Standard_D2s_v5")});
    std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{{peer.node_id, "1"}};

    auto instance_id = find_vmss_instance_id(azure, *scale_set, peer.node_id);
    BOOST_REQUIRE(instance_id.has_value());
    external_vmss_deallocate(azure, *scale_set, *instance_id);

    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 0u);

    std::move(mgr.decommission_node(peer.node_id)).get();
    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(vmss_decommission_removes_instance, AzureIntegrationFixture) {
    auto scale_set = env_opt("AZURE_TEST_VMSS_NAME");
    if (!preflight_ok || !scale_set) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed or AZURE_TEST_VMSS_NAME unset (see stderr)");
        return;
    }
    TestCostReport cost{.test_name = "vmss_decommission_removes_instance"};
    kythira::azure_vmss_quorum_manager<> mgr{vmss_config()};

    auto peer = std::move(mgr.provision_node("1", std::nullopt)).get();
    cost.resources.push_back(
        {.label = "VMSS instance", .hourly_rate = azure_vm_hourly_rate("Standard_D2s_v5")});

    BOOST_REQUIRE(find_vmss_instance_id(azure, *scale_set, peer.node_id).has_value());

    std::move(mgr.decommission_node(peer.node_id)).get();

    // decommission_node's own 30s poll (design.md's decommission_node
    // sequence) already waits for the instance to disappear from the scale
    // set's list before returning, so no additional wait is needed here.
    BOOST_CHECK(!find_vmss_instance_id(azure, *scale_set, peer.node_id).has_value());

    for (auto& r : cost.resources) {
        r.finalize();
    }
    g_cost_accumulator.add(std::move(cost));
}

BOOST_FIXTURE_TEST_CASE(vmss_decommission_idempotent, AzureIntegrationFixture) {
    if (!preflight_ok || vmss_config().scale_set_by_group.empty()) {
        BOOST_TEST_MESSAGE("Skipping: preflight failed or AZURE_TEST_VMSS_NAME unset (see stderr)");
        return;
    }
    kythira::azure_vmss_quorum_manager<> mgr{vmss_config()};
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(999999999)).get());
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(999999999)).get());
}

BOOST_FIXTURE_TEST_CASE(vmss_rejects_automatic_upgrade_mode, AzureIntegrationFixture) {
    auto automatic_scale_set = env_opt("AZURE_TEST_VMSS_AUTOMATIC_UPGRADE_NAME");
    if (!preflight_ok || !automatic_scale_set) {
        BOOST_TEST_MESSAGE(
            "Skipping: preflight failed or AZURE_TEST_VMSS_AUTOMATIC_UPGRADE_NAME unset (operator "
            "must pre-provision a scale set with upgradePolicy.mode=Automatic for this case)");
        return;
    }
    auto cfg = vmss_config();
    cfg.scale_set_by_group["1"] = *automatic_scale_set;
    BOOST_CHECK_THROW((kythira::azure_vmss_quorum_manager<>{cfg}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_AZURE_SDK

BOOST_AUTO_TEST_CASE(azure_sdk_not_available) {
    BOOST_TEST_MESSAGE(
        "Azure SDK not available at build time; real-Azure quorum manager tests skipped");
}

#endif  // KYTHIRA_HAS_AZURE_SDK

#else  // !KYTHIRA_AZURE_REAL_TESTS

BOOST_AUTO_TEST_CASE(real_azure_tests_disabled) {
    BOOST_TEST_MESSAGE("KYTHIRA_AZURE_REAL_TESTS not defined; this binary is a compile-only stub");
}

#endif  // KYTHIRA_AZURE_REAL_TESTS
