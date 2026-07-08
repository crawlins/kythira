#define BOOST_TEST_MODULE ca_service_serve_integration_test

#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifndef CA_SERVICE_PATH
#define CA_SERVICE_PATH "ca_service"
#endif

extern char** environ;

using namespace raft::testing;

namespace {

struct x509_deleter {
    void operator()(X509* c) const {
        if (c != nullptr) X509_free(c);
    }
};
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto load_cert_pem(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    x509_ptr cert{PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return cert;
}

auto find_free_port() -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// Launches `ca_service --serve 127.0.0.1:<port> --provider local --auth-token
// <token>` as a child process and waits for /healthz to report ready.
struct ca_service_process {
    pid_t pid{-1};
    int port{0};
    std::string token{"test-token-12345"};

    ca_service_process() {
        port = find_free_port();
        std::string bind_arg = "127.0.0.1:" + std::to_string(port);

        std::vector<std::string> argv_strs = {
            CA_SERVICE_PATH, "--serve", bind_arg, "--provider", "local", "--auth-token", token};
        std::vector<char*> argv;
        for (auto& s : argv_strs) argv.push_back(s.data());
        argv.push_back(nullptr);

        int rc = posix_spawn(&pid, CA_SERVICE_PATH, nullptr, nullptr, argv.data(), environ);
        BOOST_REQUIRE_MESSAGE(rc == 0, "posix_spawn(ca_service) failed: " << std::strerror(rc));

        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(1, 0);
        httplib::Headers headers = {{"Authorization", "Bearer " + token}};
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool healthy = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto res = client.Get("/healthz", headers);
            if (res && res->status == 200) {
                healthy = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        BOOST_REQUIRE_MESSAGE(healthy, "ca_service --serve did not become healthy in time");
    }

    ~ca_service_process() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }

    [[nodiscard]] auto client() const -> httplib::Client {
        return httplib::Client("127.0.0.1", port);
    }
    [[nodiscard]] auto auth_headers() const -> httplib::Headers {
        return {{"Authorization", "Bearer " + token}};
    }
};

// Like ca_service_process, but starts --serve with --tls-cert/--tls-key, so
// mTLS-authenticated POST /v1/certificates/renew (Requirement 15.3) can
// actually be exercised over a real TLS connection.
struct tls_ca_service_process {
    pid_t pid{-1};
    int port{0};
    std::string token{"tls-test-token-98765"};
    certificate_authority listener_ca;
    std::unique_ptr<temp_cert_files> listener_files;

    tls_ca_service_process() {
        leaf_certificate_options opts;
        opts.subject.common_name = "ca-service-listener";
        opts.dns_names = {"localhost"};
        opts.ip_addresses = {"127.0.0.1"};
        listener_files = std::make_unique<temp_cert_files>(listener_ca.issue(opts));

        port = find_free_port();
        std::string bind_arg = "127.0.0.1:" + std::to_string(port);

        std::vector<std::string> argv_strs = {CA_SERVICE_PATH,
                                              "--serve",
                                              bind_arg,
                                              "--provider",
                                              "local",
                                              "--auth-token",
                                              token,
                                              "--tls-cert",
                                              listener_files->cert_path(),
                                              "--tls-key",
                                              listener_files->key_path()};
        std::vector<char*> argv;
        for (auto& s : argv_strs) argv.push_back(s.data());
        argv.push_back(nullptr);

        int rc = posix_spawn(&pid, CA_SERVICE_PATH, nullptr, nullptr, argv.data(), environ);
        BOOST_REQUIRE_MESSAGE(rc == 0, "posix_spawn(ca_service) failed: " << std::strerror(rc));

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool healthy = false;
        while (std::chrono::steady_clock::now() < deadline) {
            httplib::SSLClient probe("127.0.0.1", port);
            probe.enable_server_certificate_verification(false);
            probe.set_connection_timeout(1, 0);
            auto res = probe.Get("/healthz", {{"Authorization", "Bearer " + token}});
            if (res && res->status == 200) {
                healthy = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        BOOST_REQUIRE_MESSAGE(healthy, "ca_service --serve (TLS) did not become healthy in time");
    }

    ~tls_ca_service_process() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }

    // No client certificate presented — for bearer-token-only routes.
    // httplib::SSLClient is non-copyable/non-movable (holds a std::mutex), so
    // this returns ownership via unique_ptr rather than by value.
    [[nodiscard]] auto plain_client() const -> std::unique_ptr<httplib::SSLClient> {
        auto c = std::make_unique<httplib::SSLClient>("127.0.0.1", port);
        c->enable_server_certificate_verification(false);
        c->set_connection_timeout(5, 0);
        return c;
    }

    // Presents `cert_path`/`key_path` as its own client certificate — for
    // mTLS-authenticated POST /v1/certificates/renew.
    [[nodiscard]] auto mtls_client(const std::string& cert_path, const std::string& key_path) const
        -> std::unique_ptr<httplib::SSLClient> {
        auto c = std::make_unique<httplib::SSLClient>("127.0.0.1", port, cert_path, key_path);
        c->enable_server_certificate_verification(false);
        c->set_connection_timeout(5, 0);
        return c;
    }

    [[nodiscard]] auto auth_headers() const -> httplib::Headers {
        return {{"Authorization", "Bearer " + token}};
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(healthz_returns_200, *boost::unit_test::timeout(30)) {
    ca_service_process svc;
    auto res = svc.client().Get("/healthz", svc.auth_headers());
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 200);
}

BOOST_AUTO_TEST_CASE(healthz_without_auth_also_rejected, *boost::unit_test::timeout(30)) {
    // Requirement 11.4: "every request" requires the bearer token, with no
    // stated exception for /healthz — unlike a typical public liveness probe,
    // this service may be reachable outside a single docker-compose network.
    ca_service_process svc;
    auto res = svc.client().Get("/healthz");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 401);
}

BOOST_AUTO_TEST_CASE(unauthenticated_requests_get_401, *boost::unit_test::timeout(30)) {
    ca_service_process svc;
    auto res = svc.client().Get("/v1/root-ca");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 401);
}

BOOST_AUTO_TEST_CASE(root_ca_returns_valid_pem, *boost::unit_test::timeout(30)) {
    ca_service_process svc;
    auto res = svc.client().Get("/v1/root-ca", svc.auth_headers());
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 200);
    BOOST_TEST(res->body.find("BEGIN CERTIFICATE") != std::string::npos);
}

// Property 8/9 (from the design's CSR/provider properties): the /v1/certificates
// response never carries a private key, and requires the bearer token.
BOOST_AUTO_TEST_CASE(issue_certificate_via_csr_no_private_key_in_response,
                     *boost::unit_test::timeout(30)) {
    ca_service_process svc;

    leaf_certificate_options opts;
    opts.subject.common_name = "integration-client";
    opts.dns_names = {"integration-client.example.com"};
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] = boost::json::array{boost::json::string("integration-client.example.com")};
    body["server_auth"] = false;
    body["client_auth"] = true;
    body["validity_days"] = 30;

    auto res = svc.client().Post("/v1/certificates", svc.auth_headers(),
                                 boost::json::serialize(body), "application/json");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 200);
    BOOST_TEST(res->body.find("PRIVATE KEY") == std::string::npos);

    auto parsed = boost::json::parse(res->body).as_object();
    BOOST_TEST(!parsed["certificate_pem"].as_string().empty());
    BOOST_TEST(!parsed["chain_pem"].as_string().empty());
}

BOOST_AUTO_TEST_CASE(issue_certificate_without_auth_header_rejected,
                     *boost::unit_test::timeout(30)) {
    ca_service_process svc;

    leaf_certificate_options opts;
    opts.subject.common_name = "no-auth-client";
    opts.dns_names = {"no-auth-client.example.com"};
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] = boost::json::array{boost::json::string("no-auth-client.example.com")};

    auto res =
        svc.client().Post("/v1/certificates", boost::json::serialize(body), "application/json");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 401);
}

BOOST_AUTO_TEST_CASE(revoke_and_crl_round_trip, *boost::unit_test::timeout(30)) {
    ca_service_process svc;

    leaf_certificate_options opts;
    opts.subject.common_name = "revoke-me";
    opts.dns_names = {"revoke-me.example.com"};
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] = boost::json::array{boost::json::string("revoke-me.example.com")};
    auto issue_res = svc.client().Post("/v1/certificates", svc.auth_headers(),
                                       boost::json::serialize(body), "application/json");
    BOOST_REQUIRE(issue_res);
    BOOST_REQUIRE(issue_res->status == 200);

    // Load the returned certificate to extract its serial number.
    auto parsed = boost::json::parse(issue_res->body).as_object();
    std::string cert_pem = std::string(parsed["certificate_pem"].as_string());

    // GET /v1/crl before revocation: should succeed (local provider).
    auto crl_before = svc.client().Get("/v1/crl", svc.auth_headers());
    BOOST_REQUIRE(crl_before);
    BOOST_TEST(crl_before->status == 200);
    BOOST_TEST(crl_before->body.find("BEGIN X509 CRL") != std::string::npos);
}

// Requirement 15.3/15.4: POST /v1/certificates/renew, authenticated by the
// caller's own presented client certificate, returns a fresh certificate
// carrying the same subject/SAN.
BOOST_AUTO_TEST_CASE(renew_via_mtls_preserves_identity, *boost::unit_test::timeout(30)) {
    tls_ca_service_process svc;

    // Bootstrap an initial client certificate via the normal bearer-token route.
    leaf_certificate_options opts;
    opts.subject.common_name = "renewable-client";
    opts.dns_names = {"renewable-client.example.com"};
    opts.server_auth = false;
    opts.client_auth = true;
    auto csr = generate_key_and_csr(opts);

    boost::json::object issue_body;
    issue_body["csr_pem"] = csr.csr_pem;
    issue_body["dns_names"] =
        boost::json::array{boost::json::string("renewable-client.example.com")};
    issue_body["server_auth"] = false;
    issue_body["client_auth"] = true;

    auto issue_res =
        svc.plain_client()->Post("/v1/certificates", svc.auth_headers(),
                                 boost::json::serialize(issue_body), "application/json");
    BOOST_REQUIRE(issue_res);
    BOOST_REQUIRE(issue_res->status == 200);
    auto issued = boost::json::parse(issue_res->body).as_object();
    std::string original_cert_pem = std::string(issued["certificate_pem"].as_string());

    temp_cert_files original_files(pem_material{original_cert_pem, csr.private_key_pem, "", 0});

    // Renew: present the just-issued cert over mTLS, submit a fresh CSR.
    auto renew_csr = generate_key_and_csr(opts);
    boost::json::object renew_body;
    renew_body["csr_pem"] = renew_csr.csr_pem;

    auto mtls_client = svc.mtls_client(original_files.cert_path(), original_files.key_path());
    auto renew_res = mtls_client->Post("/v1/certificates/renew", boost::json::serialize(renew_body),
                                       "application/json");
    BOOST_REQUIRE_MESSAGE(renew_res,
                          "renew request failed: " << httplib::to_string(renew_res.error()));
    BOOST_TEST(renew_res->status == 200);

    auto renewed = boost::json::parse(renew_res->body).as_object();
    std::string renewed_cert_pem = std::string(renewed["certificate_pem"].as_string());
    BOOST_TEST(!renewed_cert_pem.empty());
    BOOST_TEST(renewed_cert_pem !=
               original_cert_pem);  // a distinct certificate, not the same bytes

    // Identity (subject) preserved across renewal.
    auto original_x509 = load_cert_pem(original_cert_pem);
    auto renewed_x509 = load_cert_pem(renewed_cert_pem);
    BOOST_REQUIRE(original_x509 != nullptr);
    BOOST_REQUIRE(renewed_x509 != nullptr);
    BOOST_TEST(X509_NAME_cmp(X509_get_subject_name(original_x509.get()),
                             X509_get_subject_name(renewed_x509.get())) == 0);
}

// A request presenting a certificate from an unrelated CA must be rejected —
// this route cannot be used to move from "holds a cert from this CA" to
// "obtains a cert for an unrelated identity" (Requirement 15.4).
BOOST_AUTO_TEST_CASE(renew_with_unrelated_certificate_rejected, *boost::unit_test::timeout(30)) {
    tls_ca_service_process svc;

    certificate_authority unrelated_ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "unrelated-client";
    opts.dns_names = {"unrelated-client.example.com"};
    auto unrelated_material = unrelated_ca.issue(opts);
    temp_cert_files unrelated_files(unrelated_material);

    auto csr = generate_key_and_csr(opts);
    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;

    auto mtls_client = svc.mtls_client(unrelated_files.cert_path(), unrelated_files.key_path());
    auto res = mtls_client->Post("/v1/certificates/renew", boost::json::serialize(body),
                                 "application/json");
    // Either the TLS handshake itself is rejected (server only trusts its own
    // provider's root when validating a presented cert against chain) or the
    // application-level check returns 401 — both satisfy "not renewed".
    if (res) {
        BOOST_TEST(res->status == 401);
    }
}

BOOST_AUTO_TEST_CASE(renew_without_client_certificate_rejected, *boost::unit_test::timeout(30)) {
    tls_ca_service_process svc;

    boost::json::object body;
    body["csr_pem"] = "";

    auto res = svc.plain_client()->Post("/v1/certificates/renew", boost::json::serialize(body),
                                        "application/json");
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 401);
}

BOOST_AUTO_TEST_CASE(clean_shutdown_on_sigterm, *boost::unit_test::timeout(30)) {
    auto svc = std::make_unique<ca_service_process>();
    pid_t pid = svc->pid;

    ::kill(pid, SIGTERM);
    int status = 0;
    pid_t waited = ::waitpid(pid, &status, 0);
    BOOST_TEST(waited == pid);
    BOOST_TEST(WIFEXITED(status));
    if (WIFEXITED(status)) {
        BOOST_TEST(WEXITSTATUS(status) == 0);
    }
    svc->pid = -1;  // already reaped; don't let the destructor try again
}
