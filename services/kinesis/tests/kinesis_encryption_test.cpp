// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Encryption at rest (`.kiro/specs/kinesis-compatible-streams/` task 14,
// Requirement 14.1): the local key provider, sealed records, the AWS KMS
// adapter against a fake KMS, and Start/StopStreamEncryption through the
// front end on the in-process cluster, with the records on disk checked to
// be ciphertext and read back by a second gateway that shares only the
// passphrase, and the encryption entries and data keys round-tripping.

#define BOOST_TEST_MODULE kinesis_encryption_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_kms.hpp>
#include <kythira/kinesis/kinesis_kms_aws.hpp>
#include <kythira/kinesis/kinesis_service.hpp>

#include <raft/record_segment_store.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin = kythira::kinesis;
namespace aj = kythira::aws_json;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

namespace {

constexpr std::string_view k_account = "111122223333";
constexpr std::string_view k_secret_text = "sealed-payload-0123456789";

struct temp_dir {
    std::filesystem::path _path;
    temp_dir() {
        static int counter = 0;
        _path = std::filesystem::temp_directory_path() /
                ("kinesis_encryption_test." + std::to_string(::getpid()) + "." +
                 std::to_string(++counter));
        std::filesystem::remove_all(_path);
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
};

struct allow_all {
    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::" + std::string(k_account) + ":user/alice";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request&) -> az::authz_decision {
        az::authz_decision d;
        d.allowed = true;
        return d;
    }
};

auto bytes_of(std::string_view s) -> std::vector<std::byte> {
    const auto b = std::as_bytes(std::span(s.data(), s.size()));
    return {b.begin(), b.end()};
}

auto config_with(kin::key_provider kms) -> kin::kinesis_service_config {
    kin::kinesis_service_config c;
    // Every gateway of a cluster shares the iterator ring.
    c._iterator_keys._keys.push_back({._id = 0, ._secret = std::string(32, 'k')});
    c._kms = std::move(kms);
    return c;
}

/// One gateway: a service and its front end over the shared cluster.
struct gateway {
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    std::unique_ptr<kin::kinesis_service<kin::local_stream_cluster>> service;

    gateway(kin::local_stream_cluster& cluster, kin::key_provider kms) {
        service = std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(
            cluster, config_with(std::move(kms)));
        service->register_with(fe);
    }

    auto call(std::string_view op, const json::value& body) -> aj::http_response {
        kin::front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._body = json::serialize(body);
        r._headers = {{"Content-Type", "application/x-amz-json-1.1"},
                      {"X-Amz-Target", "Kinesis_20131202." + std::string(op)}};
        return fe.handle(r);
    }

    auto ok(std::string_view op, const json::value& body) -> json::object {
        const auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, std::string(op) + ": " + r.body);
        return json::parse(r.body).as_object();
    }

    // Every record of single-shard stream `stream`: (partition key, data
    // as base64, encryption type).
    auto read_all(std::string_view stream) -> std::vector<std::vector<std::string>> {
        const auto it = ok("GetShardIterator", {{"StreamName", stream},
                                                {"ShardId", "shardId-000000000000"},
                                                {"ShardIteratorType", "TRIM_HORIZON"}});
        const auto got = ok("GetRecords", {{"ShardIterator", it.at("ShardIterator")}});
        std::vector<std::vector<std::string>> out;
        for (const auto& v : got.at("Records").as_array()) {
            const auto& o = v.as_object();
            const auto* e = o.if_contains("EncryptionType");
            out.push_back({std::string(o.at("PartitionKey").as_string()),
                           std::string(o.at("Data").as_string()),
                           e != nullptr ? std::string(e->as_string()) : std::string("NONE")});
        }
        return out;
    }
};

struct fixture {
    temp_dir dir;
    std::unique_ptr<kythira::record_segment_store> store =
        std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
            ._data_dir = dir._path, ._barriers_enabled = false});
    std::int64_t now = 1'000'000;
    kin::local_stream_cluster cluster{store.get(), [this] { return now; }};
    gateway gw{cluster, kin::key_provider(kin::local_kms_provider("correct horse battery"))};

    // Whether `needle` appears anywhere in the segment files.
    [[nodiscard]] auto on_disk(std::string_view needle) const -> bool {
        for (const auto& e : std::filesystem::recursive_directory_iterator(dir._path)) {
            if (!e.is_regular_file()) {
                continue;
            }
            std::ifstream in(e.path(), std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            if (content.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    auto record(std::string_view stream) -> kin::stream_record {
        std::optional<kin::stream_record> out;
        (void)cluster.read("", [&](const auto& sm) {
            if (const auto* s = sm.find_stream(k_account, stream)) {
                out = *s;
            }
        });
        BOOST_REQUIRE(out.has_value());
        return *out;
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

auto b64(std::string_view s) -> std::string {
    return kin::kms_aws_detail::base64_encode(s);
}

/// A provider that fails with `failure` (to check the error mapping).
struct failing_provider {
    kin::kms_failure _failure;
    auto generate_data_key(const std::string&, const kin::encryption_context&)
        -> kin::generated_data_key {
        throw kin::kms_error(_failure, "refused");
    }
    auto decrypt_data_key(const std::string&, const std::string&, const kin::encryption_context&)
        -> kin::data_key_bytes {
        throw kin::kms_error(_failure, "refused");
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(local_provider_wraps_keys_bound_to_id_context_and_passphrase) {
    const kin::local_kms_provider p("correct horse battery");
    const kin::encryption_context ctx{{"stream", "a/b/1"}};
    const auto k = p.generate_data_key("alias/aws/kinesis", ctx);
    BOOST_TEST(k._key_id == "alias/aws/kinesis");
    BOOST_TEST(k._wrapped.find(std::string(reinterpret_cast<const char*>(k._plaintext.data()),
                                           k._plaintext.size())) == std::string::npos);
    BOOST_TEST((p.decrypt_data_key("alias/aws/kinesis", k._wrapped, ctx) == k._plaintext));

    // A copy (another gateway's provider) shares nothing but the passphrase.
    const kin::local_kms_provider other("correct horse battery");
    BOOST_TEST((other.decrypt_data_key("alias/aws/kinesis", k._wrapped, ctx) == k._plaintext));

    const auto refused = [&](auto&& f, kin::kms_failure want) {
        try {
            f();
        } catch (const kin::kms_error& e) {
            return e.failure() == want;
        }
        return false;
    };
    using enum kin::kms_failure;
    BOOST_TEST(
        refused([&] { (void)p.decrypt_data_key("other-key", k._wrapped, ctx); }, invalid_state));
    BOOST_TEST(refused(
        [&] { (void)p.decrypt_data_key("alias/aws/kinesis", k._wrapped, {{"stream", "a/b/2"}}); },
        invalid_state));
    BOOST_TEST(refused(
        [&] {
            (void)kin::local_kms_provider("wrong").decrypt_data_key("alias/aws/kinesis", k._wrapped,
                                                                    ctx);
        },
        invalid_state));
    auto tampered = k._wrapped;
    tampered.back() = static_cast<char>(tampered.back() ^ 1);
    BOOST_TEST(refused([&] { (void)p.decrypt_data_key("alias/aws/kinesis", tampered, ctx); },
                       invalid_state));
    BOOST_TEST(refused([&] { (void)p.generate_data_key("", ctx); }, not_found));
    BOOST_CHECK_THROW(kin::local_kms_provider(""), std::invalid_argument);

    // Erased, and empty.
    const kin::key_provider erased(p);
    BOOST_TEST(static_cast<bool>(erased));
    BOOST_TEST((erased.decrypt_data_key("alias/aws/kinesis", k._wrapped, ctx) == k._plaintext));
    const kin::key_provider none;
    BOOST_TEST(!static_cast<bool>(none));
    BOOST_TEST(refused([&] { (void)none.generate_data_key("k", ctx); }, unavailable));
}

BOOST_AUTO_TEST_CASE(sealed_records_open_only_in_their_shard_under_their_key) {
    kin::data_key_bytes key{};
    key.fill(7);
    const auto data = bytes_of(k_secret_text);
    const auto sealed = kin::seal_record(key, 3, "k/1/0", "pk-1", data);
    BOOST_TEST(kin::is_sealed_record(sealed));
    BOOST_TEST(sealed.size() == data.size() + 4 + kin::k_sealed_record_overhead);
    BOOST_TEST(kin::sealed_generation(sealed).value_or(0) == 3U);
    const std::string_view text(reinterpret_cast<const char*>(sealed.data()), sealed.size());
    BOOST_TEST(text.find(k_secret_text) == std::string_view::npos);

    const auto opened = kin::open_record(key, "k/1/0", sealed);
    BOOST_TEST(opened._partition_key == "pk-1");
    BOOST_TEST((opened._data == data));

    // Billed as its plaintext: partition key and data.
    const kin::append_record a{._partition_key = {}, ._data = sealed};
    BOOST_TEST(a.sealed());
    BOOST_TEST(a.billed_bytes() == data.size() + 4);

    BOOST_CHECK_THROW((void)kin::open_record(key, "k/1/1", sealed), kin::record_open_error);
    auto other = key;
    other[0] = 8;
    BOOST_CHECK_THROW((void)kin::open_record(other, "k/1/0", sealed), kin::record_open_error);
    auto tampered = sealed;
    tampered[10] ^= std::byte{1};
    BOOST_CHECK_THROW((void)kin::open_record(key, "k/1/0", tampered), kin::record_open_error);
    // The generation is bound too.
    auto relabelled = sealed;
    relabelled[5] = std::byte{4};
    BOOST_CHECK_THROW((void)kin::open_record(key, "k/1/0", relabelled), kin::record_open_error);
    // A plaintext stored record never looks sealed.
    BOOST_TEST(!kin::is_sealed_record(kin::encode_stored_record(std::string(256, 'p'), data)));
}

BOOST_AUTO_TEST_CASE(the_log_refuses_a_sealed_record_without_its_marker) {
    temp_dir dir;
    kythira::record_segment_store store(
        kythira::record_segment_store_options{._data_dir = dir._path, ._barriers_enabled = false});
    kin::local_stream_cluster cluster{&store, [] { return std::int64_t{1}; }};
    const auto key = kin::routing_key(1, 0);
    BOOST_REQUIRE(cluster.propose(key, kin::encode_command(kin::open_shard_command{key, 0})));
    const auto append = [&](std::vector<std::byte> data) {
        return cluster.propose(key, kin::encode_command(kin::append_records_command{
                                        ._key = key,
                                        ._arrival_ms = 1,
                                        ._records = {{._partition_key = {}, ._data = data}}}));
    };
    const auto bad = append(std::vector<std::byte>(64, std::byte{1}));
    BOOST_REQUIRE(bad.has_value());
    BOOST_TEST((bad->_status == kin::apply_status::invalid));
    const auto short_one = append({std::byte{0xFF}, std::byte{0xFF}, std::byte{0}});
    BOOST_TEST((short_one->_status == kin::apply_status::invalid));
    kin::data_key_bytes k{};
    const auto good = append(kin::seal_record(k, 1, key, "pk", bytes_of("x")));
    BOOST_TEST((good->_status == kin::apply_status::ok));
}

BOOST_AUTO_TEST_CASE(an_encrypted_stream_stores_ciphertext_and_reads_back_everywhere) {
    fixture f;
    (void)f.gw.ok("CreateStream", {{"StreamName", "s"}, {"ShardCount", 1}});
    const auto put = [&](std::string_view pk, std::string_view data) {
        return f.gw.ok("PutRecord",
                       {{"StreamName", "s"}, {"PartitionKey", pk}, {"Data", b64(data)}});
    };
    // Absent, not "NONE", for an unencrypted stream, as AWS answers.
    BOOST_TEST(!put("clear", "plain-before").contains("EncryptionType"));

    (void)f.gw.ok("StartStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "alias/aws/kinesis"}});
    const auto summary = f.gw.ok("DescribeStreamSummary", {{"StreamName", "s"}})
                             .at("StreamDescriptionSummary")
                             .as_object();
    BOOST_TEST(summary.at("EncryptionType").as_string() == "KMS");
    BOOST_TEST(summary.at("KeyId").as_string() == "alias/aws/kinesis");
    BOOST_TEST(summary.at("StreamStatus").as_string() == "ACTIVE");

    BOOST_TEST(put("secret-pk", k_secret_text).at("EncryptionType").as_string() == "KMS");
    const auto batch = f.gw.ok(
        "PutRecords",
        {{"StreamName", "s"},
         {"Records", json::array{json::object{{"PartitionKey", "p2"}, {"Data", b64("batch-1")}}}}});
    BOOST_TEST(batch.at("EncryptionType").as_string() == "KMS");
    BOOST_TEST(batch.at("FailedRecordCount").as_int64() == 0);

    // The same key again changes nothing; another key re-keys.
    (void)f.gw.ok("StartStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "alias/aws/kinesis"}});
    BOOST_TEST(f.record("s")._data_keys.size() == 1U);
    (void)f.gw.ok("StartStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "key-two"}});
    BOOST_TEST(f.record("s")._data_keys.size() == 2U);
    BOOST_TEST(f.record("s")._data_keys.back()._generation == 2U);
    (void)put("rekeyed", "after-rekey");

    (void)f.gw.ok("StopStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "key-two"}});
    BOOST_TEST(f.gw.ok("DescribeStreamSummary", {{"StreamName", "s"}})
                   .at("StreamDescriptionSummary")
                   .as_object()
                   .at("EncryptionType")
                   .as_string() == "NONE");
    BOOST_TEST(!put("clear-again", "plain-after").contains("EncryptionType"));

    // What reached the disk: plaintext only from the unencrypted stretches.
    BOOST_TEST(f.on_disk("plain-before"));
    BOOST_TEST(f.on_disk("plain-after"));
    BOOST_TEST(!f.on_disk(k_secret_text));
    BOOST_TEST(!f.on_disk("secret-pk"));
    BOOST_TEST(!f.on_disk("batch-1"));
    BOOST_TEST(!f.on_disk("after-rekey"));

    const std::vector<std::vector<std::string>> want{{"clear", b64("plain-before"), "NONE"},
                                                     {"secret-pk", b64(k_secret_text), "KMS"},
                                                     {"p2", b64("batch-1"), "KMS"},
                                                     {"rekeyed", b64("after-rekey"), "KMS"},
                                                     {"clear-again", b64("plain-after"), "NONE"}};
    BOOST_TEST((f.gw.read_all("s") == want));

    // Another gateway with only the passphrase in common reads it all.
    gateway second(f.cluster, kin::key_provider(kin::local_kms_provider("correct horse battery")));
    BOOST_TEST((second.read_all("s") == want));

    // A gateway with another passphrase cannot unwrap the data keys; one
    // with no provider has none to unwrap them with.
    gateway wrong(f.cluster, kin::key_provider(kin::local_kms_provider("wrong")));
    const auto it = wrong.ok("GetShardIterator", {{"StreamName", "s"},
                                                  {"ShardId", "shardId-000000000000"},
                                                  {"ShardIteratorType", "TRIM_HORIZON"}});
    const auto r = wrong.call("GetRecords", {{"ShardIterator", it.at("ShardIterator")}});
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(type_of(r) == "KMSInvalidStateException");
    gateway none(f.cluster, kin::key_provider{});
    const auto r2 = none.call("GetRecords", {{"ShardIterator", it.at("ShardIterator")}});
    BOOST_TEST(type_of(r2) == "KMSNotFoundException");
    // Writing to an encrypted stream needs the provider too.
    (void)f.gw.ok("StartStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "key-three"}});
    const auto w =
        none.call("PutRecord", {{"StreamName", "s"}, {"PartitionKey", "p"}, {"Data", b64("x")}});
    BOOST_TEST(type_of(w) == "KMSNotFoundException");
}

BOOST_AUTO_TEST_CASE(start_and_stop_refusals) {
    fixture f;
    (void)f.gw.ok("CreateStream", {{"StreamName", "s"}, {"ShardCount", 1}});
    const auto start = [&](gateway& g, std::string_view type, std::string_view key) {
        return g.call("StartStreamEncryption",
                      {{"StreamName", "s"}, {"EncryptionType", type}, {"KeyId", key}});
    };
    auto r = start(f.gw, "NONE", "k");
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    r = f.gw.call("StartStreamEncryption",
                  {{"StreamName", "missing"}, {"EncryptionType", "KMS"}, {"KeyId", "k"}});
    BOOST_TEST(type_of(r) == "ResourceNotFoundException");

    gateway none(f.cluster, kin::key_provider{});
    r = start(none, "KMS", "k");
    BOOST_TEST(type_of(r) == "InvalidArgumentException");

    // Stopping a stream that is not encrypted changes nothing.
    (void)f.gw.ok("StopStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "k"}});
    BOOST_TEST(f.record("s")._data_keys.empty());

    (void)f.gw.ok("StartStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "k"}});
    r = f.gw.call("StopStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "other"}});
    BOOST_TEST(type_of(r) == "InvalidArgumentException");

    // Provider failures answer as Kinesis's KMS exceptions.
    const std::vector<std::pair<kin::kms_failure, std::string>> cases{
        {kin::kms_failure::not_found, "KMSNotFoundException"},
        {kin::kms_failure::access_denied, "KMSAccessDeniedException"},
        {kin::kms_failure::disabled, "KMSDisabledException"},
        {kin::kms_failure::invalid_state, "KMSInvalidStateException"},
        {kin::kms_failure::throttling, "KMSThrottlingException"},
        {kin::kms_failure::opt_in_required, "KMSOptInRequired"}};
    for (const auto& [failure, type] : cases) {
        gateway failing(f.cluster, kin::key_provider(failing_provider{failure}));
        const auto got = start(failing, "KMS", "another-key");
        BOOST_TEST(type_of(got) == type);
    }
}

BOOST_AUTO_TEST_CASE(aws_kms_provider_signs_its_calls_and_maps_kms_errors) {
    const auto now = std::chrono::sys_seconds{std::chrono::seconds{1'794'268'800}};  // 2026-11-10
    kin::data_key_bytes plain{};
    plain.fill(9);
    const std::string plain_text(reinterpret_cast<const char*>(plain.data()), plain.size());
    std::vector<std::string> targets;
    std::optional<std::pair<unsigned, std::string>> canned;
    kin::aws_kms_config cfg;
    cfg._region = "us-west-2";
    cfg._credentials = [] {
        return kin::aws_kms_credentials{"AKIDEXAMPLE", "secret-example", "session-token"};
    };
    cfg._clock = [now] { return now; };
    const kin::aws_kms_provider p(
        cfg,
        [&](const std::string& host_port, const kin::peer_client::headers& headers,
            std::string body) -> std::optional<kin::peer_response> {
            BOOST_TEST(host_port == "kms.us-west-2.amazonaws.com:443");
            // The signature verifies with the verifier the gateways use.
            sv4::service_rules rules;
            rules.service = "kms";
            rules.protocol = sv4::error_protocol::json;
            const auto parsed =
                sv4::parse(sv4::request{"POST", "/", "", headers, body}, rules, now);
            BOOST_REQUIRE_MESSAGE(parsed.has_value(), parsed.error().message);
            BOOST_TEST(parsed->access_key_id == "AKIDEXAMPLE");
            BOOST_TEST(parsed->security_token.value_or("") == "session-token");
            BOOST_TEST(
                sv4::verify(*parsed, sv4::derive_signing_key("secret-example", parsed->scope)));
            std::string target;
            for (const auto& [k, v] : headers) {
                if (k == "x-amz-target") {
                    target = v;
                }
            }
            targets.push_back(target);
            const auto req = json::parse(body).as_object();
            BOOST_TEST(req.at("EncryptionContext").as_object().at("stream").as_string() == "x");
            if (canned) {
                return kin::peer_response{._status = canned->first, ._body = canned->second};
            }
            if (target == "TrentService.GenerateDataKey") {
                BOOST_TEST(req.at("KeySpec").as_string() == "AES_256");
                return kin::peer_response{
                    ._status = 200,
                    ._body = json::serialize(
                        json::object{{"KeyId", "arn:aws:kms:us-west-2:111122223333:key/abcd"},
                                     {"Plaintext", b64(plain_text)},
                                     {"CiphertextBlob", b64("wrapped-blob")}})};
            }
            BOOST_TEST(req.at("CiphertextBlob").as_string() == b64("wrapped-blob"));
            BOOST_TEST(req.at("KeyId").as_string() ==
                       "arn:aws:kms:us-west-2:111122223333:key/abcd");
            return kin::peer_response{
                ._status = 200,
                ._body = json::serialize(json::object{{"Plaintext", b64(plain_text)}})};
        });
    const kin::encryption_context ctx{{"stream", "x"}};
    const auto k = p.generate_data_key("alias/aws/kinesis", ctx);
    BOOST_TEST(k._key_id == "arn:aws:kms:us-west-2:111122223333:key/abcd");
    BOOST_TEST(k._wrapped == "wrapped-blob");
    BOOST_TEST((k._plaintext == plain));
    BOOST_TEST((p.decrypt_data_key(k._key_id, k._wrapped, ctx) == plain));
    BOOST_TEST((targets ==
                std::vector<std::string>{"TrentService.GenerateDataKey", "TrentService.Decrypt"}));

    const auto failure_for = [&](unsigned status, const std::string& body) {
        canned = std::pair{status, body};
        try {
            (void)p.generate_data_key("k", ctx);
        } catch (const kin::kms_error& e) {
            return std::optional{e.failure()};
        }
        return std::optional<kin::kms_failure>{};
    };
    using enum kin::kms_failure;
    BOOST_TEST((failure_for(400, R"({"__type":"NotFoundException","message":"m"})") == not_found));
    BOOST_TEST((failure_for(400, R"({"__type":"DisabledException"})") == disabled));
    BOOST_TEST((failure_for(400, R"({"__type":"com.amazonaws.kms#AccessDeniedException"})") ==
                access_denied));
    BOOST_TEST((failure_for(400, R"({"__type":"ThrottlingException"})") == throttling));
    BOOST_TEST((failure_for(400, R"({"__type":"KMSInvalidStateException"})") == invalid_state));
    BOOST_TEST((failure_for(500, "not json") == invalid_state));
    BOOST_TEST((failure_for(200, R"({"Plaintext":"c2hvcnQ="})") == invalid_state));

    // No credentials: refused before any call.
    auto no_creds = cfg;
    no_creds._credentials = [] { return kin::aws_kms_credentials{}; };
    const kin::aws_kms_provider q(no_creds, [](auto&&...) -> std::optional<kin::peer_response> {
        BOOST_FAIL("called");
        return std::nullopt;
    });
    try {
        (void)q.generate_data_key("k", ctx);
        BOOST_FAIL("expected kms_error");
    } catch (const kin::kms_error& e) {
        BOOST_TEST((e.failure() == access_denied));
    }
}

BOOST_AUTO_TEST_CASE(encryption_commands_and_data_keys_round_trip) {
    const kin::set_encryption_command start{
        ._account = std::string(k_account),
        ._name = "s",
        ._id = 7,
        ._encryption = kin::encryption_type::kms,
        ._key_id = "alias/aws/kinesis",
        ._data_key = kin::data_key_record{._generation = 0, ._key_id = "arn:k", ._wrapped = "w"}};
    const auto bytes = kin::encode_command(start);
    BOOST_TEST((kin::encode_command(kin::decode_command(bytes)) == bytes));
    BOOST_TEST(kin::command_key(start) == kin::stream_key(std::string(k_account), "s"));
    const kin::set_encryption_command stop{._account = std::string(k_account),
                                           ._name = "s",
                                           ._id = 7,
                                           ._encryption = kin::encryption_type::none,
                                           ._key_id = {},
                                           ._data_key = std::nullopt};
    const auto stop_bytes = kin::encode_command(stop);
    BOOST_TEST((kin::encode_command(kin::decode_command(stop_bytes)) == stop_bytes));

    kin::stream_record rec;
    rec._id = 7;
    rec._account = std::string(k_account);
    rec._name = "s";
    rec._encryption = kin::encryption_type::kms;
    rec._key_id = "alias/aws/kinesis";
    rec._data_keys = {{._generation = 1, ._key_id = "arn:a", ._wrapped = std::string("\0\1", 2)},
                      {._generation = 2, ._key_id = "arn:b", ._wrapped = "x"}};
    BOOST_TEST((kin::decode_stream_record(kin::encode_stream_record(rec)) == rec));
    BOOST_TEST(rec.current_data_key()->_generation == 2U);
    BOOST_TEST(rec.find_data_key(1)->_key_id == "arn:a");
    BOOST_TEST(rec.find_data_key(3) == nullptr);
    rec._encryption = kin::encryption_type::none;
    BOOST_TEST(rec.current_data_key() == nullptr);
}

// doc/encryption.md: a plaintext data key is never replicated. Neither the
// state machine's snapshot nor the segment files hold it; only the wrapped
// form is in the catalog, and it unwraps to a key that is nowhere on disk.
BOOST_AUTO_TEST_CASE(a_plaintext_data_key_is_never_replicated) {
    fixture f;
    (void)f.gw.ok("CreateStream", {{"StreamName", "s"}, {"ShardCount", 1}});
    (void)f.gw.ok("StartStreamEncryption",
                  {{"StreamName", "s"}, {"EncryptionType", "KMS"}, {"KeyId", "alias/aws/kinesis"}});
    (void)f.gw.ok("PutRecord", {{"StreamName", "s"}, {"PartitionKey", "pk"}, {"Data", b64("x")}});
    const auto rec = f.record("s");
    BOOST_REQUIRE(rec._data_keys.size() == 1U);
    const kin::local_kms_provider provider("correct horse battery");
    const kin::encryption_context ctx{
        {"kythira:kinesis:stream", rec._account + "/" + rec._name + "/" + std::to_string(rec._id)}};
    const auto plaintext =
        provider.decrypt_data_key(rec._data_keys[0]._key_id, rec._data_keys[0]._wrapped, ctx);
    const std::string needle(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());

    std::vector<std::byte> snapshot;
    (void)f.cluster.read("", [&](const auto& sm) { snapshot = sm.get_state(); });
    const std::string state(reinterpret_cast<const char*>(snapshot.data()), snapshot.size());
    BOOST_TEST(!state.empty());
    BOOST_TEST(state.find(rec._data_keys[0]._wrapped) != std::string::npos);
    BOOST_TEST(state.find(needle) == std::string::npos);
    BOOST_TEST(!f.on_disk(needle));
}
