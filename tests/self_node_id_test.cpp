// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 6 of the cloud-composite-node-ids spec: self_node_id reads a
// node's own composite id from each of the five metadata services. Every case
// runs against a cpp-httplib fake on loopback that enforces the provider's
// request headers the way the real service does, so a lookup that forgot the
// IMDSv2 token or the Metadata header fails here exactly as it would on the
// cloud.

#define BOOST_TEST_MODULE self_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/self_node_id.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace kythira;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view k_aws_token = "AQAEAFake-imdsv2-token==";
constexpr std::string_view k_ali_token = "fake-aliyun-token";
constexpr std::string_view k_ocid =
    "ocid1.instance.oc1.iad.anuwcljrabcdefghijklmnopqrstuvwxyz0123456789";

/// A metadata server fake on an ephemeral loopback port.
struct fake_metadata {
    httplib::Server server;
    std::thread thread;
    int port{0};
    std::atomic<int> requests{0};
    std::vector<std::string> paths;
    std::mutex paths_mu;

    fake_metadata() {
        server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response&) {
            ++requests;
            std::lock_guard lock(paths_mu);
            paths.push_back(req.method + " " + req.path);
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }

    [[nodiscard]] auto options(std::chrono::milliseconds timeout = 2000ms) const
        -> self_node_id_options {
        return {.host = "127.0.0.1", .port = static_cast<std::uint16_t>(port), .timeout = timeout};
    }

    [[nodiscard]] auto seen() -> std::vector<std::string> {
        std::lock_guard lock(paths_mu);
        return paths;
    }

    ~fake_metadata() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    fake_metadata(const fake_metadata&) = delete;
    auto operator=(const fake_metadata&) -> fake_metadata& = delete;
    fake_metadata(fake_metadata&&) = delete;
    auto operator=(fake_metadata&&) -> fake_metadata& = delete;
};

/// IMDSv2 as AWS serves it: a PUT for a token carrying a TTL header, then
/// reads that are refused (401) without that token.
void serve_imdsv2(fake_metadata& fake, std::string instance_id, std::string region,
                  std::string token_ttl_header = "X-aws-ec2-metadata-token-ttl-seconds",
                  std::string token_header = "X-aws-ec2-metadata-token",
                  std::string token = std::string(k_aws_token),
                  std::string region_path = "/latest/meta-data/placement/region") {
    fake.server.Put("/latest/api/token", [=](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value(token_ttl_header).empty()) {
            res.status = 400;
            return;
        }
        res.set_content(token, "text/plain");
    });
    auto guarded = [=](std::string body) {
        return [=](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value(token_header) != token) {
                res.status = 401;
                return;
            }
            res.set_content(body, "text/plain");
        };
    };
    fake.server.Get("/latest/meta-data/instance-id", guarded(std::move(instance_id)));
    fake.server.Get(region_path, guarded(std::move(region)));
}

void serve_azure(fake_metadata& fake, std::string body) {
    fake.server.Get("/metadata/instance/compute",
                    [body = std::move(body)](const httplib::Request& req, httplib::Response& res) {
                        if (req.get_header_value("Metadata") != "true" ||
                            req.get_param_value("api-version").empty()) {
                            res.status = 400;
                            return;
                        }
                        res.set_content(body, "application/json");
                    });
}

void serve_gcp(fake_metadata& fake, std::string name, std::string zone, std::string project,
               bool flavor_header = true) {
    auto field = [=](std::string body) {
        return [=](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value("Metadata-Flavor") != "Google") {
                res.status = 403;
                return;
            }
            if (flavor_header) {
                res.set_header("Metadata-Flavor", "Google");
            }
            res.set_content(body, "application/text");
        };
    };
    fake.server.Get("/computeMetadata/v1/instance/name", field(std::move(name)));
    fake.server.Get("/computeMetadata/v1/instance/zone", field(std::move(zone)));
    fake.server.Get("/computeMetadata/v1/project/project-id", field(std::move(project)));
}

void serve_oci(fake_metadata& fake, std::string ocid) {
    fake.server.Get("/opc/v2/instance/id",
                    [ocid = std::move(ocid)](const httplib::Request& req, httplib::Response& res) {
                        if (req.get_header_value("Authorization") != "Bearer Oracle") {
                            res.status = 401;
                            return;
                        }
                        res.set_content(ocid, "text/plain");
                    });
}

}  // namespace

// ── names ────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(source_names_round_trip) {
    for (auto source : {node_id_source::aws_ec2, node_id_source::alibaba_ecs,
                        node_id_source::azure_vm, node_id_source::gcp, node_id_source::oci}) {
        BOOST_CHECK(parse_node_id_source(node_id_source_name(source)) == source);
    }
    BOOST_CHECK(parse_node_id_source("aws") == node_id_source::aws_ec2);
    BOOST_CHECK(parse_node_id_source("alibaba") == node_id_source::alibaba_ecs);
    BOOST_CHECK(parse_node_id_source("azure") == node_id_source::azure_vm);
    for (const char* bad : {"", "AWS", "ec2", "docker", "gce", "aws ", "metadata:aws"}) {
        BOOST_CHECK_MESSAGE(!parse_node_id_source(bad), "accepted '" << bad << "'");
    }
}

// ── success, one per provider ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(aws_imdsv2_token_then_reads) {
    fake_metadata fake;
    serve_imdsv2(fake, "i-0123456789abcdef0", "us-east-1");
    fake.start();

    auto id = self_node_id_text(node_id_source::aws_ec2, fake.options());
    BOOST_REQUIRE_MESSAGE(id.has_value(), id.error());
    BOOST_CHECK_EQUAL(*id, "aws-ec2:us-east-1:i-0123456789abcdef0");

    // The token came first; nothing was read without one.
    const auto seen = fake.seen();
    BOOST_REQUIRE_EQUAL(seen.size(), 3u);
    BOOST_CHECK_EQUAL(seen[0], "PUT /latest/api/token");
    BOOST_CHECK_EQUAL(seen[1], "GET /latest/meta-data/instance-id");
    BOOST_CHECK_EQUAL(seen[2], "GET /latest/meta-data/placement/region");

    auto typed = self_node_id<aws_ec2_node_id>(node_id_source::aws_ec2, fake.options());
    BOOST_REQUIRE_MESSAGE(typed.has_value(), typed.error());
    BOOST_CHECK_EQUAL(typed->native(), "i-0123456789abcdef0");
    BOOST_CHECK_EQUAL(typed->scope(), "us-east-1");
}

BOOST_AUTO_TEST_CASE(aws_legacy_eight_digit_id_and_trailing_newline) {
    fake_metadata fake;
    serve_imdsv2(fake, "i-1234abcd\n", "us-gov-west-1\n");
    fake.start();
    auto id = self_node_id_text(node_id_source::aws_ec2, fake.options());
    BOOST_REQUIRE_MESSAGE(id.has_value(), id.error());
    BOOST_CHECK_EQUAL(*id, "aws-ec2:us-gov-west-1:i-1234abcd");
}

BOOST_AUTO_TEST_CASE(alibaba_token_then_reads) {
    fake_metadata fake;
    serve_imdsv2(fake, "i-bp67acfmxazb4ph2xyz", "cn-hangzhou",
                 "X-aliyun-ecs-metadata-token-ttl-seconds", "X-aliyun-ecs-metadata-token",
                 std::string(k_ali_token), "/latest/meta-data/region-id");
    fake.start();
    auto id = self_node_id_text(node_id_source::alibaba_ecs, fake.options());
    BOOST_REQUIRE_MESSAGE(id.has_value(), id.error());
    BOOST_CHECK_EQUAL(*id, "alibaba-ecs:cn-hangzhou:i-bp67acfmxazb4ph2xyz");
}

BOOST_AUTO_TEST_CASE(azure_compute_document_folds_case) {
    fake_metadata fake;
    serve_azure(fake, R"({"name":"Kythira-Node-1","resourceGroupName":"Prod-RG",)"
                      R"("location":"eastus","vmId":"0b1f7e2c-0000-0000-0000-000000000000"})");
    fake.start();
    auto id = self_node_id_text(node_id_source::azure_vm, fake.options());
    BOOST_REQUIRE_MESSAGE(id.has_value(), id.error());
    BOOST_CHECK_EQUAL(*id, "azure-vm:prod-rg:kythira-node-1");
}

BOOST_AUTO_TEST_CASE(gcp_reads_name_zone_and_project) {
    fake_metadata fake;
    serve_gcp(fake, "kythira-a-0", "projects/123456789/zones/us-central1-a", "example.com:proj");
    fake.start();
    auto id = self_node_id_text(node_id_source::gcp, fake.options());
    BOOST_REQUIRE_MESSAGE(id.has_value(), id.error());
    // The domain-scoped project's ':' is percent-encoded in canonical text.
    BOOST_CHECK_EQUAL(*id, "gcp:example.com%3Aproj/us-central1-a:kythira-a-0");
    auto typed = self_node_id<gcp_instance_node_id>(node_id_source::gcp, fake.options());
    BOOST_REQUIRE_MESSAGE(typed.has_value(), typed.error());
    BOOST_CHECK_EQUAL(typed->scope(), "example.com:proj/us-central1-a");
}

BOOST_AUTO_TEST_CASE(oci_reads_the_instance_ocid) {
    fake_metadata fake;
    serve_oci(fake, std::string(k_ocid));
    fake.start();
    auto id = self_node_id_text(node_id_source::oci, fake.options());
    BOOST_REQUIRE_MESSAGE(id.has_value(), id.error());
    BOOST_CHECK_EQUAL(*id, "oci:iad:" + std::string(k_ocid));
}

// ── IMDSv2 token flow failures ───────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(aws_token_refused_is_an_error_without_a_v1_fallback) {
    fake_metadata fake;
    fake.server.Put("/latest/api/token",
                    [](const httplib::Request&, httplib::Response& res) { res.status = 403; });
    // An IMDSv1 server would answer these without a token; the lookup must
    // not ask.
    fake.server.Get("/latest/meta-data/instance-id",
                    [](const httplib::Request&, httplib::Response& res) {
                        res.set_content("i-0123456789abcdef0", "text/plain");
                    });
    fake.start();
    auto id = self_node_id_text(node_id_source::aws_ec2, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("PUT /latest/api/token: HTTP 403"), std::string::npos);
    BOOST_CHECK_NE(id.error().find("metadata:aws-ec2"), std::string::npos);
    BOOST_CHECK_EQUAL(fake.requests.load(), 1);
}

BOOST_AUTO_TEST_CASE(aws_empty_token_is_an_error) {
    fake_metadata fake;
    serve_imdsv2(fake, "i-0123456789abcdef0", "us-east-1", "X-aws-ec2-metadata-token-ttl-seconds",
                 "X-aws-ec2-metadata-token", "");
    fake.start();
    auto id = self_node_id_text(node_id_source::aws_ec2, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("empty token"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(aws_read_refused_with_the_token_is_an_error) {
    fake_metadata fake;
    // A server whose token does not match the one it hands out: every read
    // is refused, as an expired token would be.
    fake.server.Put("/latest/api/token", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("stale", "text/plain");
    });
    fake.server.Get("/latest/meta-data/instance-id",
                    [](const httplib::Request&, httplib::Response& res) { res.status = 401; });
    fake.start();
    auto id = self_node_id_text(node_id_source::aws_ec2, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("GET /latest/meta-data/instance-id: HTTP 401"),
                   std::string::npos);
}

// ── malformed answers ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(aws_invalid_instance_id_is_an_error) {
    for (const char* bad : {"", "x-0123", "i-0123456789ABCDEF0", "i-01234", "<html>oops</html>"}) {
        fake_metadata fake;
        serve_imdsv2(fake, bad, "us-east-1");
        fake.start();
        auto id = self_node_id_text(node_id_source::aws_ec2, fake.options());
        BOOST_CHECK_MESSAGE(!id.has_value(), "accepted '" << bad << "'");
        BOOST_CHECK_NE(id.error().find("not a valid id"), std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(azure_malformed_documents_are_errors) {
    for (const char* body :
         {"", "not json", "[]", R"({"name":"vm1"})", R"({"resourceGroupName":"rg"})",
          R"({"name":7,"resourceGroupName":"rg"})",
          R"({"name":"bad name","resourceGroupName":"rg"})"}) {
        fake_metadata fake;
        serve_azure(fake, body);
        fake.start();
        auto id = self_node_id_text(node_id_source::azure_vm, fake.options());
        BOOST_CHECK_MESSAGE(!id.has_value(), "accepted '" << body << "'");
    }
}

BOOST_AUTO_TEST_CASE(gcp_answer_without_the_flavor_header_is_refused) {
    fake_metadata fake;
    serve_gcp(fake, "kythira-a-0", "projects/1/zones/us-central1-a", "proj", false);
    fake.start();
    auto id = self_node_id_text(node_id_source::gcp, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("not the metadata server"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(gcp_malformed_zone_is_an_error) {
    for (const char* zone : {"us-central1-a", "projects/1/zones/", "zones/us-central1-a"}) {
        fake_metadata fake;
        serve_gcp(fake, "kythira-a-0", zone, "proj");
        fake.start();
        auto id = self_node_id_text(node_id_source::gcp, fake.options());
        BOOST_CHECK_MESSAGE(!id.has_value(), "accepted zone '" << zone << "'");
    }
}

BOOST_AUTO_TEST_CASE(oci_non_instance_ocid_is_an_error) {
    for (const char* ocid : {"", "ocid1.volume.oc1.iad.abc", "not-an-ocid"}) {
        fake_metadata fake;
        serve_oci(fake, ocid);
        fake.start();
        auto id = self_node_id_text(node_id_source::oci, fake.options());
        BOOST_CHECK_MESSAGE(!id.has_value(), "accepted '" << ocid << "'");
    }
}

BOOST_AUTO_TEST_CASE(http_error_status_is_an_error) {
    fake_metadata fake;
    fake.server.Get("/opc/v2/instance/id",
                    [](const httplib::Request&, httplib::Response& res) { res.status = 404; });
    fake.start();
    auto id = self_node_id_text(node_id_source::oci, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("HTTP 404"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(oversized_body_is_an_error) {
    fake_metadata fake;
    serve_oci(fake, std::string(200U * 1024U, 'a'));
    fake.start();
    auto id = self_node_id_text(node_id_source::oci, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("larger than"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(typed_lookup_refuses_another_providers_id) {
    fake_metadata fake;
    serve_oci(fake, std::string(k_ocid));
    fake.start();
    auto id = self_node_id<aws_ec2_node_id>(node_id_source::oci, fake.options());
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("not an id of the requested node id type"), std::string::npos);
    BOOST_CHECK_THROW((void)id.value(), std::logic_error);
}

// ── bounded time ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_silent_server_times_out_within_the_budget) {
    fake_metadata fake;
    fake.server.Get("/opc/v2/instance/id", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(1500ms);
        res.set_content(std::string(k_ocid), "text/plain");
    });
    fake.start();
    const auto started = std::chrono::steady_clock::now();
    auto id = self_node_id_text(node_id_source::oci, fake.options(300ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_MESSAGE(id.error().find("no answer within 300 ms") != std::string::npos,
                        id.error());
    BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1200);
}

BOOST_AUTO_TEST_CASE(the_budget_covers_every_request_of_a_lookup) {
    // Each read is quick, but together they exceed the budget: the deadline
    // is one for the lookup, not one per request.
    fake_metadata fake;
    auto slow = [](std::string body) {
        return [body = std::move(body)](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(200ms);
            res.set_header("Metadata-Flavor", "Google");
            res.set_content(body, "application/text");
        };
    };
    fake.server.Get("/computeMetadata/v1/instance/name", slow("kythira-a-0"));
    fake.server.Get("/computeMetadata/v1/instance/zone", slow("projects/1/zones/us-central1-a"));
    fake.server.Get("/computeMetadata/v1/project/project-id", slow("proj"));
    fake.start();
    auto id = self_node_id_text(node_id_source::gcp, fake.options(450ms));
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_NE(id.error().find("no answer within 450 ms"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(nothing_listening_fails_fast) {
    int port = 0;
    {
        // Bind and release a port so nothing is listening on it.
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
    }
    const auto started = std::chrono::steady_clock::now();
    auto id = self_node_id_text(
        node_id_source::azure_vm,
        {.host = "127.0.0.1", .port = static_cast<std::uint16_t>(port), .timeout = 2000ms});
    BOOST_REQUIRE(!id.has_value());
    BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started)
                       .count(),
                   2500);
}
