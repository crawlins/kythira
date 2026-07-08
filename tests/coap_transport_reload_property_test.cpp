#define BOOST_TEST_MODULE coap_transport_reload_property_test

#include <boost/test/unit_test.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/test_types.hpp>
#include "ca_test_fixture.hpp"

#include <chrono>
#include <fstream>
#include <thread>

using namespace kythira;
using namespace raft::testing;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";

using test_types = kythira::test_transport_types<json_serializer>;

void replace_file(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    std::filesystem::rename(tmp, path);
}

auto read_file(const std::string& path) -> std::string {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// A successful reload_tls_material() call re-invokes coap_context_set_pki() on
// the live context without tearing it down (Requirement 16.2/16.4).
BOOST_AUTO_TEST_CASE(coap_server_reload_succeeds_with_valid_material,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    const auto& files =
        fixture.bootstrap_client("coap-reload-server", {"localhost"}, {"127.0.0.1"});

    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;  // no CA/client cert needed for this reload-only test

    test_types::metrics_type metrics;
    coap_server<test_types> server(test_bind_address, 18701, config, metrics);

    // Re-write the same (still valid) cert/key bytes in place via an atomic
    // rename, standing in for a real renewal (ca_test_fixture::renew(), task 20).
    replace_file(files.cert_path(), read_file(files.cert_path()));
    replace_file(files.key_path(), read_file(files.key_path()));

    BOOST_CHECK_NO_THROW(server.reload_tls_material());
}

// reload_tls_material() rejects unparseable material and requires cert_file
// to have been configured in the first place (Requirement 16.3).
BOOST_AUTO_TEST_CASE(coap_server_reload_rejects_invalid_material, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    const auto& files =
        fixture.bootstrap_client("coap-reload-invalid", {"localhost"}, {"127.0.0.1"});

    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;

    test_types::metrics_type metrics;
    coap_server<test_types> server(test_bind_address, 18702, config, metrics);

    auto original_cert = read_file(files.cert_path());
    replace_file(files.cert_path(), "NOT A VALID CERTIFICATE");

    BOOST_CHECK_THROW(server.reload_tls_material(), std::exception);

    replace_file(files.cert_path(), original_cert);
}

BOOST_AUTO_TEST_CASE(coap_server_reload_requires_cert_configured, *boost::unit_test::timeout(30)) {
    coap_server_config config;
    config.enable_dtls = false;  // no cert_file/key_file configured

    test_types::metrics_type metrics;
    coap_server<test_types> server(test_bind_address, 18703, config, metrics);

    BOOST_CHECK_THROW(server.reload_tls_material(), std::logic_error);
}

// enable_auto_reload()/disable_auto_reload() start and cleanly stop a
// background poll thread without disturbing the server (Requirement 16.5/16.6).
BOOST_AUTO_TEST_CASE(coap_server_auto_reload_starts_and_stops_cleanly,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    const auto& files = fixture.bootstrap_client("coap-auto-reload", {"localhost"}, {"127.0.0.1"});

    coap_server_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;

    test_types::metrics_type metrics;
    coap_server<test_types> server(test_bind_address, 18704, config, metrics);

    server.enable_auto_reload(std::chrono::seconds(1));

    auto cert_content = read_file(files.cert_path());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    replace_file(files.cert_path(), cert_content);

    std::this_thread::sleep_for(std::chrono::seconds(3));  // bounded number of poll intervals

    BOOST_CHECK_NO_THROW(server.disable_auto_reload());
}

// Same coverage for coap_client's own presented certificate under mutual DTLS.
BOOST_AUTO_TEST_CASE(coap_client_reload_succeeds_with_valid_material,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    const auto& files = fixture.bootstrap_client("coap-reload-client", {"client.example.com"});

    coap_client_config config;
    config.enable_dtls = true;
    config.cert_file = files.cert_path();
    config.key_file = files.key_path();
    config.verify_peer_cert = false;

    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[1] = "coaps://127.0.0.1:5684";

    test_types::metrics_type metrics;
    coap_client<test_types> client(endpoints, config, metrics);

    replace_file(files.cert_path(), read_file(files.cert_path()));
    replace_file(files.key_path(), read_file(files.key_path()));

    BOOST_CHECK_NO_THROW(client.reload_tls_material());

    client.disable_auto_reload();  // no-op, never enabled — exercises the joinable() guard
}
