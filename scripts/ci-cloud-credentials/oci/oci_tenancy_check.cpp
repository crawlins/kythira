/// @file oci_tenancy_check.cpp
/// @brief Validate OCI Request Signing v1 — and, progressively, an OCI
///        tenancy's readiness for the real-OCI test tier — against the real
///        service, using the same `oci_http_client` the providers use.
///
/// Why this exists: the signing scheme in this tree has been checked two ways,
/// and neither of them talks to Oracle. `oci_signing_unit_test.cpp` pins it
/// against golden vectors derived from `oci-go-sdk`'s source, and
/// `oci_mock_server.hpp` deliberately does not verify signatures. A wrong
/// canonical string still produces a well-formed request; the only symptom is a
/// 401 that says nothing about which side is wrong. One authenticated call
/// against a real endpoint settles it, and stage 1 below launches nothing and
/// costs nothing.
///
/// Each stage is skipped when its env var is unset, so this is runnable at
/// every point of the tenancy walkthrough — stage 1 needs only an API key.

#include <raft/oci_http_client.hpp>
#include <raft/oci_instance_pool_quorum_manager.hpp>
#include <raft/oci_signing.hpp>

#include <boost/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

auto env(const char* name) -> std::string {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

auto require_env(const char* name) -> std::string {
    auto value = env(name);
    if (value.empty()) {
        std::cerr << "FATAL: " << name << " is required\n";
        std::exit(2);
    }
    return value;
}

int failures = 0;
int skipped = 0;

/// Run one probe. On failure, print the exact canonical string that was signed
/// — the single most useful thing to look at when OCI answers 401, and the one
/// thing the error itself never contains.
auto probe(const kythira::oci_http_client& client, const kythira::oci_client_config& cfg,
           const std::string& label, const std::string& service, const std::string& path) -> bool {
    std::cout << "\n[" << label << "]\n  GET https://" << client.host_for(service) << path << "\n";
    try {
        const auto body = client.request(service, "GET", path);
        std::cout << "  OK\n";
        const auto rendered = boost::json::serialize(body);
        std::cout << "  " << (rendered.size() > 240 ? rendered.substr(0, 240) + " ..." : rendered)
                  << "\n";
        return true;
    } catch (const std::exception& e) {
        ++failures;
        std::cout << "  FAILED: " << e.what() << "\n";
        std::cout << "  --- canonical string that was signed ---\n";
        const auto signing_string = kythira::oci_signing::build_signing_string(
            "GET", path, client.host_for(service), "", "<date header>");
        std::cout << signing_string << "\n";
        std::cout << "  ---------------------------------------\n";
        std::cout << "  A 401 here means the signature or keyId is wrong; a 404 with\n"
                     "  NotAuthorizedOrNotFound usually means the IAM policy is, since OCI\n"
                     "  reports 'not permitted' and 'does not exist' identically.\n";
        return false;
    }
}

auto optional_probe(const kythira::oci_http_client& client, const kythira::oci_client_config& cfg,
                    const std::string& label, const char* env_name, const std::string& service,
                    const std::string& path_prefix, const std::string& path_suffix = "") -> void {
    const auto ocid = env(env_name);
    if (ocid.empty()) {
        ++skipped;
        std::cout << "\n[" << label << "]\n  SKIPPED (" << env_name << " unset)\n";
        return;
    }
    (void)probe(client, cfg, label, service, path_prefix + ocid + path_suffix);
}

}  // namespace

auto main() -> int {
    kythira::oci_client_config cfg;
    cfg.region = require_env("KYTHIRA_OCI_REGION");
    cfg.tenancy_id = require_env("KYTHIRA_OCI_TENANCY_ID");
    cfg.user_id = require_env("KYTHIRA_OCI_USER_ID");
    cfg.fingerprint = require_env("KYTHIRA_OCI_FINGERPRINT");
    cfg.private_key_pem = require_env("KYTHIRA_OCI_PRIVATE_KEY_PEM");
    cfg.private_key_passphrase = env("KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE");
    cfg.api_timeout = std::chrono::seconds{30};

    const kythira::oci_http_client client{cfg};

    std::cout << "region      " << cfg.region << "\n"
              << "tenancy     " << cfg.tenancy_id << "\n"
              << "user        " << cfg.user_id << "\n"
              << "fingerprint " << cfg.fingerprint << "\n"
              << "key         " << cfg.private_key_pem.size() << " bytes\n";

    // Stage 1 — signing itself. `ListRegions` is the call Requirement 13.9
    // names for the real fixture's preflight: authenticated, read-only, and
    // permitted to any principal without a specific policy. So a failure here
    // is the signature, not the IAM policy, which is exactly the separation
    // that makes it worth running first.
    const bool signing_ok =
        probe(client, cfg, "1. signing (ListRegions)", "identity", "/20160918/regions");
    if (!signing_ok) {
        std::cout << "\nSigning failed — later stages would all fail for the same reason.\n";
        return 1;
    }

    // Stage 2 — the compartment and the instance-family read permission.
    if (const auto compartment = env("KYTHIRA_OCI_COMPARTMENT_ID"); !compartment.empty()) {
        (void)probe(client, cfg, "2. compartment access (ListInstances)", "iaas",
                    "/20160918/instances?compartmentId=" + compartment);
    } else {
        ++skipped;
        std::cout << "\n[2. compartment access]\n  SKIPPED (KYTHIRA_OCI_COMPARTMENT_ID unset)\n";
    }

    // Stage 3 — exactly what oci_instance_pool_quorum_manager's constructor
    // does, including reading placementConfigurations. If this passes, the
    // manager will construct.
    if (const auto pool = env("KYTHIRA_OCI_INSTANCE_POOL_ID"); !pool.empty()) {
        if (probe(client, cfg, "3. GetInstancePool (what the ctor does)", "iaas",
                  "/20160918/instancePools/" + pool)) {
            try {
                const auto body = client.request("iaas", "GET", "/20160918/instancePools/" + pool);
                const auto* obj = body.if_object();
                const auto* placements =
                    obj != nullptr ? obj->if_contains("placementConfigurations") : nullptr;
                const auto count = (placements != nullptr && placements->is_array())
                                       ? placements->get_array().size()
                                       : 0;
                std::cout << "  placementConfigurations: " << count << "\n";
                if (count != 1) {
                    ++failures;
                    std::cout << "  PROBLEM: the design requires exactly ONE placement\n"
                                 "  configuration per pool. With "
                              << count
                              << ", provision_node\n"
                                 "  cannot honour its target_group — OCI chooses placement.\n";
                }
                if (const auto size = obj != nullptr ? obj->if_contains("size") : nullptr;
                    size != nullptr && size->is_int64() && size->get_int64() != 0) {
                    std::cout << "  NOTE: size is " << size->get_int64()
                              << ", not 0. Instances launched before a manager exists carry no\n"
                                 "  kythira-node-id tag, so the first provision_node adopts one.\n";
                }
            } catch (const std::exception&) {
                // The probe above already reported it.
            }
        }
    } else {
        ++skipped;
        std::cout << "\n[3. GetInstancePool]\n  SKIPPED (KYTHIRA_OCI_INSTANCE_POOL_ID unset)\n";
    }

    // Stage 4 — the listing assess_quorum drives.
    if (const auto pool = env("KYTHIRA_OCI_INSTANCE_POOL_ID"),
        compartment = env("KYTHIRA_OCI_COMPARTMENT_ID");
        !pool.empty() && !compartment.empty()) {
        (void)probe(client, cfg, "4. ListInstancePoolInstances", "iaas",
                    "/20160918/instancePools/" + pool + "/instances?compartmentId=" + compartment);
    } else {
        ++skipped;
        std::cout
            << "\n[4. ListInstancePoolInstances]\n  SKIPPED (pool and/or compartment unset)\n";
    }

    // Stage 5 — the *other* service host. Worth its own stage: a provider that
    // sent this to the management plane instead of the retrieval plane gets a
    // 404 with nothing to suggest why.
    optional_probe(client, cfg, "5. GetCertificateAuthorityBundle",
                   "KYTHIRA_OCI_CERTIFICATE_AUTHORITY_ID", "certificates",
                   "/20210224/certificateAuthorityBundles/");

    // Stage 6 -- the only stage that costs money, so it is opt-in twice over:
    // a pool OCID *and* an explicit KYTHIRA_OCI_ALLOW_LAUNCH=1. It drives the
    // real oci_instance_pool_quorum_manager through provision -> assess ->
    // decommission, which is the whole path Task 6's suite will automate and
    // the only way to exercise the size bump, the untagged-instance poll, the
    // read-merge-write tag write and the VNIC lookup against real OCI.
    //
    // Cleanup runs whatever happens above. An instance leaked here bills until
    // someone notices, and every other cloud provider in this project leaked
    // one on its first live run.
    if (env("KYTHIRA_OCI_ALLOW_LAUNCH") == "1" && !env("KYTHIRA_OCI_INSTANCE_POOL_ID").empty()) {
        std::cout << "\n[6. full lifecycle -- LAUNCHES A BILLABLE INSTANCE]\n";
        kythira::oci_instance_pool_quorum_manager_config qcfg;
        qcfg.oci = cfg;
        qcfg.compartment_id = env("KYTHIRA_OCI_COMPARTMENT_ID");
        qcfg.instance_pool_id = env("KYTHIRA_OCI_INSTANCE_POOL_ID");
        qcfg.cluster_name = "kythira-tenancy-check";
        qcfg.node_port = 7000;
        // A stock image runs no kythira process, so there is no heartbeat tag
        // to be fresh. Zero selects lifecycleState-only liveness.
        qcfg.heartbeat_timeout = std::chrono::seconds{0};
        qcfg.provision_timeout = std::chrono::seconds{600};
        qcfg.poll_interval = std::chrono::milliseconds{10000};

        std::uint64_t node = 0;
        kythira::oci_instance_pool_quorum_manager<> mgr{qcfg};
        const std::string ad = mgr.availability_domains().front();
        std::cout << "  availability domain: " << ad << "\n" << std::flush;
        try {
            const auto started = std::chrono::steady_clock::now();
            auto peer = std::move(mgr.provision_node(ad, std::nullopt)).get();
            node = peer.node_id;
            std::cout << "  provisioned node_id=" << peer.node_id << " address=" << peer.address
                      << " in "
                      << std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - started)
                             .count()
                      << "s\n"
                      << std::flush;

            const std::vector<kythira::node_placement<std::uint64_t, std::string>> cluster{
                {.node_id = node, .group_id = ad}};
            auto health = std::move(mgr.assess_quorum(cluster)).get();
            std::cout << "  assess: status=" << health.status << " live=" << health.live_node_count
                      << "/" << health.total_node_count << "\n"
                      << std::flush;
            if (health.live_node_count != 1) {
                ++failures;
                std::cout << "  PROBLEM: the node it just provisioned is not live\n";
            }
        } catch (const std::exception& e) {
            ++failures;
            std::cout << "  FAILED: " << e.what() << "\n" << std::flush;
        }
        if (node != 0) {
            try {
                std::move(mgr.decommission_node(node)).get();
                std::cout << "  decommissioned node_id=" << node << "\n" << std::flush;
            } catch (const std::exception& e) {
                ++failures;
                std::cout << "  CLEANUP FAILED -- check for a leaked instance: " << e.what() << "\n"
                          << std::flush;
            }
        }
    } else {
        ++skipped;
        std::cout << "\n[6. full lifecycle]\n  SKIPPED (set KYTHIRA_OCI_ALLOW_LAUNCH=1 to run; "
                     "it launches a billable instance)\n";
    }

    std::cout << "\n=========================================\n"
              << (failures == 0 ? "ALL RUN STAGES PASSED" : "FAILURES: " + std::to_string(failures))
              << "  (" << skipped << " skipped)\n";
    if (failures == 0 && skipped > 0) {
        std::cout << "Set the remaining env vars and re-run as you create each resource.\n";
    }
    return failures == 0 ? 0 : 1;
}
