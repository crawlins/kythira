// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The native KCL lease layer's server half
// (`.kiro/specs/kinesis-compatible-streams/` task 9, Requirement 15.1):
// lease records in the stream state machine (versions are log indexes,
// conditions, patch, list pages, snapshots) and the Lease API over them
// (SigV4, the DynamoDB actions on the application's table ARN, the
// conditional headers and the answers' shapes).

#define BOOST_TEST_MODULE kinesis_leases_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_lease_api.hpp>
#include <kythira/kinesis/kinesis_leases.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/sigv4_verifier.hpp>
#include <raft/static_key_authorizer.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin = kythira::kinesis;
namespace aj = kythira::aws_json;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

namespace {

constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
constexpr std::string_view k_account = "111122223333";
constexpr std::string_view k_other = "444455556666";
const auto k_now = sv4::detail::parse_amz_date("20261110T120000Z").value();

// ---- state machine level ---------------------------------------------------

struct machine {
    kin::stream_state_machine<> sm;
    std::uint64_t index = 0;

    auto run(const kin::stream_command& c) -> kin::apply_result {
        return kin::decode_result(sm.apply(kin::encode_command(c), ++index));
    }
};

auto put(std::string key, std::map<std::string, std::string> fields,
         kin::lease_condition cond = kin::lease_condition::any, std::uint64_t expected = 0)
    -> kin::lease_write_command {
    return {._account = std::string(k_account),
            ._application = "orders-app",
            ._key = std::move(key),
            ._op = kin::lease_op::put,
            ._condition = cond,
            ._expected_version = expected,
            ._fields = std::move(fields)};
}

auto list(std::string after = {}, std::string prefix = {}, std::uint32_t limit = 0)
    -> kin::lease_read_command {
    return {._account = std::string(k_account),
            ._application = "orders-app",
            ._kind = kin::lease_read_kind::list,
            ._key = std::move(after),
            ._prefix = std::move(prefix),
            ._limit = limit};
}

auto keys_of(const kin::apply_result& r) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& e : kin::decode_lease_page(r._payload)._leases) {
        out.push_back(e._key);
    }
    return out;
}

// ---- API level -------------------------------------------------------------

auto request(std::string method, std::string path, std::string body = {},
             std::vector<std::pair<std::string, std::string>> extra = {}, std::string query = {})
    -> kin::front_end_request {
    kin::front_end_request r;
    r._method = std::move(method);
    r._path = std::move(path);
    r._query = std::move(query);
    r._body = std::move(body);
    r._source_ip = "192.0.2.10";
    r._headers = {{"Host", "localhost:4567"}, {"X-Amz-Date", "20261110T120000Z"}};
    for (auto& h : extra) {
        r._headers.push_back(std::move(h));
    }
    return r;
}

// Signs `r` as `akid` (header form), as the Java lease layer's signer does.
auto sign(kin::front_end_request r, std::string_view akid) -> kin::front_end_request {
    sv4::service_rules rules;
    rules.service = "kinesis";
    rules.protocol = sv4::error_protocol::json;
    const sv4::credential_scope scope{"20261110", "us-east-1", "kinesis"};
    r._headers.emplace_back(
        "Authorization", "AWS4-HMAC-SHA256 Credential=" + std::string(akid) + "/" + scope.text() +
                             ", SignedHeaders=host;x-amz-date, Signature=" + std::string(64, '0'));
    const auto parsed =
        sv4::parse(sv4::request{r._method, r._path, r._query, r._headers, r._body}, rules, k_now);
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), parsed.error().message);
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
    r._headers.back().second.replace(r._headers.back().second.size() - 64, 64, sig);
    return r;
}

struct api_fixture {
    kin::local_stream_cluster cluster{nullptr, [] { return std::int64_t{1'762'776'000'000}; }};
    az::static_key_authorizer auth{std::vector<az::static_key>{
                                       {"AKIDREAD", std::string(k_secret), std::string(k_account),
                                        "reader", az::static_role::read_only},
                                       {"AKIDWRITE", std::string(k_secret), std::string(k_account),
                                        "worker", az::static_role::read_write},
                                       {"AKIDADMIN", std::string(k_secret), std::string(k_account),
                                        "admin", az::static_role::admin},
                                       {"AKIDOTHER", std::string(k_secret), std::string(k_other),
                                        "elsewhere", az::static_role::read_write},
                                   },
                                   "aws",
                                   [] { return std::chrono::system_clock::time_point(k_now); }};
    std::unique_ptr<kin::lease_api<kin::local_stream_cluster, az::static_key_authorizer>> api;

    api_fixture() {
        auth.register_service(kin::kinesis_static_roles());
        auth.register_service(kin::lease_static_roles());
        api =
            std::make_unique<kin::lease_api<kin::local_stream_cluster, az::static_key_authorizer>>(
                cluster, auth, kin::front_end_config{});
    }

    auto call(std::string method, std::string path, std::string body = {},
              std::vector<std::pair<std::string, std::string>> extra = {}, std::string query = {},
              std::string_view akid = "AKIDWRITE") -> aj::http_response {
        return api->handle(sign(request(std::move(method), std::move(path), std::move(body),
                                        std::move(extra), std::move(query)),
                                akid));
    }
};

auto body_of(const aj::http_response& r) -> json::object {
    return json::parse(r.body).as_object();
}

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(body_of(r).at("__type").as_string());
}

auto version_of(const aj::http_response& r) -> std::uint64_t {
    return body_of(r).at("version").to_number<std::uint64_t>();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(lease_records)

BOOST_AUTO_TEST_CASE(versions_are_log_indexes_and_never_repeat) {
    machine m;
    const auto a =
        m.run(put("shardId-0", {{"leaseOwner", R"("w1")"}}, kin::lease_condition::absent));
    BOOST_TEST((a._status == kin::apply_status::ok));
    BOOST_TEST(a._id == m.index);
    // Creating again fails and names the version there.
    const auto again = m.run(put("shardId-0", {}, kin::lease_condition::absent));
    BOOST_TEST((again._status == kin::apply_status::condition_failed));
    BOOST_TEST(again._id == a._id);
    // A stale version fails; the current one wins and moves the version.
    BOOST_TEST((m.run(put("shardId-0", {}, kin::lease_condition::version, a._id + 7))._status ==
                kin::apply_status::condition_failed));
    const auto b =
        m.run(put("shardId-0", {{"leaseOwner", R"("w2")"}}, kin::lease_condition::version, a._id));
    BOOST_TEST((b._status == kin::apply_status::ok));
    BOOST_TEST(b._id > a._id);
    // Deleted and created again: a fresh version, so a holder of the old
    // one cannot write the new lease.
    kin::lease_write_command del = put("shardId-0", {}, kin::lease_condition::version, b._id);
    del._op = kin::lease_op::remove;
    BOOST_TEST((m.run(del)._status == kin::apply_status::ok));
    BOOST_TEST(m.sm.find_lease(k_account, "orders-app", "shardId-0") == nullptr);
    const auto c = m.run(put("shardId-0", {}, kin::lease_condition::absent));
    BOOST_TEST(c._id > b._id);
    BOOST_TEST((m.run(put("shardId-0", {}, kin::lease_condition::version, b._id))._status ==
                kin::apply_status::condition_failed));
}

BOOST_AUTO_TEST_CASE(patch_sets_and_removes_fields_on_an_existing_lease) {
    machine m;
    kin::lease_write_command patch = put("shardId-1", {{"checkpoint", R"("TRIM_HORIZON")"}});
    patch._op = kin::lease_op::patch;
    BOOST_TEST((m.run(patch)._status == kin::apply_status::not_found));
    const auto v1 = m.run(put("shardId-1", {{"leaseOwner", R"("w1")"}, {"leaseCounter", "1"}}))._id;
    patch._removed = {"leaseOwner"};
    patch._condition = kin::lease_condition::version;
    patch._expected_version = v1;
    const auto r = m.run(patch);
    BOOST_TEST((r._status == kin::apply_status::ok));
    const auto* l = m.sm.find_lease(k_account, "orders-app", "shardId-1");
    BOOST_REQUIRE(l != nullptr);
    BOOST_TEST(l->_version == r._id);
    BOOST_TEST((l->_fields == std::map<std::string, std::string>{
                                  {"checkpoint", R"("TRIM_HORIZON")"}, {"leaseCounter", "1"}}));
    // Patch never creates, and refuses If-None-Match.
    patch._condition = kin::lease_condition::absent;
    BOOST_TEST((m.run(patch)._status == kin::apply_status::invalid));
}

BOOST_AUTO_TEST_CASE(lists_page_in_key_order_within_one_application) {
    machine m;
    for (const auto* k : {"b-2", "a-1", "b-1", "c-1", "b-3"}) {
        (void)m.run(put(k, {{"k", "1"}}));
    }
    // Another application and another account stay out of the list.
    auto foreign = put("a-0", {{"k", "1"}});
    foreign._application = "billing-app";
    (void)m.run(foreign);
    foreign._application = "orders-app";
    foreign._account = std::string(k_other);
    (void)m.run(foreign);

    const auto all = m.run(list());
    BOOST_TEST((keys_of(all) == std::vector<std::string>{"a-1", "b-1", "b-2", "b-3", "c-1"}));
    BOOST_TEST(!kin::decode_lease_page(all._payload)._more);
    const auto page1 = m.run(list({}, "b-", 2));
    BOOST_TEST((keys_of(page1) == std::vector<std::string>{"b-1", "b-2"}));
    BOOST_TEST(kin::decode_lease_page(page1._payload)._more);
    const auto page2 = m.run(list("b-2", "b-", 2));
    BOOST_TEST((keys_of(page2) == std::vector<std::string>{"b-3"}));
    BOOST_TEST(!kin::decode_lease_page(page2._payload)._more);
    // Reads change nothing.
    BOOST_TEST(m.sm.find_lease(k_account, "orders-app", "b-1")->_version == 3U);

    const kin::lease_drop_application_command drop{._account = std::string(k_account),
                                                   ._application = "orders-app"};
    BOOST_TEST(m.run(drop)._first_counter == 5U);
    BOOST_TEST(keys_of(m.run(list())).empty());
    BOOST_TEST(m.sm.find_lease(k_account, "billing-app", "a-0") != nullptr);
    BOOST_TEST(m.sm.find_lease(k_other, "orders-app", "a-0") != nullptr);
}

BOOST_AUTO_TEST_CASE(leases_survive_snapshots_and_commands_round_trip) {
    machine m;
    (void)m.run(
        put("shardId-7", {{"leaseOwner", R"("w1")"}, {"ownerSwitchesSinceCheckpoint", "0"}}));
    kin::stream_state_machine<> copy;
    copy.restore_from_snapshot(m.sm.get_state(), m.index);
    BOOST_TEST((copy.entries() == m.sm.entries()));

    kin::lease_write_command w = put("k", {{"a", "1"}}, kin::lease_condition::version, 9);
    w._op = kin::lease_op::patch;
    w._removed = {"b"};
    for (const kin::stream_command& c : std::vector<kin::stream_command>{
             w, list("x", "y", 3), kin::lease_drop_application_command{"1", "app"}}) {
        const auto bytes = kin::encode_command(c);
        BOOST_TEST((kin::encode_command(kin::decode_command(bytes)) == bytes));
    }
    // A read's answer crosses the forwarding hop with its payload.
    const kin::apply_result r{._id = 4, ._payload = kin::encode_lease_page({._more = true})};
    BOOST_TEST((kin::decode_result(kin::encode_result(r)) == r));
    // Every lease command routes to the catalog group.
    BOOST_TEST(kin::command_key(w).starts_with(kin::catalog_prefix()));
    BOOST_TEST(kin::command_key(list()).starts_with(kin::catalog_prefix()));
}

BOOST_AUTO_TEST_CASE(bad_names_and_oversized_leases_are_invalid) {
    machine m;
    auto bad = put("k", {{"a", "1"}});
    bad._application = "ab";
    BOOST_TEST((m.run(bad)._status == kin::apply_status::invalid));
    bad = put(std::string("a\nb"), {});
    BOOST_TEST((m.run(bad)._status == kin::apply_status::invalid));
    bad = put("k", {{"big", std::string(kin::k_max_lease_bytes, '1')}});
    BOOST_TEST((m.run(bad)._status == kin::apply_status::invalid));
    BOOST_TEST(m.sm.entries().empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(lease_api)

BOOST_AUTO_TEST_CASE(create_read_replace_and_delete_with_versions) {
    api_fixture f;
    const std::string path = "/v1/leases/orders-app/shardId-000000000000";
    const auto created =
        f.call("PUT", path, R"({"leaseOwner":"w1","leaseCounter":1})", {{"If-None-Match", "*"}});
    BOOST_TEST_REQUIRE(created.status == 200U, created.body);
    const auto v1 = version_of(created);
    BOOST_TEST(*created.header("ETag") == "\"" + std::to_string(v1) + "\"");

    const auto dup = f.call("PUT", path, "{}", {{"If-None-Match", "*"}});
    BOOST_TEST(dup.status == 412U);
    BOOST_TEST(type_of(dup) == "ConditionalCheckFailedException");
    BOOST_TEST(version_of(dup) == v1);

    const auto got = f.call("GET", path);
    BOOST_TEST_REQUIRE(got.status == 200U, got.body);
    const auto g = body_of(got);
    BOOST_TEST(g.at("key").as_string() == "shardId-000000000000");
    BOOST_TEST(g.at("version").to_number<std::uint64_t>() == v1);
    BOOST_TEST(g.at("lease").as_object().at("leaseOwner").as_string() == "w1");
    BOOST_TEST(g.at("lease").as_object().at("leaseCounter").as_int64() == 1);

    BOOST_TEST(f.call("PUT", path, "{}", {{"If-Match", "\"999\""}}).status == 412U);
    const auto replaced =
        f.call("PUT", path, R"({"leaseOwner":"w2"})", {{"If-Match", std::to_string(v1)}});
    BOOST_TEST_REQUIRE(replaced.status == 200U, replaced.body);
    const auto v2 = version_of(replaced);
    BOOST_TEST(v2 > v1);

    const auto patched = f.call("PATCH", path, R"({"checkpoint":"LATEST","leaseOwner":null})",
                                {{"If-Match", "W/\"" + std::to_string(v2) + "\""}});
    BOOST_TEST_REQUIRE(patched.status == 200U, patched.body);
    const auto lease = body_of(f.call("GET", path)).at("lease").as_object();
    BOOST_TEST(lease.size() == 1U);
    BOOST_TEST(lease.at("checkpoint").as_string() == "LATEST");

    BOOST_TEST(f.call("DELETE", path, {}, {{"If-Match", std::to_string(v2)}}).status == 412U);
    BOOST_TEST(f.call("DELETE", path).status == 200U);
    const auto gone = f.call("GET", path);
    BOOST_TEST(gone.status == 404U);
    BOOST_TEST(type_of(gone) == "ResourceNotFoundException");
    BOOST_TEST(f.call("PATCH", path, "{}").status == 404U);
}

BOOST_AUTO_TEST_CASE(lists_page_and_keys_are_percent_decoded) {
    api_fixture f;
    // A multi-stream lease key carries colons; one with a slash and a space
    // shows the key is the whole decoded rest of the path.
    for (const auto* k : {"111122223333%3Aorders%3A1%3AshardId-0", "a%2Fb%20c", "z-last"}) {
        BOOST_TEST_REQUIRE(
            f.call("PUT", std::string("/v1/leases/orders-app/") + k, R"({"n":1})").status == 200U);
    }
    const auto page = f.call("GET", "/v1/leases/orders-app/", {}, {}, "limit=2");
    BOOST_TEST_REQUIRE(page.status == 200U, page.body);
    const auto p = body_of(page);
    const auto& leases = p.at("leases").as_array();
    BOOST_TEST_REQUIRE(leases.size() == 2U);
    BOOST_TEST(leases[0].as_object().at("key").as_string() == "111122223333:orders:1:shardId-0");
    BOOST_TEST(leases[1].as_object().at("key").as_string() == "a/b c");
    BOOST_TEST(p.at("nextStartAfter").as_string() == "a/b c");
    const auto rest =
        body_of(f.call("GET", "/v1/leases/orders-app/", {}, {}, "limit=2&start-after=a%2Fb%20c"));
    BOOST_TEST(rest.at("leases").as_array().size() == 1U);
    BOOST_TEST(!rest.contains("nextStartAfter"));
    const auto prefixed = body_of(f.call("GET", "/v1/leases/orders-app/", {}, {}, "prefix=z-"));
    BOOST_TEST(prefixed.at("leases").as_array().size() == 1U);

    // Another account sees its own, empty, application of the same name.
    const auto theirs = body_of(f.call("GET", "/v1/leases/orders-app/", {}, {}, {}, "AKIDOTHER"));
    BOOST_TEST(theirs.at("leases").as_array().empty());

    // Deleting the application is admin's.
    BOOST_TEST(f.call("DELETE", "/v1/leases/orders-app/").status == 403U);
    const auto dropped = f.call("DELETE", "/v1/leases/orders-app/", {}, {}, {}, "AKIDADMIN");
    BOOST_TEST_REQUIRE(dropped.status == 200U, dropped.body);
    BOOST_TEST(body_of(dropped).at("deleted").as_int64() == 3);
}

BOOST_AUTO_TEST_CASE(dynamodb_actions_on_the_application_table_authorize) {
    api_fixture f;
    const std::string path = "/v1/leases/orders-app/shardId-1";
    BOOST_TEST(f.call("PUT", path, "{}").status == 200U);
    // A read-only key reads and lists but cannot write.
    BOOST_TEST(f.call("GET", path, {}, {}, {}, "AKIDREAD").status == 200U);
    BOOST_TEST(f.call("GET", "/v1/leases/orders-app/", {}, {}, {}, "AKIDREAD").status == 200U);
    const auto denied = f.call("PUT", path, "{}", {}, {}, "AKIDREAD");
    BOOST_TEST(denied.status == 403U);
    BOOST_TEST(type_of(denied) == "AccessDeniedException");
    BOOST_TEST(std::string(body_of(denied).at("message").as_string()) ==
               "User: arn:aws:iam::111122223333:user/reader is not authorized to perform: "
               "dynamodb:PutItem on resource: "
               "arn:aws:dynamodb:us-east-1:111122223333:table/orders-app");
    // Unsigned: the verifier's answer, before anything else.
    const auto unsigned_r = f.api->handle(request("GET", path));
    BOOST_TEST(unsigned_r.status == 400U);
    BOOST_TEST(type_of(unsigned_r) != "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(malformed_requests_are_refused) {
    api_fixture f;
    const std::string path = "/v1/leases/orders-app/k";
    BOOST_TEST(type_of(f.call("PUT", "/v1/leases/ab/k", "{}")) == "ValidationException");
    BOOST_TEST(f.call("PUT", "/v1/leases/orders-app/a%0Ab", "{}").status == 400U);
    BOOST_TEST(f.call("PUT", "/v1/leases/orders-app/a%zz", "{}").status == 400U);
    BOOST_TEST(f.call("PUT", path, "[1]").status == 400U);
    BOOST_TEST(f.call("PUT", path, "{not json").status == 400U);
    BOOST_TEST(f.call("PUT", path, "{}", {{"If-Match", "1"}, {"If-None-Match", "*"}}).status ==
               400U);
    BOOST_TEST(f.call("PUT", path, "{}", {{"If-Match", "one"}}).status == 400U);
    BOOST_TEST(f.call("PATCH", path, "{}", {{"If-None-Match", "*"}}).status == 400U);
    BOOST_TEST(f.call("POST", path, "{}").status == 405U);
    BOOST_TEST(f.call("PUT", "/v1/leases/orders-app/", "{}").status == 405U);
    BOOST_TEST(f.call("GET", "/v1/leases/orders-app/", {}, {}, "limit=0").status == 400U);
    BOOST_TEST(f.call("GET", "/v1/leases/orders-app/", {}, {}, "limit=1001").status == 400U);
    const std::string big = R"({"x":")" + std::string(kin::k_max_lease_bytes, 'a') + R"("})";
    BOOST_TEST(f.call("PUT", path, big).status == 400U);
}

BOOST_AUTO_TEST_SUITE_END()
