// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_federation_unit_test.cpp
/// @brief Instance Principal federation (`oci_federation.hpp`, Requirement
///        1.6) against a real local HTTP server standing in for both the
///        instance metadata service and the Auth service.
///
/// What this tier can and cannot prove, stated up front: every wire detail
/// here — the `Bearer Oracle` metadata header, the `v1/x509` path, the body
/// field names, the no-`host` signed set, the fingerprint spelling, the
/// `ST$` keyId — is asserted against values transcribed from
/// `oracle/oci-go-sdk`'s `common/auth/`, a known-good implementation. That
/// is the strongest oracle available off-instance, and it is still not OCI:
/// the metadata service only exists on a real instance, so a detail the
/// go-sdk reading got wrong stays wrong here until a real instance runs
/// this flow (`spike-notes.md` Finding 16's addendum tracks that).
///
/// The two signature checks are the load-bearing cases. Both rebuild the
/// canonical string from what actually arrived on the wire — the recorded
/// request's own date, body and target — and verify the RSA signature
/// cryptographically: the federation request against the *instance leaf*
/// certificate's public key, and the subsequent API request against the
/// *session* public key lifted out of the federation exchange body itself.
/// That second one pins the contract that matters most: the key OCI is told
/// about and the key that later signs must be the same pair, and a client
/// that regenerated between the two (or signed with the leaf key — the
/// pre-Finding-16 design) fails here.

#define BOOST_TEST_MODULE oci_federation_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/oci_federation.hpp>
#include <raft/oci_http_client.hpp>

#include "test_timeout_scale.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace federation = kythira::oci_federation;

/// 18324 — next free slot in the OCI block (18320-18323 are the http client,
/// quorum unit, quorum mock and certificates mock tests).
constexpr std::uint16_t test_port = 18324;
constexpr const char* bind_address = "127.0.0.1";

const std::string test_origin =
    std::string("http://") + bind_address + ":" + std::to_string(test_port);

/// A fixed instance identity, generated once with the openssl CLI so the
/// expected fingerprint below is an *independent* oracle (openssl's own
/// `-fingerprint -sha256`, lowercased) rather than this header's output fed
/// back to itself. The leaf's subject carries the `opc-tenant:` OU exactly
/// where OCI puts it; the intermediate deliberately has no such attribute.
const std::string leaf_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIEGzCCAwOgAwIBAgIUWC4efolKLxVIpzYoa55hMti1x/gwDQYJKoZIhvcNAQEL
BQAwgZsxNjA0BgNVBAMMLW9jaWQxLmluc3RhbmNlLm9jMS5waHguZmVkZXJhdGlv
bnRlc3RpbnN0YW5jZTFEMEIGA1UECww7b3BjLXRlbmFudDpvY2lkMS50ZW5hbmN5
Lm9jMS4uYWFhYWFhYWFmZWRlcmF0aW9udGVzdHRlbmFuY3kxGzAZBgNVBAoMEk9y
YWNsZSBDb3Jwb3JhdGlvbjAgFw0yNjA4MTEyMzUyNDJaGA8yMTI2MDcxODIzNTI0
MlowgZsxNjA0BgNVBAMMLW9jaWQxLmluc3RhbmNlLm9jMS5waHguZmVkZXJhdGlv
bnRlc3RpbnN0YW5jZTFEMEIGA1UECww7b3BjLXRlbmFudDpvY2lkMS50ZW5hbmN5
Lm9jMS4uYWFhYWFhYWFmZWRlcmF0aW9udGVzdHRlbmFuY3kxGzAZBgNVBAoMEk9y
YWNsZSBDb3Jwb3JhdGlvbjCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEB
AOrNc/5javaCabtFyVxGbdQBnAELLmT41vqsj6aB3oFtXBy97ZFAgFlWXL4O/c9n
G5rDJZMksDM+R2DBx1kvZ9HRRg8lJAhyGG7ijtk4VS8tDLNbNFDqqRgCAMAzTW+S
8+1XJhYyE6iUwVTa2aUDtG1gDGsI4s1egj0DssJN5gBCVCoiV6Vg8f5+g2o7cfa8
baB+U6VCO3FUi1qyEx9fN46V65qBCC1S7hgfA3gPzOB7eyHEhfjhpNyIKlzrtB3B
/kidFFXcrKr/G4GRKfGuJ2rTzWT8KtSB/OuBpXAsxBn2PVvrMwVTKZDQ/zoOa0vB
6PCq2dRpZYZqykO4Ly4oULMCAwEAAaNTMFEwHQYDVR0OBBYEFLCWVvwdCz2iWpl1
Vqi5dU5qP23MMB8GA1UdIwQYMBaAFLCWVvwdCz2iWpl1Vqi5dU5qP23MMA8GA1Ud
EwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAHIwtTDbWsmIIRrrxRgcV3PP
51tcgPOPApL2QjFS2ml02MjbnVoqCE7tuChSbV4ds2ooanzuMeuTzmv/mJhvsxBN
bi0gdEXZqzEx2m1pyqdMHdvTIabfYdNNu+An4xvSpWSeAQICky+OgNVRPGkrfzqT
znlX4zII2SObTGwIZmovwYTsZ/ICslqbK5QsNnoJpTR/maZqBkS8bVcM2j2Nx9Uv
MLDsmI0hyw64FWsNXOGIDrnXXnBZbeTN3V+YTEfC97k3O7s8Jco+euT0DNo6BoiE
qTq4R1svPSm9JQieqqapYYx2H0C2Kz3YhhManprRRupDYJyRt1h8IIC9WZUsPLQ=
-----END CERTIFICATE-----
)";

const std::string leaf_key_pem = R"(-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDqzXP+Y2r2gmm7
RclcRm3UAZwBCy5k+Nb6rI+mgd6BbVwcve2RQIBZVly+Dv3PZxuawyWTJLAzPkdg
wcdZL2fR0UYPJSQIchhu4o7ZOFUvLQyzWzRQ6qkYAgDAM01vkvPtVyYWMhOolMFU
2tmlA7RtYAxrCOLNXoI9A7LCTeYAQlQqIlelYPH+foNqO3H2vG2gflOlQjtxVIta
shMfXzeOleuagQgtUu4YHwN4D8zge3shxIX44aTciCpc67Qdwf5InRRV3Kyq/xuB
kSnxridq081k/CrUgfzrgaVwLMQZ9j1b6zMFUymQ0P86DmtLwejwqtnUaWWGaspD
uC8uKFCzAgMBAAECggEADPBoeXbKEK6MHA1S9NpOuBWknqwakObjlenHesICXHiF
0HGo802uJP4s2y5hTzob29HwMqUdbp16ensDPMdvfbD+HtyJmUuMTCQBAjhn/VMK
taGLhv//dHq7xtus4z6iYYxhZWy6B5q28rGit8S1/ycBWC6jrPSN0cXv5mQ7jfz+
t9lAqZMx0DaFAqsjgLHQ4TXwj3VdjSvWzRMV2Qrzasc/KjVUo82FWA6U37MLNv27
5PLP2YCXcoEUZ3Xq+7yKHsfMXJQH6uBABlh2IKgg3P1lCmb96EjgbUrGbyRqEYjO
ghYPmQn8s5eAp7jj/sGg9j8l4SBxxLnSk5zVtATStQKBgQD1bZxQgo0HE+0jfnF9
6LAnRA694AQ+zL4TN6lEkslZC3gYc+lxgmFQSmuOT6E9Sb1VzAYd5NytYXsVQoDw
6bs8nCuLYiCaOvl2C9EhbmYdbeoWjdcCE9nTeKjE5XrUEerQsmMxzUm838NGb5bA
WiAEWklXNBIIyLam1Hx+fCSZRQKBgQD06qvqOjHG4MMesZ4QR17981OjI7ODUjpA
Er0tqtT+rXVfFI1kVIYlFNbMbrhJez4+GKBmtYjHxLva522aFES4S01Ec2fTbX5n
yJRw6uWLGJM7lhstjZmLb0ni/j9xpc6sXBW5wpMsAPYrrdOpIDrz8VqE6R3q7HIW
gK1U1tZVlwKBgQDMZjZaIvA7JdBI0ETK/ODAQwEYB5rhhnkC3kd90dYY7+FcVCTm
oRRU3zYGdrEtrt0duGabvQBA8b+lbBahDbgfeV0Wn2nRqS4brynD8wLenazojca0
dz3hzkqYeRo7xpROrVLJplQ0FhO29LaTijvCBEOyea5PXIIqHSt12ARPFQKBgQCk
GE+TIeaT/6f7+rmS4p7su4mANr1h8UgCAgwqetk2wfVv/Q8298LhOXMpic2DvkVX
yZw+9MZuQowzAPhYUdgxOpLMmB4qdKRK2QNMHLYrWg3b0JjpDoSf/bKdmgfcF2G/
7frIeWuuAf7uCQtVt7iIFV+2yZI4Aq/2D8USoNb+hQKBgQDqwKwQqC9j2rv7w/zw
SVWT+ZE/eByasj6LQJSip0jF4FYv3hou8vLsOyqIBYEzZ4snqAhfdLY8YyubrKy7
2T+gxwxSfMT8qpOr7p2h8gBFBA0JvmZUGn9zTHWKrjZui1s1Uo610g/QgtorIlIu
KM7hHatw/2BO0DUeQ24x9QNNig==
-----END PRIVATE KEY-----
)";

const std::string intermediate_cert_pem = R"(-----BEGIN CERTIFICATE-----
MIIDdTCCAl2gAwIBAgIUREohyV76YXoIF5S9Amcfe8fLCBcwDQYJKoZIhvcNAQEL
BQAwSTEqMCgGA1UEAwwhUEtJU1ZDIElkZW50aXR5IEludGVybWVkaWF0ZSB0ZXN0
MRswGQYDVQQKDBJPcmFjbGUgQ29ycG9yYXRpb24wIBcNMjYwODExMjM1MjQzWhgP
MjEyNjA3MTgyMzUyNDNaMEkxKjAoBgNVBAMMIVBLSVNWQyBJZGVudGl0eSBJbnRl
cm1lZGlhdGUgdGVzdDEbMBkGA1UECgwST3JhY2xlIENvcnBvcmF0aW9uMIIBIjAN
BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAh02eaNKYEP5xVPY648pf7yd23v04
cUTsOuG2i1T0C6E4sdkfeLCJu3jIs5dg5FWWH1cVdJ/XSEi7xYT4Qe4bOVtAupNc
QWsmKcnVTQnPhY8gYKwOHyoh2MFZlT2d6I2Rmwh1Mw4fqSHEWEfIcIskLfAwdAia
M2xZ7cenfc1Cky34RJ9nNBpsV00c1kvJCfEWW1/tlgL8FGxGBbWmTKopNNvjBePI
bS1g03bUH58k5V5ZjCAAw7uuVvre8bft9M481tHN/zw7JD8R2bqSaceEihfzgGaT
U3doRPH+nJ9qYNvEO0a7F7m43uOJ48gteQcZvppif/EudIh+x/OPf2lVcQIDAQAB
o1MwUTAdBgNVHQ4EFgQUDLEFDAJAfeNOAa5GYO4nLEgR0nAwHwYDVR0jBBgwFoAU
DLEFDAJAfeNOAa5GYO4nLEgR0nAwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0B
AQsFAAOCAQEAEUpwn7dDN391taU2uoezAKZnI648lSGr20ZLnsoeXKdD6gezkK0B
1SafXwpukKQQ9zaJDm6FdTJQwJ2vfZFMpjqB2LUwTPfhgpiJhecJzyIQG8NWN5F2
8Yvi66IKkNHEtX8ExdYeW6jhaKfvzBHWoNwmIQEeVmK7BncV2MCNlpt7wb65Ng70
VP8r1qOtJ89YG/wqTWwSDavlENK1yeDQ0AnT/6NGa8J4nalCz1CfvtjptSXbOz9F
ogCbfzP7P4e/a+knbeW4knR3q5jvm10g7ggaBHXvwOpdSs1VEjZF1bYhcwIB6lDG
3KaOPk90RxOOEMoDEhbRWtLNZH43JMk11w==
-----END CERTIFICATE-----
)";

/// openssl x509 -fingerprint -sha256 over the leaf, lowercased — computed
/// outside this codebase so the fingerprint test has an oracle that is not
/// the function under test.
const std::string expected_leaf_fingerprint =
    "ad:ca:12:7f:d4:53:20:eb:31:21:54:25:ef:cc:3f:47:19:3b:17:1e:c4:f4:bf:cf:ae:e6:41:d2:b0:48:"
    "bd:87";

const std::string expected_tenancy = "ocid1.tenancy.oc1..aaaaaaaafederationtesttenancy";

/// Standard base64url encode (no padding) for building tokens in tests.
auto base64url_encode(const std::string& in) -> std::string {
    std::string out(4 * ((in.size() + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                        reinterpret_cast<const unsigned char*>(in.data()),
                                        static_cast<int>(in.size()));
    out.resize(static_cast<std::size_t>(written));
    while (!out.empty() && out.back() == '=') {
        out.pop_back();
    }
    for (auto& c : out) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    return out;
}

auto make_token(std::time_t exp) -> std::string {
    return base64url_encode(R"({"alg":"RS256","typ":"JWT"})") + "." +
           base64url_encode(R"({"sub":"test","exp":)" + std::to_string(exp) + "}") + ".c2ln";
}

/// Pull one `name="value"` field out of an OCI `authorization` header.
auto auth_field(const std::string& header, const std::string& name) -> std::string {
    const auto key = name + "=\"";
    const auto start = header.find(key);
    if (start == std::string::npos) {
        return {};
    }
    const auto end = header.find('"', start + key.size());
    return header.substr(start + key.size(), end - start - key.size());
}

/// Verify an rsa-sha256 signature (base64, as carried in the authorization
/// header) over @p signed_bytes with @p key.
auto verify_signature(EVP_PKEY* key, const std::string& signed_bytes,
                      const std::string& signature_b64) -> bool {
    // Standard base64 decode; EVP_DecodeBlock pads its output to a multiple
    // of three, so trim by the '='s.
    std::string sig(3 * (signature_b64.size() / 4), '\0');
    const int decoded =
        EVP_DecodeBlock(reinterpret_cast<unsigned char*>(sig.data()),
                        reinterpret_cast<const unsigned char*>(signature_b64.data()),
                        static_cast<int>(signature_b64.size()));
    if (decoded < 0) {
        return false;
    }
    std::size_t pad = 0;
    for (auto it = signature_b64.rbegin(); it != signature_b64.rend() && *it == '='; ++it) {
        ++pad;
    }
    sig.resize(static_cast<std::size_t>(decoded) - pad);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    struct ctx_guard {
        EVP_MD_CTX* c;
        ~ctx_guard() { EVP_MD_CTX_free(c); }
    } guard{ctx};
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) != 1) {
        return false;
    }
    return EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                            reinterpret_cast<const unsigned char*>(signed_bytes.data()),
                            signed_bytes.size()) == 1;
}

auto leaf_public_key() -> EVP_PKEY* {
    BIO* bio = BIO_new_mem_buf(leaf_cert_pem.data(), static_cast<int>(leaf_cert_pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);
    EVP_PKEY* key = X509_get_pubkey(cert);
    X509_free(cert);
    return key;
}

auto public_key_from_der_b64(const std::string& der_b64) -> EVP_PKEY* {
    std::string der(3 * (der_b64.size() / 4), '\0');
    const int decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(der.data()),
                                        reinterpret_cast<const unsigned char*>(der_b64.data()),
                                        static_cast<int>(der_b64.size()));
    std::size_t pad = 0;
    for (auto it = der_b64.rbegin(); it != der_b64.rend() && *it == '='; ++it) {
        ++pad;
    }
    der.resize(static_cast<std::size_t>(decoded) - pad);
    const auto* p = reinterpret_cast<const unsigned char*>(der.data());
    return d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size()));  // NOLINT(google-runtime-int)
}

/// The metadata service and the Auth service on one listener. Enforces the
/// `Bearer Oracle` header on every metadata path exactly the way the real v2
/// service does — a client that forgot it passes every *local* test that
/// doesn't enforce this, and then 401s on every real instance, which is the
/// mock-blindness pattern this spec keeps documenting.
class federation_server {
public:
    explicit federation_server(std::time_t token_exp) : _token_exp(token_exp) {
        _server.Get(R"(/opc/v2/.*)", [this](const httplib::Request& req, httplib::Response& resp) {
            if (req.get_header_value("Authorization") != "Bearer Oracle") {
                resp.status = 401;
                resp.set_content("Authorization: Bearer Oracle required", "text/plain");
                return;
            }
            const std::string path = req.path;
            if (path == "/opc/v2/instance/region") {
                resp.set_content("phx", "text/plain");
            } else if (path == "/opc/v2/identity/cert.pem") {
                resp.set_content(leaf_cert_pem, "text/plain");
            } else if (path == "/opc/v2/identity/key.pem") {
                resp.set_content(leaf_key_pem, "text/plain");
            } else if (path == "/opc/v2/identity/intermediate.pem") {
                resp.set_content(intermediate_cert_pem, "text/plain");
            } else {
                resp.status = 404;
            }
        });
        _server.Post("/v1/x509", [this](const httplib::Request& req, httplib::Response& resp) {
            {
                const std::lock_guard<std::mutex> lock(_mutex);
                _exchanges.push_back(req);
            }
            resp.set_content(R"({"token":")" + make_token(_token_exp) + R"("})",
                             "application/json");
        });
        // A stand-in API endpoint for the oci_http_client end-to-end case.
        _server.Get("/20160918/instancePools/pool-1", [this](const httplib::Request& req,
                                                             httplib::Response& resp) {
            {
                const std::lock_guard<std::mutex> lock(_mutex);
                _api_calls.push_back(req);
            }
            resp.set_content(R"({"id":"pool-1","lifecycleState":"RUNNING"})", "application/json");
        });
    }

    auto start() -> void {
        _thread = std::thread([this] { _server.listen(bind_address, test_port); });
        _server.wait_until_ready();
    }

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    [[nodiscard]] auto exchange_count() const -> std::size_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _exchanges.size();
    }
    [[nodiscard]] auto exchange_at(std::size_t i) const -> httplib::Request {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _exchanges.at(i);
    }
    [[nodiscard]] auto api_call_count() const -> std::size_t {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _api_calls.size();
    }
    [[nodiscard]] auto api_call_at(std::size_t i) const -> httplib::Request {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _api_calls.at(i);
    }

private:
    httplib::Server _server;
    std::thread _thread;
    std::time_t _token_exp;
    mutable std::mutex _mutex;
    std::vector<httplib::Request> _exchanges;
    std::vector<httplib::Request> _api_calls;
};

auto signer_options() -> federation::instance_principal_signer::options {
    federation::instance_principal_signer::options opts;
    opts.metadata_base_url = test_origin + "/opc/v2";
    opts.auth_endpoint_override = test_origin;
    opts.timeout = std::chrono::seconds{5};
    return opts;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oci_federation_tests)

/// The sanitized forms are what the federation body carries — armor and
/// newlines gone, base64 payload intact, and a bundle split per certificate
/// rather than spliced into one broken string.
BOOST_AUTO_TEST_CASE(pem_sanitization_and_bundle_splitting) {
    const auto sanitized = federation::sanitize_pem(leaf_cert_pem);
    BOOST_CHECK(sanitized.find("BEGIN") == std::string::npos);
    BOOST_CHECK(sanitized.find('\n') == std::string::npos);
    BOOST_CHECK(sanitized.starts_with("MIIEGzCC"));

    BOOST_CHECK_EQUAL(federation::sanitize_pem("-----BEGIN PUBLIC KEY-----\nQUJD\n-----END "
                                               "PUBLIC KEY-----\n"),
                      "QUJD");

    const auto bundle = intermediate_cert_pem + leaf_cert_pem;
    const auto split = federation::split_pem_certificates(bundle);
    BOOST_REQUIRE_EQUAL(split.size(), 2);
    BOOST_CHECK(split.at(0).starts_with("-----BEGIN CERTIFICATE-----"));
    BOOST_CHECK(federation::sanitize_pem(split.at(1)).starts_with("MIIEGzCC"));
}

/// The keyId's fingerprint half, against openssl's own `-fingerprint -sha256`
/// output for the same certificate. Lowercase and colon-separated is part of
/// the contract (the go-sdk formats with `% x`), not a display choice.
BOOST_AUTO_TEST_CASE(certificate_fingerprint_matches_openssl) {
    BOOST_CHECK_EQUAL(federation::certificate_fingerprint(leaf_cert_pem),
                      expected_leaf_fingerprint);
}

/// The tenancy comes out of the leaf's subject, `opc-tenant:` prefix — and a
/// certificate without the attribute is an error, not an empty string that
/// would build a keyId starting with "/".
BOOST_AUTO_TEST_CASE(tenancy_extraction) {
    BOOST_CHECK_EQUAL(federation::extract_tenancy_ocid(leaf_cert_pem), expected_tenancy);
    BOOST_CHECK_THROW((void)federation::extract_tenancy_ocid(intermediate_cert_pem),
                      std::runtime_error);
}

/// `/instance/region` answers with a short code on OCI's four original
/// regions and the canonical name everywhere else; unknown short codes are an
/// error rather than a guessed hostname.
BOOST_AUTO_TEST_CASE(region_resolution_and_auth_host) {
    BOOST_CHECK_EQUAL(federation::resolve_region("us-phoenix-1"), "us-phoenix-1");
    BOOST_CHECK_EQUAL(federation::resolve_region("phx"), "us-phoenix-1");
    BOOST_CHECK_EQUAL(federation::resolve_region("iad"), "us-ashburn-1");
    BOOST_CHECK_THROW((void)federation::resolve_region("xyz"), std::runtime_error);
    // The bare domain form, not `.oci.` — per-service domains are per-service
    // (Finding 10) and the go-sdk derives auth endpoints without the label.
    BOOST_CHECK_EQUAL(federation::auth_host_for("us-phoenix-1"),
                      "auth.us-phoenix-1.oraclecloud.com");
}

/// The federation request's canonical string, byte for byte — and the one
/// property that distinguishes it from every other OCI request: **no `host`
/// line**. A signer that reused the ordinary five-header form would produce a
/// well-formed request the Auth service rejects with a 401 naming nothing.
BOOST_AUTO_TEST_CASE(federation_signing_string_has_no_host) {
    const std::string body = R"({"certificate":"abc"})";
    const auto signing_string = federation::build_federation_signing_string(
        "POST", "/v1/x509", body, "Mon, 10 Aug 2026 12:00:00 GMT");
    BOOST_CHECK_EQUAL(signing_string,
                      "date: Mon, 10 Aug 2026 12:00:00 GMT\n"
                      "(request-target): post /v1/x509\n"
                      "content-length: " +
                          std::to_string(body.size()) +
                          "\n"
                          "content-type: application/json\n"
                          "x-content-sha256: " +
                          kythira::oci_signing::detail::sha256_base64(body));
    BOOST_CHECK(signing_string.find("host") == std::string::npos);

    const auto names = federation::federation_signed_headers();
    const std::vector<std::string> expected{"date", "(request-target)", "content-length",
                                            "content-type", "x-content-sha256"};
    BOOST_CHECK_EQUAL_COLLECTIONS(names.begin(), names.end(), expected.begin(), expected.end());
}

/// `exp` parsing exercises the base64url alphabet specifically: the fixture
/// token's payload encodes to a string containing `-`, which standard base64
/// rejects — a decoder that conflated the two alphabets fails here and
/// nowhere else until a real token happens to contain the two characters.
BOOST_AUTO_TEST_CASE(jwt_exp_parsing) {
    const std::string token =
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJvY2lkMS5pbnN0YW5jZS5vYzEucGh4LnRlc3QiLCJleHAiOjIwMDAwMDAwMDAsImp0aSI6Ij4-"
        "PiJ9.c2ln";
    BOOST_REQUIRE(token.find('-') != std::string::npos);
    BOOST_CHECK_EQUAL(federation::parse_jwt_exp(token), 2000000000);

    BOOST_CHECK_THROW((void)federation::parse_jwt_exp("not-a-jwt"), std::runtime_error);
    BOOST_CHECK_THROW((void)federation::parse_jwt_exp(base64url_encode("{}") + "." +
                                                      base64url_encode(R"({"sub":"x"})") + ".s"),
                      std::runtime_error);
}

/// The expiry buffer: five minutes before `exp` the token is already treated
/// as dead, so a request signed at the boundary does not arrive expired.
BOOST_AUTO_TEST_CASE(token_validity_buffer) {
    const std::time_t now = 1'000'000;
    federation::security_token token{"raw", now + 301};
    BOOST_CHECK(token.valid(now));
    token.exp = now + 300;
    BOOST_CHECK(!token.valid(now));
    BOOST_CHECK(!federation::security_token{}.valid(now));
}

/// The full flow against the mock metadata + Auth services, with both
/// signatures verified cryptographically from what arrived on the wire.
BOOST_AUTO_TEST_CASE(end_to_end_exchange_and_session_signing) {
    federation_server server(std::time(nullptr) + 3600);
    server.start();

    federation::instance_principal_signer signer(signer_options());
    const std::string api_body = R"({"size":3})";
    const auto headers = signer.sign("POST", "/20160918/instancePools/pool-1/actions/resize",
                                     "iaas.us-phoenix-1.oraclecloud.com", api_body);
    // A second sign while the token is fresh must not re-exchange.
    const auto again = signer.sign("GET", "/20160918/instancePools/pool-1",
                                   "iaas.us-phoenix-1.oraclecloud.com", "");
    server.stop();

    BOOST_REQUIRE_EQUAL(server.exchange_count(), 1);
    BOOST_CHECK_EQUAL(signer.exchange_count(), 1);
    const auto exchange = server.exchange_at(0);

    // The federation request: keyId is {tenancy}/fed-x509-sha256/{fp} with
    // the openssl-oracle fingerprint, the signed set carries no host, and the
    // signature verifies against the *leaf* certificate's key over exactly
    // what hit the wire.
    const auto auth_header = exchange.get_header_value("authorization");
    BOOST_CHECK_EQUAL(auth_field(auth_header, "keyId"),
                      expected_tenancy + "/fed-x509-sha256/" + expected_leaf_fingerprint);
    BOOST_CHECK_EQUAL(auth_field(auth_header, "headers"),
                      "date (request-target) content-length content-type x-content-sha256");
    BOOST_CHECK_EQUAL(auth_field(auth_header, "algorithm"), "rsa-sha256");

    const auto federation_signed = federation::build_federation_signing_string(
        "POST", "/v1/x509", exchange.body, exchange.get_header_value("date"));
    EVP_PKEY* leaf_key = leaf_public_key();
    BOOST_CHECK(
        verify_signature(leaf_key, federation_signed, auth_field(auth_header, "signature")));
    EVP_PKEY_free(leaf_key);

    // The body: go-sdk field names, sanitized leaf, the intermediate as an
    // array entry, SHA256 declared.
    const auto body = boost::json::parse(exchange.body).as_object();
    BOOST_CHECK_EQUAL(std::string(body.at("certificate").as_string()),
                      federation::sanitize_pem(leaf_cert_pem));
    BOOST_CHECK_EQUAL(std::string(body.at("fingerprintAlgorithm").as_string()), "SHA256");
    BOOST_REQUIRE_EQUAL(body.at("intermediateCertificates").as_array().size(), 1);

    // The API request: ST$ keyId carrying the very token the exchange
    // returned, ordinary six-header signed set (host back in), and the
    // signature verifies against the session public key *from the exchange
    // body* — the pair OCI was told about must be the pair that signs.
    const auto api_auth = headers.at("authorization");
    BOOST_CHECK(auth_field(api_auth, "keyId").starts_with("ST$"));
    BOOST_CHECK_EQUAL(auth_field(api_auth, "headers"),
                      "date (request-target) host content-length content-type x-content-sha256");
    const auto canonical = kythira::oci_signing::build_signing_string(
        "POST", "/20160918/instancePools/pool-1/actions/resize",
        "iaas.us-phoenix-1.oraclecloud.com", api_body, headers.at("date"));
    EVP_PKEY* session_key = public_key_from_der_b64(std::string(body.at("publicKey").as_string()));
    BOOST_REQUIRE(session_key != nullptr);
    BOOST_CHECK(verify_signature(session_key, canonical, auth_field(api_auth, "signature")));
    EVP_PKEY_free(session_key);

    // The bodyless second call used the three-header form, same token.
    BOOST_CHECK_EQUAL(auth_field(again.at("authorization"), "headers"),
                      "date (request-target) host");
    BOOST_CHECK_EQUAL(auth_field(again.at("authorization"), "keyId"),
                      auth_field(api_auth, "keyId"));
}

/// Renewal: a token already inside the five-minute buffer is renewed on the
/// next sign, with a *fresh* session key pair — asserted by the two
/// exchanges' `publicKey` fields differing, which is the observable form of
/// "the session key has no lifetime beyond its token".
BOOST_AUTO_TEST_CASE(expired_token_triggers_renewal_with_fresh_session_key) {
    federation_server server(std::time(nullptr) + 120);  // < the 300s buffer
    server.start();

    federation::instance_principal_signer signer(signer_options());
    (void)signer.sign("GET", "/a", "iaas.us-phoenix-1.oraclecloud.com", "");
    (void)signer.sign("GET", "/b", "iaas.us-phoenix-1.oraclecloud.com", "");
    server.stop();

    BOOST_REQUIRE_EQUAL(server.exchange_count(), 2);
    BOOST_CHECK_EQUAL(signer.exchange_count(), 2);
    const auto first = boost::json::parse(server.exchange_at(0).body).as_object();
    const auto second = boost::json::parse(server.exchange_at(1).body).as_object();
    BOOST_CHECK(first.at("publicKey").as_string() != second.at("publicKey").as_string());
}

/// The whole stack through `oci_http_client`: a config with
/// `use_instance_principal` and *none* of the API-key fields drives a signed
/// request end to end, with the environment overrides doing what the
/// requirement says they do. This is the path a kythira process on a real
/// instance takes.
BOOST_AUTO_TEST_CASE(oci_http_client_routes_instance_principal) {
    federation_server server(std::time(nullptr) + 3600);
    server.start();

    setenv("OCI_METADATA_BASE_URL", (test_origin + "/opc/v2").c_str(), 1);
    setenv("OCI_SDK_AUTH_CLIENT_REGION_URL", test_origin.c_str(), 1);

    kythira::oci_client_config cfg;
    cfg.region = "us-phoenix-1";
    cfg.use_instance_principal = true;
    cfg.endpoint_override = test_origin;
    cfg.api_timeout = std::chrono::seconds{5};

    const kythira::oci_http_client client(cfg);
    const auto response = client.request("iaas", "GET", "/20160918/instancePools/pool-1");

    unsetenv("OCI_METADATA_BASE_URL");
    unsetenv("OCI_SDK_AUTH_CLIENT_REGION_URL");
    server.stop();

    BOOST_CHECK_EQUAL(std::string(response.as_object().at("lifecycleState").as_string()),
                      "RUNNING");
    BOOST_REQUIRE_EQUAL(server.api_call_count(), 1);
    const auto api = server.api_call_at(0);
    BOOST_CHECK(auth_field(api.get_header_value("authorization"), "keyId").starts_with("ST$"));
    // The signed-versus-sent Host invariant survives the new signer path.
    BOOST_CHECK_EQUAL(api.get_header_value("Host"),
                      bind_address + std::string(":") + std::to_string(test_port));
}

/// `sign_request` stays API-key-only, and says where Instance Principal went
/// rather than "not implemented" — the risk pinned here is a refactor making
/// `use_instance_principal` silently take the API-key path, which would
/// authenticate as the wrong principal instead of erroring.
BOOST_AUTO_TEST_CASE(sign_request_still_refuses_instance_principal) {
    kythira::oci_client_config cfg;
    cfg.use_instance_principal = true;
    BOOST_CHECK_EXCEPTION((void)kythira::oci_signing::sign_request(cfg, "GET", "/x", "h", ""),
                          std::runtime_error, [](const std::runtime_error& e) {
                              return std::string(e.what()).find("oci_federation") !=
                                     std::string::npos;
                          });
}

BOOST_AUTO_TEST_SUITE_END()
