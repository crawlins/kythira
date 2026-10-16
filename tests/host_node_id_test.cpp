// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 6 of the cloud-composite-node-ids spec, the host side: the
// `host_node_id` parsers every cmd/ binary reads its own id and its peers' ids
// with. The parsers are templates, so both id kinds are tested here in one
// binary whichever KYTHIRA_NODE_ID_KIND this build selected; the last case
// pins the alias itself to the selected kind.

#define BOOST_TEST_MODULE host_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/host_node_id.hpp>

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>

using namespace kythira;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view k_aws_id = "aws-ec2:us-east-1:i-0123456789abcdef0";
constexpr std::string_view k_ocid =
    "ocid1.instance.oc1.iad.anuwcljrabcdefghijklmnopqrstuvwxyz0123456789";

/// An OCI IMDS v2 fake on an ephemeral loopback port: the cheapest provider
/// to stand up, and enough to show `metadata:` is routed to `self_node_id`.
struct fake_oci {
    httplib::Server server;
    std::thread thread;
    int port{0};

    fake_oci() {
        server.Get("/opc/v2/instance/id", [](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value("Authorization") != "Bearer Oracle") {
                res.status = 401;
                return;
            }
            res.set_content(std::string(k_ocid), "text/plain");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    [[nodiscard]] auto options() const -> self_node_id_options {
        return {.host = "127.0.0.1", .port = static_cast<std::uint16_t>(port), .timeout = 2000ms};
    }

    ~fake_oci() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    fake_oci(const fake_oci&) = delete;
    auto operator=(const fake_oci&) -> fake_oci& = delete;
    fake_oci(fake_oci&&) = delete;
    auto operator=(fake_oci&&) -> fake_oci& = delete;
};

}  // namespace

BOOST_AUTO_TEST_CASE(numeric_accepts_only_decimal) {
    auto id = parse_node_id_text<std::uint64_t>("42");
    BOOST_REQUIRE(id);
    BOOST_CHECK_EQUAL(*id, 42U);
    for (const std::string_view bad : std::initializer_list<std::string_view>{
             "", "-1", "4x", "18446744073709551616", k_aws_id}) {
        BOOST_CHECK_MESSAGE(!parse_node_id_text<std::uint64_t>(bad), "accepted '" << bad << "'");
    }
    const auto refused = parse_node_id_text<std::uint64_t>(k_aws_id);
    BOOST_CHECK_NE(refused.error().find("KYTHIRA_NODE_ID_KIND=numeric"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(text_accepts_canonical_text_and_normalised_decimal) {
    auto aws = parse_node_id_text<std::string>(k_aws_id);
    BOOST_REQUIRE(aws);
    BOOST_CHECK_EQUAL(*aws, k_aws_id);

    // Two spellings of one number must not name two nodes.
    auto seven = parse_node_id_text<std::string>("007");
    BOOST_REQUIRE(seven);
    BOOST_CHECK_EQUAL(*seven, "7");

    for (const std::string_view bad :
         {"", "node-1", "aws-ec2:us-east-1", "nosuch:scope:native", "18446744073709551616"}) {
        BOOST_CHECK_MESSAGE(!parse_node_id_text<std::string>(bad), "accepted '" << bad << "'");
    }
}

BOOST_AUTO_TEST_CASE(any_composite_splits_scope_and_native) {
    auto parts = parse_any_composite_node_id(k_aws_id);
    BOOST_REQUIRE(parts);
    BOOST_CHECK_EQUAL(parts->first, "us-east-1");
    BOOST_CHECK_EQUAL(parts->second, "i-0123456789abcdef0");
    BOOST_CHECK(!parse_any_composite_node_id("12"));
}

BOOST_AUTO_TEST_CASE(metadata_resolves_in_a_text_build) {
    fake_oci fake;
    auto id = parse_local_node_id<std::string>("metadata:oci", fake.options());
    BOOST_REQUIRE_MESSAGE(id, (id ? std::string{} : id.error()));
    BOOST_CHECK_EQUAL(*id, "oci:iad:" + std::string(k_ocid));
}

BOOST_AUTO_TEST_CASE(metadata_is_refused_in_a_numeric_build_without_asking) {
    fake_oci fake;
    auto id = parse_local_node_id<std::uint64_t>("metadata:oci", fake.options());
    BOOST_REQUIRE(!id);
    BOOST_CHECK_NE(id.error().find("KYTHIRA_NODE_ID_KIND=text"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(metadata_unknown_provider_is_refused) {
    for (const std::string_view bad : {"metadata:", "metadata:ibm", "metadata:AWS"}) {
        auto id = parse_local_node_id<std::string>(bad);
        BOOST_REQUIRE_MESSAGE(!id, "accepted '" << bad << "'");
        BOOST_CHECK_NE(id.error().find("unknown metadata provider"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(local_id_without_prefix_parses_as_text) {
    auto n = parse_local_node_id<std::uint64_t>("3");
    BOOST_REQUIRE(n);
    BOOST_CHECK_EQUAL(*n, 3U);
    auto t = parse_local_node_id<std::string>(k_aws_id);
    BOOST_REQUIRE(t);
    BOOST_CHECK_EQUAL(*t, k_aws_id);
}

BOOST_AUTO_TEST_CASE(split_leading_node_id_handles_both_kinds) {
    auto numeric = split_leading_node_id("2:kythira-2:7000");
    BOOST_REQUIRE(numeric);
    BOOST_CHECK_EQUAL(numeric->first, "2");
    BOOST_CHECK_EQUAL(numeric->second, "kythira-2:7000");

    const std::string entry = std::string(k_aws_id) + ":10.0.0.5:7000";
    auto text = split_leading_node_id(entry);
    BOOST_REQUIRE(text);
    BOOST_CHECK_EQUAL(text->first, k_aws_id);
    BOOST_CHECK_EQUAL(text->second, "10.0.0.5:7000");

    BOOST_CHECK(!split_leading_node_id("2"));
    BOOST_CHECK(!split_leading_node_id("aws-ec2:us-east-1"));
    BOOST_CHECK(!split_leading_node_id("aws-ec2:us-east-1:i-0"));
}

BOOST_AUTO_TEST_CASE(node_id_json_quotes_only_text) {
    BOOST_CHECK_EQUAL(node_id_json(std::uint64_t{17}), "17");
    BOOST_CHECK_EQUAL(node_id_json(std::string(k_aws_id)), "\"" + std::string(k_aws_id) + "\"");
    BOOST_CHECK_EQUAL(node_id_json(std::string("a\"b\\c\n")), R"("a\"b\\c\u000a")");
}

BOOST_AUTO_TEST_CASE(host_alias_matches_the_configured_kind) {
#if defined(KYTHIRA_NODE_ID_TEXT)
    BOOST_CHECK(host_node_id_is_text);
    BOOST_CHECK_EQUAL(host_node_id_kind, "text");
    BOOST_CHECK_EQUAL(host_node_id_from_number(5), "5");
#else
    BOOST_CHECK(!host_node_id_is_text);
    BOOST_CHECK_EQUAL(host_node_id_kind, "numeric");
    BOOST_CHECK_EQUAL(host_node_id_from_number(5), 5U);
#endif
    BOOST_CHECK_EQUAL(host_node_id_text(host_node_id_from_number(5)), "5");
}
