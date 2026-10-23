// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE aws_endpoint_override_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/aws_endpoint_override.hpp>

// Unit coverage for include/raft/aws_endpoint_override.hpp
// (object-backup-s3-endpoint Requirements 3.1-3.4 and 5.1, task 2).
//
// Registered unconditionally: the header includes no SDK, so every build leg
// runs it whether or not the S3 provider is compiled in.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace kythira;

namespace {

auto map_lookup(std::map<std::string, std::string> vars) -> env_lookup {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        auto it = vars.find(std::string(name));
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

auto contains(const std::string& haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

void require_rejected(std::string_view endpoint, std::string_view rule_text) {
    BOOST_TEST_CONTEXT("endpoint " << endpoint) {
        const auto check = validate_s3_endpoint(endpoint);
        BOOST_TEST(!check.ok());
        BOOST_TEST(contains(check.error, "--endpoint"));
        BOOST_TEST(contains(check.error, rule_text), "error was: " << check.error);
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(validate_s3_endpoint_suite)

BOOST_AUTO_TEST_CASE(accepts_the_designs_examples) {
    for (const char* endpoint :
         {"http://localhost:4566", "https://minio.internal:9000/", "http://127.0.0.1:9000",
          "http://[::1]:4566", "HTTPS://s3.example.com"}) {
        BOOST_TEST_CONTEXT("endpoint " << endpoint) {
            const auto check = validate_s3_endpoint(endpoint);
            BOOST_TEST(check.ok(), "error was: " << check.error);
        }
    }
}

BOOST_AUTO_TEST_CASE(strips_one_trailing_slash_only) {
    BOOST_TEST(validate_s3_endpoint("https://minio.internal:9000/").normalised ==
               "https://minio.internal:9000");
    BOOST_TEST(validate_s3_endpoint("http://localhost:4566").normalised == "http://localhost:4566");
}

BOOST_AUTO_TEST_CASE(scheme_is_required_and_limited) {
    require_rejected("localhost:4566", "needs a scheme");
    require_rejected("ftp://localhost:4566", "scheme must be http or https");
}

BOOST_AUTO_TEST_CASE(host_must_be_present_and_well_formed) {
    require_rejected("http://:4566", "host is empty");
    require_rejected("http://", "host is empty");
    require_rejected("http://::1:4566", "IPv6 literal must be enclosed");
    require_rejected("http://[::1:4566", "IPv6 literal must be enclosed");
    require_rejected("http://bad_host:9000", "not a DNS name");
}

BOOST_AUTO_TEST_CASE(port_must_be_in_range) {
    require_rejected("http://localhost:0", "from 1 to 65535");
    require_rejected("http://localhost:65536", "from 1 to 65535");
    require_rejected("http://localhost:45a", "must be a number");
    require_rejected("http://localhost:", "port after ':' is empty");
    BOOST_TEST(validate_s3_endpoint("http://localhost:1").ok());
    BOOST_TEST(validate_s3_endpoint("http://localhost:65535").ok());
}

BOOST_AUTO_TEST_CASE(path_query_and_fragment_are_rejected) {
    require_rejected("http://minio:9000/s3", "path is not accepted");
    require_rejected("http://minio:9000/?x=1", "query");
    require_rejected("http://minio:9000?x=1", "query");
    require_rejected("http://minio:9000#frag", "fragment");
}

BOOST_AUTO_TEST_CASE(userinfo_is_rejected_without_echoing_the_value) {
    const auto check = validate_s3_endpoint("http://admin:hunter2@minio:9000");
    BOOST_TEST(!check.ok());
    BOOST_TEST(contains(check.error, "--endpoint"));
    BOOST_TEST(contains(check.error, "userinfo"));
    BOOST_TEST(!contains(check.error, "hunter2"));
    BOOST_TEST(!contains(check.error, "admin"));
}

BOOST_AUTO_TEST_CASE(plaintext_to_a_non_loopback_host_warns) {
    const auto check = validate_s3_endpoint("http://minio.internal:9000");
    BOOST_TEST(check.ok());
    BOOST_REQUIRE_EQUAL(check.warnings.size(), 1U);
    BOOST_TEST(contains(check.warnings.front(), "unencrypted"));
}

BOOST_AUTO_TEST_CASE(plaintext_to_loopback_and_any_https_do_not_warn) {
    for (const char* endpoint :
         {"http://localhost:4566", "http://LOCALHOST:4566", "http://127.0.0.1:9000",
          "http://127.4.5.6", "http://[::1]:4566", "https://minio.internal:9000"}) {
        BOOST_TEST_CONTEXT("endpoint " << endpoint) {
            const auto check = validate_s3_endpoint(endpoint);
            BOOST_TEST(check.ok());
            BOOST_TEST(check.warnings.empty());
        }
    }
    // Spelled like loopback but not a valid IPv4 literal: warn.
    BOOST_TEST(validate_s3_endpoint("http://127.0.0.1.example.com").warnings.size() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ignored_endpoint_variable_warnings_suite)

BOOST_AUTO_TEST_CASE(neither_variable_set_gives_no_warning) {
    BOOST_TEST(ignored_endpoint_variable_warnings(map_lookup({})).empty());
}

BOOST_AUTO_TEST_CASE(each_variable_warns_by_name_and_never_by_value) {
    const std::string secret = "https://secret-host.example";
    for (const char* name : {"AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_S3"}) {
        BOOST_TEST_CONTEXT("variable " << name) {
            const auto warnings = ignored_endpoint_variable_warnings(map_lookup({{name, secret}}));
            BOOST_REQUIRE_EQUAL(warnings.size(), 1U);
            BOOST_TEST(warnings.front().starts_with(std::string(name) + " "));
            BOOST_TEST(contains(warnings.front(), "--endpoint"));
            BOOST_TEST(!contains(warnings.front(), "secret-host"));
        }
    }
    const auto both = ignored_endpoint_variable_warnings(
        map_lookup({{"AWS_ENDPOINT_URL", secret}, {"AWS_ENDPOINT_URL_S3", secret}}));
    BOOST_TEST(both.size() == 2U);
}

BOOST_AUTO_TEST_CASE(an_empty_value_counts_as_unset) {
    BOOST_TEST(ignored_endpoint_variable_warnings(
                   map_lookup({{"AWS_ENDPOINT_URL", ""}, {"AWS_ENDPOINT_URL_S3", ""}}))
                   .empty());
}

BOOST_AUTO_TEST_SUITE_END()
