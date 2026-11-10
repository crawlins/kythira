// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_kms_aws.hpp
/// @brief A `kms_key_provider` over AWS KMS (`.kiro/specs/kinesis-compatible-streams/`
///        task 14, Requirement 14.1).
///
/// It speaks KMS's JSON 1.1 protocol itself, signed with SigV4, so it needs
/// no AWS SDK component: `GenerateDataKey` (`KeySpec` `AES_256`) wraps a
/// fresh data key and `Decrypt` unwraps it, both with the stream's
/// encryption context. The key id can be anything KMS takes: a key id, a
/// key ARN, an alias name or an alias ARN. `GenerateDataKey` answers with
/// the key's ARN, which is what the catalog keeps, so a later `Decrypt`
/// does not depend on an alias still pointing where it did.
///
/// KMS's errors map onto Kinesis's: `NotFoundException` to
/// `KMSNotFoundException`, `DisabledException` to `KMSDisabledException`,
/// `KMSInvalidStateException` and `InvalidCiphertextException` to
/// `KMSInvalidStateException`, `AccessDeniedException` to
/// `KMSAccessDeniedException`, `ThrottlingException` to
/// `KMSThrottlingException`, `OptInRequired` to `KMSOptInRequired`, and
/// anything else (including no answer) to `KMSInvalidStateException`.
///
/// Credentials come from a callback, so a caller can rotate them; the
/// daemon reads the standard `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`
/// and `AWS_SESSION_TOKEN`.

#include <kythira/kinesis/kinesis_kms.hpp>
#include <kythira/kinesis/kinesis_peer_client.hpp>

#include <raft/sigv4_verifier.hpp>

#include <boost/json.hpp>

#include <openssl/evp.h>

#include <chrono>
#include <ctime>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::kinesis {

struct aws_kms_credentials {
    std::string _access_key_id;
    std::string _secret_access_key;
    std::string _session_token;
};

struct aws_kms_config {
    std::string _region;
    /// "host:port"; empty is `kms.<region>.amazonaws.com:443`.
    std::string _endpoint;
    bool _tls = true;
    std::string _ca_path;
    std::chrono::milliseconds _timeout{5000};
    std::function<aws_kms_credentials()> _credentials;
    /// The signing clock; the system clock when empty (tests pin it).
    std::function<std::chrono::sys_seconds()> _clock;
};

/// One signed POST: (host:port, headers, body) to the answer. The default
/// is `peer_client`; tests put a fake KMS here.
using aws_kms_transport = std::function<std::optional<peer_response>(
    const std::string& host_port, const peer_client::headers& headers, std::string body)>;

namespace kms_aws_detail {

[[nodiscard]] inline auto base64_encode(std::string_view in) -> std::string {
    std::string out(4 * ((in.size() + 2) / 3), '\0');
    const auto n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                   reinterpret_cast<const unsigned char*>(in.data()),
                                   static_cast<int>(in.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

[[nodiscard]] inline auto base64_decode(std::string_view in) -> std::optional<std::string> {
    if (in.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out(3 * (in.size() / 4), '\0');
    const auto n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                   reinterpret_cast<const unsigned char*>(in.data()),
                                   static_cast<int>(in.size()));
    if (n < 0) {
        return std::nullopt;
    }
    std::size_t pad = 0;
    if (!in.empty() && in.back() == '=') {
        pad = in.size() >= 2 && in[in.size() - 2] == '=' ? 2 : 1;
    }
    out.resize(static_cast<std::size_t>(n) - pad);
    return out;
}

[[nodiscard]] inline auto failure_of(std::string_view type) -> kms_failure {
    // `__type` may carry a namespace ("com.amazonaws.kms#NotFoundException").
    if (const auto hash = type.rfind('#'); hash != std::string_view::npos) {
        type = type.substr(hash + 1);
    }
    if (type == "NotFoundException") {
        return kms_failure::not_found;
    }
    if (type == "DisabledException") {
        return kms_failure::disabled;
    }
    if (type == "AccessDeniedException") {
        return kms_failure::access_denied;
    }
    if (type == "ThrottlingException" || type == "LimitExceededException") {
        return kms_failure::throttling;
    }
    if (type == "OptInRequired") {
        return kms_failure::opt_in_required;
    }
    return kms_failure::invalid_state;
}

}  // namespace kms_aws_detail

class aws_kms_provider {
public:
    explicit aws_kms_provider(aws_kms_config config, aws_kms_transport transport = {})
        : _config(std::move(config)), _transport(std::move(transport)) {
        if (_config._region.empty()) {
            throw std::invalid_argument("aws_kms_provider: a region is required");
        }
        if (_config._endpoint.empty()) {
            _config._endpoint = "kms." + _config._region + ".amazonaws.com:443";
        }
        if (!_transport) {
            peer_client client(peer_client_config{._tls = _config._tls,
                                                  ._ca_path = _config._ca_path,
                                                  ._timeout = _config._timeout,
                                                  ._max_body_bytes = 1024 * 1024});
            _transport = [client = std::move(client)](const std::string& host_port,
                                                      const peer_client::headers& headers,
                                                      std::string body) {
                return client.request(host_port, boost::beast::http::verb::post, "/", headers,
                                      std::move(body));
            };
        }
    }

    [[nodiscard]] auto generate_data_key(const std::string& key_id,
                                         const encryption_context& context) const
        -> generated_data_key {
        boost::json::object req{{"KeyId", key_id}, {"KeySpec", "AES_256"}};
        add_context(req, context);
        const auto res = call("GenerateDataKey", req);
        generated_data_key out;
        const auto plain = blob(res, "Plaintext");
        if (plain.size() != out._plaintext.size()) {
            throw kms_error(kms_failure::invalid_state,
                            "KMS returned a data key of the wrong size.");
        }
        std::memcpy(out._plaintext.data(), plain.data(), plain.size());
        out._wrapped = blob(res, "CiphertextBlob");
        const auto* id = res.if_contains("KeyId");
        out._key_id = id != nullptr && id->is_string() ? std::string(id->get_string()) : key_id;
        return out;
    }

    [[nodiscard]] auto decrypt_data_key(const std::string& key_id, const std::string& wrapped,
                                        const encryption_context& context) const -> data_key_bytes {
        boost::json::object req{{"KeyId", key_id},
                                {"CiphertextBlob", kms_aws_detail::base64_encode(wrapped)}};
        add_context(req, context);
        const auto res = call("Decrypt", req);
        const auto plain = blob(res, "Plaintext");
        data_key_bytes out{};
        if (plain.size() != out.size()) {
            throw kms_error(kms_failure::invalid_state,
                            "KMS returned a data key of the wrong size.");
        }
        std::memcpy(out.data(), plain.data(), out.size());
        return out;
    }

    /// The headers of a signed KMS call: exposed for tests.
    [[nodiscard]] auto signed_headers(std::string_view target, std::string_view body) const
        -> peer_client::headers {
        namespace sd = sigv4::detail;
        const auto creds = _config._credentials ? _config._credentials() : aws_kms_credentials{};
        if (creds._access_key_id.empty() || creds._secret_access_key.empty()) {
            throw kms_error(kms_failure::access_denied, "No AWS credentials for KMS.");
        }
        const auto now =
            _config._clock
                ? _config._clock()
                : std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto amz_date = sd::format_amz_date(now);
        const sigv4::credential_scope scope{
            .date = amz_date.substr(0, 8), .region = _config._region, .service = "kms"};
        // Signed in sorted order: content-type, host, x-amz-date,
        // [x-amz-security-token,] x-amz-target.
        std::vector<std::pair<std::string, std::string>> h{
            {"content-type", "application/x-amz-json-1.1"},
            {"host", host_header()},
            {"x-amz-date", amz_date},
        };
        if (!creds._session_token.empty()) {
            h.emplace_back("x-amz-security-token", creds._session_token);
        }
        h.emplace_back("x-amz-target", "TrentService." + std::string(target));
        std::string canonical = "POST\n/\n\n";
        std::string names;
        for (const auto& [k, v] : h) {
            canonical += k + ":" + v + "\n";
            names += (names.empty() ? "" : ";") + k;
        }
        canonical += "\n" + names + "\n" + sd::sha256_hex(body);
        const auto to_sign = std::string(sigv4::k_algorithm) + "\n" + amz_date + "\n" +
                             scope.text() + "\n" + sd::sha256_hex(canonical);
        const auto key = sigv4::derive_signing_key(creds._secret_access_key, scope);
        const auto signature = sd::to_hex(sd::hmac_sha256(key, to_sign));
        peer_client::headers out;
        out.reserve(h.size() + 1);
        for (auto& [k, v] : h) {
            out.emplace_back(k == "host" ? "Host" : k, std::move(v));
        }
        out.emplace_back("Authorization", std::string(sigv4::k_algorithm) +
                                              " Credential=" + creds._access_key_id + "/" +
                                              scope.text() + ", SignedHeaders=" + names +
                                              ", Signature=" + signature);
        return out;
    }

private:
    aws_kms_config _config;
    aws_kms_transport _transport;

    // The Host header without the default port, as SDKs send it.
    [[nodiscard]] auto host_header() const -> std::string {
        const auto& e = _config._endpoint;
        if ((_config._tls && e.ends_with(":443")) || (!_config._tls && e.ends_with(":80"))) {
            return e.substr(0, e.rfind(':'));
        }
        return e;
    }

    static void add_context(boost::json::object& req, const encryption_context& context) {
        if (context.empty()) {
            return;
        }
        boost::json::object c;
        for (const auto& [k, v] : context) {
            c[k] = v;
        }
        req["EncryptionContext"] = std::move(c);
    }

    [[nodiscard]] static auto blob(const boost::json::object& res, std::string_view name)
        -> std::string {
        const auto* v = res.if_contains(name);
        if (v == nullptr || !v->is_string()) {
            throw kms_error(kms_failure::invalid_state,
                            "KMS answered without " + std::string(name) + ".");
        }
        auto decoded = kms_aws_detail::base64_decode(v->get_string());
        if (!decoded) {
            throw kms_error(kms_failure::invalid_state,
                            "KMS answered with a malformed " + std::string(name) + ".");
        }
        return std::move(*decoded);
    }

    [[nodiscard]] auto call(std::string_view target, const boost::json::object& req) const
        -> boost::json::object {
        auto body = boost::json::serialize(req);
        auto headers = signed_headers(target, body);
        const auto res = _transport(_config._endpoint, headers, std::move(body));
        if (!res) {
            throw kms_error(kms_failure::invalid_state, "KMS did not answer.");
        }
        boost::json::object parsed;
        try {
            auto v = boost::json::parse(res->_body);
            if (v.is_object()) {
                parsed = std::move(v.get_object());
            }
        } catch (const std::exception&) {
            // An unparseable body is judged by its status below.
        }
        if (res->_status == 200) {
            return parsed;
        }
        std::string type;
        if (const auto* t = parsed.if_contains("__type"); t != nullptr && t->is_string()) {
            type = std::string(t->get_string());
        }
        std::string message = "KMS answered " + std::to_string(res->_status);
        for (const auto* key : {"message", "Message"}) {
            if (const auto* m = parsed.if_contains(key); m != nullptr && m->is_string()) {
                message = std::string(m->get_string());
            }
        }
        throw kms_error(kms_aws_detail::failure_of(type), message);
    }
};

static_assert(kms_key_provider<aws_kms_provider>);

}  // namespace kythira::kinesis
