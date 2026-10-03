// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE cloud_config_env_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/alibaba_client_config_env.hpp>
#include <raft/oci_client_config_env.hpp>

// The environment-to-config mapping `raft_object_backup` uses for its OCI and
// OSS arms (`.kiro/specs/object-backup-oci-oss-credentials/`, Requirement 6.1).
//
// Registered unconditionally: the two headers include neither httplib nor
// OpenSSL, so every build leg runs this whichever providers it carries. The
// environment is a map behind the injected lookup, so nothing here reads or
// writes the process environment.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace kythira;

namespace {

using env_map = std::map<std::string, std::string, std::less<>>;

auto lookup(const env_map& vars) -> env_lookup {
    return [vars](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(name);
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

/// Distinctive values for every secret variable. Requirement 3.4 says none of
/// them may appear in any message, and these are what the secrecy case looks
/// for.
constexpr const char* k_pem_sentinel = "SENTINEL-PEM-7f3a";
constexpr const char* k_passphrase_sentinel = "SENTINEL-PASS-91c2";
constexpr const char* k_oci_token_sentinel = "SENTINEL-OCI-TOKEN-5d0e";
constexpr const char* k_alibaba_secret_sentinel = "SENTINEL-ALI-SECRET-24bb";
constexpr const char* k_alibaba_token_sentinel = "SENTINEL-ALI-TOKEN-c81f";

auto api_key_env() -> env_map {
    return {
        {"KYTHIRA_OCI_REGION", "us-phoenix-1"},
        {"KYTHIRA_OCI_TENANCY_ID", "ocid1.tenancy.oc1..t"},
        {"KYTHIRA_OCI_USER_ID", "ocid1.user.oc1..u"},
        {"KYTHIRA_OCI_FINGERPRINT", "aa:bb:cc"},
        {"KYTHIRA_OCI_PRIVATE_KEY_PEM", k_pem_sentinel},
        {"KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE", k_passphrase_sentinel},
        {"KYTHIRA_OCI_ENDPOINT_OVERRIDE", "http://127.0.0.1:1"},
    };
}

auto alibaba_env() -> env_map {
    return {
        {"KYTHIRA_ALIBABA_REGION", "cn-hangzhou"},
        {"KYTHIRA_ALIBABA_ACCESS_KEY_ID", "LTAIexample"},
        {"KYTHIRA_ALIBABA_ACCESS_KEY_SECRET", k_alibaba_secret_sentinel},
        {"KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE", "http://127.0.0.1:2"},
    };
}

auto contains(const std::string& haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

/// A file under the temp directory, removed when the test ends.
struct temp_file {
    std::filesystem::path path;

    explicit temp_file(std::string_view contents) {
        path = std::filesystem::temp_directory_path() /
               ("kythira-oci-key-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::ofstream(path, std::ios::binary) << contents;
    }
    ~temp_file() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    temp_file(const temp_file&) = delete;
    auto operator=(const temp_file&) -> temp_file& = delete;
    temp_file(temp_file&&) = delete;
    auto operator=(temp_file&&) -> temp_file& = delete;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_client_config_env)

// Case 1.
BOOST_AUTO_TEST_CASE(api_key_mode_maps_every_field) {
    const auto result = oci_client_config_from_env(lookup(api_key_env()));
    BOOST_TEST(result.ok());
    BOOST_TEST(result.warnings.empty());
    BOOST_TEST(result.config.region == "us-phoenix-1");
    BOOST_TEST(result.config.tenancy_id == "ocid1.tenancy.oc1..t");
    BOOST_TEST(result.config.user_id == "ocid1.user.oc1..u");
    BOOST_TEST(result.config.fingerprint == "aa:bb:cc");
    BOOST_TEST(result.config.private_key_pem == k_pem_sentinel);
    BOOST_TEST(result.config.private_key_passphrase == k_passphrase_sentinel);
    BOOST_TEST(result.config.endpoint_override == "http://127.0.0.1:1");
    BOOST_TEST(result.config.security_token.empty());
    BOOST_TEST(!result.config.use_instance_principal);
}

// Case 2.
BOOST_AUTO_TEST_CASE(security_token_mode_maps_the_token_and_key_and_ignores_the_api_key_fields) {
    auto vars = api_key_env();
    vars["KYTHIRA_OCI_AUTH"] = "security_token";
    vars["KYTHIRA_OCI_SECURITY_TOKEN"] = k_oci_token_sentinel;
    const auto result = oci_client_config_from_env(lookup(vars));
    BOOST_TEST(result.ok());
    BOOST_TEST(result.config.security_token == k_oci_token_sentinel);
    BOOST_TEST(result.config.private_key_pem == k_pem_sentinel);
    BOOST_TEST(result.config.tenancy_id.empty());
    BOOST_TEST(result.config.user_id.empty());
    BOOST_TEST(result.config.fingerprint.empty());

    // And it needs none of them.
    const auto minimal = oci_client_config_from_env(lookup({
        {"KYTHIRA_OCI_AUTH", "security_token"},
        {"KYTHIRA_OCI_REGION", "us-phoenix-1"},
        {"KYTHIRA_OCI_SECURITY_TOKEN", k_oci_token_sentinel},
        {"KYTHIRA_OCI_PRIVATE_KEY_PEM", k_pem_sentinel},
    }));
    BOOST_TEST(minimal.ok());
}

// Case 3.
BOOST_AUTO_TEST_CASE(the_mode_is_inferred_and_never_as_instance_principal) {
    auto with_token = api_key_env();
    with_token["KYTHIRA_OCI_SECURITY_TOKEN"] = k_oci_token_sentinel;
    const auto token = oci_client_config_from_env(lookup(with_token));
    BOOST_TEST(token.ok());
    BOOST_TEST(token.config.security_token == k_oci_token_sentinel);
    BOOST_TEST(token.config.tenancy_id.empty());

    const auto api_key = oci_client_config_from_env(lookup(api_key_env()));
    BOOST_TEST(api_key.config.security_token.empty());
    BOOST_TEST(api_key.config.tenancy_id == "ocid1.tenancy.oc1..t");

    // Only the region: an operator who forgot the API-key variables is told
    // so, rather than sent to the instance metadata service.
    const auto region_only =
        oci_client_config_from_env(lookup({{"KYTHIRA_OCI_REGION", "us-phoenix-1"}}));
    BOOST_TEST(!region_only.ok());
    BOOST_TEST(!region_only.config.use_instance_principal);
    BOOST_REQUIRE_EQUAL(region_only.errors.size(), 1U);
    BOOST_TEST(contains(region_only.errors[0], "api_key"));
}

// Case 4.
BOOST_AUTO_TEST_CASE(instance_principal_needs_only_the_region_and_warns_about_strays) {
    const auto clean = oci_client_config_from_env(lookup({
        {"KYTHIRA_OCI_AUTH", "instance_principal"},
        {"KYTHIRA_OCI_REGION", "us-phoenix-1"},
    }));
    BOOST_TEST(clean.ok());
    BOOST_TEST(clean.warnings.empty());
    BOOST_TEST(clean.config.use_instance_principal);
    BOOST_TEST(clean.config.region == "us-phoenix-1");

    auto vars = api_key_env();
    vars["KYTHIRA_OCI_AUTH"] = "instance_principal";
    vars["KYTHIRA_OCI_SECURITY_TOKEN"] = k_oci_token_sentinel;
    const auto stray = oci_client_config_from_env(lookup(vars));
    BOOST_TEST(stray.ok());
    BOOST_TEST(stray.config.use_instance_principal);
    BOOST_TEST(stray.config.private_key_pem.empty());
    // Tenancy, user, fingerprint, key PEM, passphrase, token: one each.
    BOOST_TEST(stray.warnings.size() == 6U);
    for (const char* name : {"KYTHIRA_OCI_TENANCY_ID", "KYTHIRA_OCI_USER_ID",
                             "KYTHIRA_OCI_FINGERPRINT", "KYTHIRA_OCI_PRIVATE_KEY_PEM",
                             "KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE", "KYTHIRA_OCI_SECURITY_TOKEN"}) {
        const auto hits = std::count_if(stray.warnings.begin(), stray.warnings.end(),
                                        [&](const std::string& w) { return contains(w, name); });
        BOOST_TEST(hits == 1, name << " warned " << hits << " times");
    }
}

// Case 5.
BOOST_AUTO_TEST_CASE(an_empty_environment_gives_one_error_naming_everything) {
    const auto result = oci_client_config_from_env(lookup({}));
    BOOST_REQUIRE_EQUAL(result.errors.size(), 1U);
    const auto& message = result.errors[0];
    for (const char* name : {"KYTHIRA_OCI_REGION", "KYTHIRA_OCI_TENANCY_ID", "KYTHIRA_OCI_USER_ID",
                             "KYTHIRA_OCI_FINGERPRINT",
                             "KYTHIRA_OCI_PRIVATE_KEY_PEM or KYTHIRA_OCI_PRIVATE_KEY_FILE"}) {
        BOOST_TEST(contains(message, name), message << " does not name " << name);
    }
}

// Case 6.
BOOST_AUTO_TEST_CASE(both_key_sources_set_is_an_error_naming_both) {
    temp_file key{"-----BEGIN PRIVATE KEY-----\n"};
    auto vars = api_key_env();
    vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = key.path.string();
    const auto result = oci_client_config_from_env(lookup(vars));
    BOOST_REQUIRE_EQUAL(result.errors.size(), 1U);
    BOOST_TEST(contains(result.errors[0], "KYTHIRA_OCI_PRIVATE_KEY_PEM"));
    BOOST_TEST(contains(result.errors[0], "KYTHIRA_OCI_PRIVATE_KEY_FILE"));
    BOOST_TEST(!contains(result.errors[0], "missing"));
}

// Case 7.
BOOST_AUTO_TEST_CASE(the_key_file_is_read_and_its_failures_name_the_path) {
    auto vars = api_key_env();
    vars.erase("KYTHIRA_OCI_PRIVATE_KEY_PEM");

    {
        temp_file key{k_pem_sentinel};
        vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = key.path.string();
        const auto result = oci_client_config_from_env(lookup(vars));
        BOOST_TEST(result.ok());
        BOOST_TEST(result.config.private_key_pem == k_pem_sentinel);
    }

    const std::string absent = "/nonexistent/kythira/oci_api_key.pem";
    vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = absent;
    const auto missing = oci_client_config_from_env(lookup(vars));
    BOOST_REQUIRE_EQUAL(missing.errors.size(), 1U);
    BOOST_TEST(contains(missing.errors[0], "KYTHIRA_OCI_PRIVATE_KEY_FILE"));
    BOOST_TEST(contains(missing.errors[0], absent));
    BOOST_TEST(contains(missing.errors[0], "No such file or directory"));

    temp_file empty{""};
    vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = empty.path.string();
    const auto empty_result = oci_client_config_from_env(lookup(vars));
    BOOST_REQUIRE_EQUAL(empty_result.errors.size(), 1U);
    BOOST_TEST(contains(empty_result.errors[0], empty.path.string()));
    BOOST_TEST(contains(empty_result.errors[0], "empty"));
}

// Case 8.
BOOST_AUTO_TEST_CASE(an_unknown_auth_mode_lists_the_accepted_values) {
    auto vars = api_key_env();
    vars["KYTHIRA_OCI_AUTH"] = "apikey";
    const auto result = oci_client_config_from_env(lookup(vars));
    BOOST_REQUIRE_EQUAL(result.errors.size(), 1U);
    for (const char* accepted : {"api_key", "security_token", "instance_principal"}) {
        BOOST_TEST(contains(result.errors[0], accepted));
    }
    BOOST_TEST(contains(result.errors[0], "\"apikey\""));

    // Validation still ran under the inferred mode, so a second problem is
    // reported in the same pass rather than on the next attempt.
    vars.erase("KYTHIRA_OCI_USER_ID");
    const auto both = oci_client_config_from_env(lookup(vars));
    BOOST_TEST(both.errors.size() == 2U);
}

// Case 9.
BOOST_AUTO_TEST_CASE(a_variable_set_to_empty_counts_as_unset) {
    auto vars = api_key_env();
    vars["KYTHIRA_OCI_USER_ID"] = "";
    vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = "";  // not a second key source
    vars["KYTHIRA_OCI_AUTH"] = "";              // not an unknown mode
    const auto result = oci_client_config_from_env(lookup(vars));
    BOOST_REQUIRE_EQUAL(result.errors.size(), 1U);
    BOOST_TEST(contains(result.errors[0], "missing"));
    BOOST_TEST(contains(result.errors[0], "KYTHIRA_OCI_USER_ID"));
    BOOST_TEST(!contains(result.errors[0], "KYTHIRA_OCI_TENANCY_ID"));

    // An empty token does not select security_token mode either.
    auto token = api_key_env();
    token["KYTHIRA_OCI_SECURITY_TOKEN"] = "";
    BOOST_TEST(oci_client_config_from_env(lookup(token)).config.tenancy_id ==
               "ocid1.tenancy.oc1..t");
}

// Case 10.
BOOST_AUTO_TEST_CASE(the_namespace_variable_fills_namespace_name) {
    auto vars = api_key_env();
    const auto unset = oci_object_storage_config_from_env(lookup(vars));
    BOOST_TEST(unset.ok());
    BOOST_TEST(unset.config.namespace_name.empty());
    BOOST_TEST(unset.config.oci.tenancy_id == "ocid1.tenancy.oc1..t");

    vars["KYTHIRA_OCI_NAMESPACE"] = "axunmw4f0mln";
    const auto set = oci_object_storage_config_from_env(lookup(vars));
    BOOST_TEST(set.ok());
    BOOST_TEST(set.config.namespace_name == "axunmw4f0mln");

    // The base mapping's errors carry through.
    const auto empty = oci_object_storage_config_from_env(lookup({}));
    BOOST_TEST(!empty.ok());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(alibaba_client_config_env)

// Case 11.
BOOST_AUTO_TEST_CASE(every_field_maps_with_and_without_a_token) {
    auto vars = alibaba_env();
    const auto plain = alibaba_client_config_from_env(lookup(vars));
    BOOST_TEST(plain.ok());
    BOOST_TEST(plain.warnings.empty());
    BOOST_TEST(plain.config.region == "cn-hangzhou");
    BOOST_TEST(plain.config.access_key_id == "LTAIexample");
    BOOST_TEST(plain.config.access_key_secret == k_alibaba_secret_sentinel);
    BOOST_TEST(plain.config.security_token.empty());
    BOOST_TEST(plain.config.endpoint_override == "http://127.0.0.1:2");

    vars["KYTHIRA_ALIBABA_ACCESS_KEY_ID"] = "STS.example";
    vars["KYTHIRA_ALIBABA_SECURITY_TOKEN"] = k_alibaba_token_sentinel;
    const auto sts = alibaba_client_config_from_env(lookup(vars));
    BOOST_TEST(sts.ok());
    BOOST_TEST(sts.warnings.empty());
    BOOST_TEST(sts.config.security_token == k_alibaba_token_sentinel);
}

// Case 12.
BOOST_AUTO_TEST_CASE(an_empty_environment_names_all_three_required_variables_in_one_error) {
    const auto result = alibaba_client_config_from_env(lookup({}));
    BOOST_REQUIRE_EQUAL(result.errors.size(), 1U);
    for (const char* name : {"KYTHIRA_ALIBABA_REGION", "KYTHIRA_ALIBABA_ACCESS_KEY_ID",
                             "KYTHIRA_ALIBABA_ACCESS_KEY_SECRET"}) {
        BOOST_TEST(contains(result.errors[0], name));
    }
    BOOST_TEST(!contains(result.errors[0], "KYTHIRA_ALIBABA_SECURITY_TOKEN"));
}

// Case 13.
BOOST_AUTO_TEST_CASE(an_sts_key_without_a_token_warns) {
    auto vars = alibaba_env();
    vars["KYTHIRA_ALIBABA_ACCESS_KEY_ID"] = "STS.example";
    const auto result = alibaba_client_config_from_env(lookup(vars));
    BOOST_TEST(result.ok());
    BOOST_REQUIRE_EQUAL(result.warnings.size(), 1U);
    BOOST_TEST(contains(result.warnings[0], "KYTHIRA_ALIBABA_SECURITY_TOKEN"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(cloud_config_env_secrecy)

// Case 14. Every OCI and Alibaba message a sentinel-laden environment can
// produce, checked for every sentinel.
BOOST_AUTO_TEST_CASE(no_message_contains_a_secret_value) {
    const std::vector<const char*> sentinels = {k_pem_sentinel, k_passphrase_sentinel,
                                                k_oci_token_sentinel, k_alibaba_secret_sentinel,
                                                k_alibaba_token_sentinel};
    std::vector<std::string> messages;
    const auto collect = [&](const auto& result) {
        messages.insert(messages.end(), result.errors.begin(), result.errors.end());
        messages.insert(messages.end(), result.warnings.begin(), result.warnings.end());
    };

    temp_file key{k_pem_sentinel};
    const auto oci_secrets = [] {
        return env_map{
            {"KYTHIRA_OCI_PRIVATE_KEY_PEM", k_pem_sentinel},
            {"KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE", k_passphrase_sentinel},
            {"KYTHIRA_OCI_SECURITY_TOKEN", k_oci_token_sentinel},
        };
    };
    for (const char* mode : {"api_key", "security_token", "instance_principal", "bogus", ""}) {
        auto vars = oci_secrets();
        vars["KYTHIRA_OCI_AUTH"] = mode;
        collect(oci_client_config_from_env(lookup(vars)));  // missing + both-keys
        vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = key.path.string();
        collect(oci_client_config_from_env(lookup(vars)));
        vars.erase("KYTHIRA_OCI_PRIVATE_KEY_PEM");
        vars["KYTHIRA_OCI_PRIVATE_KEY_FILE"] = "/nonexistent/key.pem";
        collect(oci_object_storage_config_from_env(lookup(vars)));
    }
    auto ali = env_map{
        {"KYTHIRA_ALIBABA_ACCESS_KEY_ID", "STS.example"},
        {"KYTHIRA_ALIBABA_ACCESS_KEY_SECRET", k_alibaba_secret_sentinel},
    };
    collect(alibaba_client_config_from_env(lookup(ali)));
    ali["KYTHIRA_ALIBABA_SECURITY_TOKEN"] = k_alibaba_token_sentinel;
    collect(alibaba_client_config_from_env(lookup(ali)));

    BOOST_TEST(messages.size() >= 10U, "the cases above should produce messages to check");
    for (const auto& message : messages) {
        for (const char* sentinel : sentinels) {
            BOOST_TEST(!contains(message, sentinel), "\"" << message << "\" leaks a secret");
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
