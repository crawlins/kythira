// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Task 1 of .kiro/specs/object-store-encrypted-bucket-checksums/: what does a
// single-part PUT from aws_s3_client's exact SDK configuration send, and what
// does S3 answer, on SSE-S3, SSE-KMS and DSSE-KMS buckets?
//
// The client configuration below is copied from aws_s3_client's constructor
// (include/raft/aws_s3_client.hpp) and must stay identical to it: zero
// retries, requestChecksumCalculation = WHEN_REQUIRED, response validation at
// the SDK default, path-style and no IMDS when an endpoint is overridden, and
// an explicit Content-MD5 on every PUT. The request headers are read after
// signing (RequestSignedHandler), so the body framing printed is the one that
// went on the wire. The response is printed as the SDK's PutObjectResult
// models it; the raw response headers come from --trace, which turns on the
// SDK's trace log (the curl client logs every received header line there).
// A HeadersReceivedEventHandler on the request does not fire on this path in
// SDK 1.11.665, so it is not used.
//
// Usage (credentials from the SDK's default chain, e.g. AWS_PROFILE):
//   s3_checksum_spike [--endpoint URL] [--region R]
//       [--put LABEL=BUCKET]... [--conditional BUCKET] [--plain BUCKET]
//       [--trace LOG_PREFIX]
// Every object goes under spike/encrypted-checksum-<epoch>/ and is deleted
// before exit.

#include <aws/core/Aws.h>
#include <aws/core/Version.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/PutObjectRequest.h>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* k_tag = "s3_checksum_spike";

using Aws::Utils::HashingUtils;

struct exchange {
    std::map<std::string, std::string> request_headers;
};

auto lower(std::string s) -> std::string {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The headers that decide the questions of Requirement 1.1. Authorization and
// the session token are never printed.
auto interesting_request(const std::string& name) -> bool {
    const auto n = lower(name);
    return n == "content-length" || n == "content-encoding" || n == "content-md5" ||
           n == "transfer-encoding" || n == "if-match" || n == "if-none-match" ||
           n.rfind("x-amz-checksum", 0) == 0 || n == "x-amz-trailer" ||
           n == "x-amz-content-sha256" || n == "x-amz-decoded-content-length" ||
           n == "x-amz-sdk-checksum-algorithm";
}

void attach(Aws::AmazonWebServiceRequest& req, exchange& ex) {
    req.SetRequestSignedHandler([&ex](const Aws::Http::HttpRequest& http) {
        for (const auto& [k, v] : http.GetHeaders()) {
            if (interesting_request(k)) ex.request_headers[lower(k)] = v;
        }
    });
}

void print_exchange(const exchange& ex) {
    std::cout << "    request:";
    if (ex.request_headers.empty()) std::cout << " (none captured)";
    for (const auto& [k, v] : ex.request_headers) std::cout << "\n      " << k << ": " << v;
    std::cout << "\n";
}

auto hex(const Aws::Utils::ByteBuffer& b) -> std::string {
    return std::string(HashingUtils::HexEncode(b));
}
auto b64(const Aws::Utils::ByteBuffer& b) -> std::string {
    return std::string(HashingUtils::Base64Encode(b));
}

// A copy of aws_s3_client's constructor body, minus the credentials provider
// (the spike uses the default chain, which is what aws_s3_client does when
// none is configured).
auto make_client(const std::string& region, const std::string& endpoint)
    -> std::shared_ptr<Aws::S3::S3Client> {
    Aws::Client::ClientConfigurationInitValues init;
    init.shouldDisableIMDS = !endpoint.empty();
    Aws::S3::S3ClientConfiguration cfg{init};
    if (!region.empty()) cfg.region = region;
    if (!endpoint.empty()) {
        cfg.endpointOverride = endpoint;
        cfg.useVirtualAddressing = false;
    }
    cfg.requestTimeoutMs = 30000;
    cfg.connectTimeoutMs = 30000;
    cfg.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(k_tag, 0L);
    cfg.checksumConfig.requestChecksumCalculation =
        Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;
    return std::make_shared<Aws::S3::S3Client>(cfg);
}

struct body_digests {
    std::string md5_hex, md5_b64, sha256_b64, crc32c_b64, crc64nvme_b64;
};

auto digests_of(const std::string& body) -> body_digests {
    const Aws::String s(body.data(), body.size());
    const auto md5 = HashingUtils::CalculateMD5(s);
    return {hex(md5), b64(md5), b64(HashingUtils::CalculateSHA256(s)),
            b64(HashingUtils::CalculateCRC32C(s)), b64(HashingUtils::CalculateCRC64(s))};
}

struct put_case {
    std::string name;
    std::optional<Aws::S3::Model::ChecksumAlgorithm> algorithm;
    std::string sha256, crc32c;
    std::string if_match, if_none_match;
};

auto put(Aws::S3::S3Client& s3, const std::string& bucket, const std::string& key,
         const std::string& body, const body_digests& d, const put_case& c) -> std::string {
    Aws::S3::Model::PutObjectRequest req;
    req.SetBucket(bucket);
    req.SetKey(key);
    req.SetContentType("application/octet-stream");
    req.SetContentMD5(d.md5_b64);
    if (c.algorithm) req.SetChecksumAlgorithm(*c.algorithm);
    if (!c.sha256.empty()) req.SetChecksumSHA256(c.sha256);
    if (!c.crc32c.empty()) req.SetChecksumCRC32C(c.crc32c);
    if (!c.if_match.empty()) req.SetIfMatch(c.if_match);
    if (!c.if_none_match.empty()) req.SetIfNoneMatch(c.if_none_match);
    auto stream = Aws::MakeShared<Aws::StringStream>(k_tag);
    stream->write(body.data(), static_cast<std::streamsize>(body.size()));
    req.SetBody(stream);

    exchange ex;
    attach(req, ex);
    auto outcome = s3.PutObject(req);
    std::cout << "  [" << c.name << "] " << key << "\n";
    std::string etag;
    if (outcome.IsSuccess()) {
        const auto& r = outcome.GetResult();
        etag = r.GetETag();
        auto bare = etag;
        if (bare.size() >= 2 && bare.front() == '"') bare = bare.substr(1, bare.size() - 2);
        std::cout << "    OK  etag=" << etag
                  << "  etag==md5hex: " << (bare == d.md5_hex ? "YES" : "NO") << "\n"
                  << "    result: sse="
                  << Aws::S3::Model::ServerSideEncryptionMapper::GetNameForServerSideEncryption(
                         r.GetServerSideEncryption())
                  << " sha256=" << r.GetChecksumSHA256() << " crc32c=" << r.GetChecksumCRC32C()
                  << " crc64nvme=" << r.GetChecksumCRC64NVME() << " type="
                  << Aws::S3::Model::ChecksumTypeMapper::GetNameForChecksumType(r.GetChecksumType())
                  << "\n"
                  << "    result vs local: sha256 "
                  << (r.GetChecksumSHA256().empty()
                          ? "absent"
                          : (r.GetChecksumSHA256() == d.sha256_b64 ? "MATCH" : "DIFFER"))
                  << ", crc32c "
                  << (r.GetChecksumCRC32C().empty()
                          ? "absent"
                          : (r.GetChecksumCRC32C() == d.crc32c_b64 ? "MATCH" : "DIFFER"))
                  << ", crc64nvme "
                  << (r.GetChecksumCRC64NVME().empty()
                          ? "absent"
                          : (r.GetChecksumCRC64NVME() == d.crc64nvme_b64 ? "MATCH" : "DIFFER"))
                  << "\n";
    } else {
        const auto& e = outcome.GetError();
        std::cout << "    FAIL http=" << static_cast<int>(e.GetResponseCode())
                  << " code=" << e.GetExceptionName() << " msg=" << e.GetMessage() << "\n";
    }
    print_exchange(ex);
    return etag;
}

auto del(Aws::S3::S3Client& s3, const std::string& bucket, const std::string& key,
         const std::string& if_match, const std::string& label) -> void {
    Aws::S3::Model::DeleteObjectRequest req;
    req.SetBucket(bucket);
    req.SetKey(key);
    if (!if_match.empty()) req.SetIfMatch(if_match);
    auto outcome = s3.DeleteObject(req);
    if (label.empty()) return;
    std::cout << "  [" << label << "] " << key << "\n    ";
    if (outcome.IsSuccess()) {
        std::cout << "OK\n";
    } else {
        const auto& e = outcome.GetError();
        std::cout << "FAIL http=" << static_cast<int>(e.GetResponseCode())
                  << " code=" << e.GetExceptionName() << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string endpoint, region = "us-east-1", conditional, plain, trace;
    std::vector<std::pair<std::string, std::string>> puts;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--endpoint")
            endpoint = value;
        else if (flag == "--region")
            region = value;
        else if (flag == "--conditional")
            conditional = value;
        else if (flag == "--plain")
            plain = value;
        else if (flag == "--trace")
            trace = value;
        else if (flag == "--put") {
            const auto eq = value.find('=');
            puts.emplace_back(value.substr(0, eq), value.substr(eq + 1));
        } else {
            std::cerr << "unknown flag " << flag << "\n";
            return 2;
        }
    }

    Aws::SDKOptions options;
    if (!trace.empty()) {
        options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Trace;
        options.loggingOptions.defaultLogPrefix = trace.c_str();
    }
    Aws::InitAPI(options);
    {
        auto s3 = make_client(region, endpoint);
        std::string body;
        for (int i = 0; body.size() < 4096; ++i) {
            body += "kythira encrypted-bucket checksum spike line " + std::to_string(i) + "\n";
        }
        body.resize(4096);
        const auto d = digests_of(body);
        const auto prefix = "spike/encrypted-checksum-" +
                            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count()) +
                            "/";
        std::cout << "SDK " << Aws::Version::GetVersionString() << "  region=" << region
                  << "  endpoint=" << (endpoint.empty() ? "(default)" : endpoint) << "\n"
                  << "body: 4096 bytes  md5=" << d.md5_hex << "\n  sha256=" << d.sha256_b64
                  << " crc32c=" << d.crc32c_b64 << " crc64nvme=" << d.crc64nvme_b64 << "\n\n";

        using Aws::S3::Model::ChecksumAlgorithm;
        // Wrong but well-formed: the SHA-256 of an empty body.
        const auto wrong_sha = b64(HashingUtils::CalculateSHA256(Aws::String{}));
        const std::vector<put_case> cases = {
            {"a no-checksum", std::nullopt, "", "", "", ""},
            {"b sha256-value", std::nullopt, d.sha256_b64, "", "", ""},
            {"b2 sha256-algo+value", ChecksumAlgorithm::SHA256, d.sha256_b64, "", "", ""},
            {"c crc32c-value", std::nullopt, "", d.crc32c_b64, "", ""},
            {"c2 crc32c-algo+value", ChecksumAlgorithm::CRC32C, "", d.crc32c_b64, "", ""},
            {"d wrong-sha256-value", std::nullopt, wrong_sha, "", "", ""},
            {"d2 wrong-sha256-algo+value", ChecksumAlgorithm::SHA256, wrong_sha, "", "", ""},
            {"e sha256-algo-only", ChecksumAlgorithm::SHA256, "", "", "", ""},
        };

        std::vector<std::pair<std::string, std::string>> written;
        for (const auto& [label, bucket] : puts) {
            std::cout << "=== " << label << " (" << bucket << ")\n";
            int n = 0;
            for (const auto& c : cases) {
                const auto key = prefix + label + "-" + std::to_string(n++);
                put(*s3, bucket, key, body, d, c);
                written.emplace_back(bucket, key);
            }
            std::cout << "\n";
        }

        if (!conditional.empty()) {
            std::cout << "=== conditional (" << conditional << ")\n";
            const auto key = prefix + "conditional";
            written.emplace_back(conditional, key);
            const auto etag = put(*s3, conditional, key, body, d,
                                  {"If-None-Match:* new key", {}, "", "", "", "*"});
            put(*s3, conditional, key, body, d,
                {"If-None-Match:* existing key", {}, "", "", "", "*"});
            const auto etag2 =
                put(*s3, conditional, key, body, d, {"If-Match right etag", {}, "", "", etag, ""});
            put(*s3, conditional, key, body, d,
                {"If-Match wrong etag", {}, "", "", "\"0123456789abcdef0123456789abcdef\"", ""});
            del(*s3, conditional, key, "\"0123456789abcdef0123456789abcdef\"",
                "DELETE If-Match wrong etag");
            del(*s3, conditional, key, etag2.empty() ? etag : etag2, "DELETE If-Match right etag");
            std::cout << "\n";
        }

        if (!plain.empty()) {
            std::cout << "=== plain PUT to SSE-C-policy bucket (" << plain << ")\n";
            const auto key = prefix + "ssec-policy";
            written.emplace_back(plain, key);
            put(*s3, plain, key, body, d, {"plain", {}, "", "", "", ""});
            std::cout << "\n";
        }

        for (const auto& [bucket, key] : written) del(*s3, bucket, key, "", "");
        std::cout << "cleanup: issued " << written.size() << " deletes\n";
    }
    Aws::ShutdownAPI(options);
    return 0;
}
