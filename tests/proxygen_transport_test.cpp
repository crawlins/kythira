#define BOOST_TEST_MODULE proxygen_transport_test
#include <boost/test/unit_test.hpp>

#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>

#include "test_timeout_scale.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 18199;
constexpr std::uint16_t test_tls_bind_port = 18200;
constexpr std::uint16_t test_multi_bind_port_base = 18210;
constexpr const char* test_server_url = "http://127.0.0.1:18199";
constexpr std::uint64_t test_node_id = 1;

// Requirement 12.6/19.3: a metrics implementation that records every
// emitted metric name/dimension pair, so a test can positively confirm
// *which* internal path (Folly fast path vs. generic bridge) a given RPC
// actually took -- success alone doesn't distinguish that (Requirement
// 19.3's own point). Copyable (proxygen_client/server copy `_metrics` per
// call, `auto metric = _metrics;`), backed by a shared_ptr so every copy
// still records into the same log.
class recording_metrics {
public:
    struct entry {
        std::string name;
        std::unordered_map<std::string, std::string> dimensions;
    };

    recording_metrics() : _state(std::make_shared<state>()) {}

    auto set_metric_name(std::string_view name) -> void { _current.name = std::string(name); }
    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        _current.dimensions[std::string(dimension_name)] = std::string(dimension_value);
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->entries.push_back(_current);
        _current = entry{};
    }

    [[nodiscard]] auto entries_named(std::string_view name) const -> std::vector<entry> {
        std::lock_guard<std::mutex> lock(_state->mutex);
        std::vector<entry> result;
        for (const auto& e : _state->entries) {
            if (e.name == name) {
                result.push_back(e);
            }
        }
        return result;
    }

private:
    struct state {
        std::mutex mutex;
        std::vector<entry> entries;
    };
    std::shared_ptr<state> _state;
    entry _current;
};

static_assert(kythira::metrics<recording_metrics>);

using test_transport_types =
    kythira::future_default_proxygen_transport_types<kythira::json_serializer, recording_metrics,
                                                     kythira::executor_default>;

auto register_echo_handlers(kythira::proxygen_server<test_transport_types>& server) -> void {
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            kythira::request_vote_response<> resp{};
            resp._term = req.term();
            resp._vote_granted = true;
            return resp;
        });
    server.register_append_entries_handler(
        [](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
            kythira::append_entries_response<> resp{};
            resp._term = req.term();
            resp._success = true;
            return resp;
        });
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            kythira::install_snapshot_response<> resp{};
            resp._term = req.term();
            return resp;
        });
}

// Real, valid self-signed cert/key pair via the `openssl` CLI, matching
// beast_transport_test.cpp's own temp_tls_material -- exercises a genuine
// TLS handshake through folly::SSLContext/wangle::SSLContextConfig, not
// just config-field validation.
struct temp_tls_material {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    temp_tls_material() {
        auto dir = std::filesystem::temp_directory_path();
        auto unique = std::to_string(std::random_device{}());
        cert_path = dir / ("proxygen_test_cert_" + unique + ".pem");
        key_path = dir / ("proxygen_test_key_" + unique + ".pem");
        // P-256 rather than RSA-2048. Both give a genuine handshake, which is
        // all this fixture is for, but their generation costs are nothing
        // alike. Measured on 2 pinned cores under 8x busy-loop load, which is
        // the closest local stand-in for a contended CI runner:
        //
        //     rsa:2048      741 - 2966 ms   (one sample alone consumed
        //                                    essentially the whole 3000ms RPC
        //                                    deadline these tests use)
        //     prime256v1     65 -  170 ms
        //
        // ~15x faster and, more importantly, a far tighter spread. Keygen is
        // the dominant cost and the dominant variance source in every TLS case
        // in this file, and it buys the tests nothing.
        std::string cmd =
            "openssl req -x509 -newkey ec -pkeyopt "
            "ec_paramgen_curve:prime256v1 -keyout " +
            key_path.string() + " -out " + cert_path.string() +
            " -days 1 -nodes -subj \"/CN=127.0.0.1\" -addext "
            "\"subjectAltName=IP:127.0.0.1\" 2>/dev/null";
        int rc = std::system(cmd.c_str());
        BOOST_REQUIRE_MESSAGE(rc == 0,
                              "openssl CLI must be available to generate test TLS material");
        BOOST_REQUIRE(std::filesystem::exists(cert_path));
        BOOST_REQUIRE(std::filesystem::exists(key_path));
    }

    ~temp_tls_material() {
        std::error_code ec;
        std::filesystem::remove(cert_path, ec);
        std::filesystem::remove(key_path, ec);
    }
};

// Requirement 6.5/11's mutual-TLS gap (tasks.md Known Follow-ups #3): a real
// CA plus a server leaf cert and a client leaf cert, both signed by that CA
// -- generated via the `openssl` CLI (not `ca_test_fixture.hpp`'s
// certificate-authority-spec fixture, deliberately, so this test file stays
// self-contained the way `temp_tls_material` above already is, and doesn't
// pull the certificate-authority feature in as a new dependency of the
// proxygen-http-transport test binary). `proxygen_client` (unlike
// `cpp_httplib_client`) actually loads `client_cert_path`/`client_key_path`
// into its own `folly::SSLContext` (`build_ssl_context()`,
// proxygen_http_transport_impl.hpp), so a genuine end-to-end mutual-TLS
// handshake is exercisable through this project's own client, not just a
// raw test-only TLS client the way http_ssl_mutual_tls_integration_test.cpp
// needs for cpp-httplib.
struct temp_mtls_material {
    std::filesystem::path ca_key_path;
    std::filesystem::path ca_cert_path;
    std::filesystem::path server_key_path;
    std::filesystem::path server_cert_path;
    std::filesystem::path client_key_path;
    std::filesystem::path client_cert_path;

    temp_mtls_material() {
        auto dir = std::filesystem::temp_directory_path();
        auto unique = std::to_string(std::random_device{}());
        ca_key_path = dir / ("proxygen_mtls_ca_key_" + unique + ".pem");
        ca_cert_path = dir / ("proxygen_mtls_ca_cert_" + unique + ".pem");
        server_key_path = dir / ("proxygen_mtls_server_key_" + unique + ".pem");
        server_cert_path = dir / ("proxygen_mtls_server_cert_" + unique + ".pem");
        client_key_path = dir / ("proxygen_mtls_client_key_" + unique + ".pem");
        client_cert_path = dir / ("proxygen_mtls_client_cert_" + unique + ".pem");
        auto server_csr_path = dir / ("proxygen_mtls_server_csr_" + unique + ".pem");
        auto client_csr_path = dir / ("proxygen_mtls_client_csr_" + unique + ".pem");
        auto server_ext_path = dir / ("proxygen_mtls_server_ext_" + unique + ".cnf");
        auto client_ext_path = dir / ("proxygen_mtls_client_ext_" + unique + ".cnf");

        // Explicit extensions on every cert -- relying on the local
        // openssl.cnf's own defaults (which vary by distro, and commonly
        // don't set CA:true on a bare `req -x509` self-signed cert) was
        // found, via a real CI failure, to produce a CA certificate X.509
        // path validation rejects as an invalid issuer ("SSL alert bad
        // certificate" during the handshake) -- basicConstraints=CA:true
        // is what makes a self-signed cert usable as a CA for verifying
        // *other* certificates, not just terminating TLS itself.
        // extendedKeyUsage is set defensively on both leaf certs to match
        // what a strict TLS 1.3 stack may expect for each cert's actual
        // role (serverAuth/clientAuth), rather than relying on it being
        // unchecked.
        {
            std::ofstream ext(server_ext_path);
            ext << "basicConstraints=critical,CA:false\n"
                << "keyUsage=critical,digitalSignature\n"
                << "extendedKeyUsage=serverAuth\n"
                << "subjectAltName=IP:127.0.0.1\n";
        }
        {
            std::ofstream ext(client_ext_path);
            ext << "basicConstraints=critical,CA:false\n"
                << "keyUsage=critical,digitalSignature\n"
                << "extendedKeyUsage=clientAuth\n";
        }

        auto run = [](const std::string& cmd) {
            int rc = std::system((cmd + " >/dev/null 2>&1").c_str());
            BOOST_REQUIRE_MESSAGE(rc == 0, "openssl command failed: " << cmd);
        };

        run("openssl ecparam -genkey -name prime256v1 -out " + ca_key_path.string());
        run("openssl req -x509 -new -key " + ca_key_path.string() + " -out " +
            ca_cert_path.string() +
            " -days 1 -nodes -subj \"/CN=proxygen-test-ca\" "
            "-addext \"basicConstraints=critical,CA:true\" "
            "-addext \"keyUsage=critical,keyCertSign,cRLSign\"");

        run("openssl ecparam -genkey -name prime256v1 -out " + server_key_path.string());
        run("openssl req -new -key " + server_key_path.string() + " -out " +
            server_csr_path.string() + " -subj \"/CN=127.0.0.1\"");
        run("openssl x509 -req -in " + server_csr_path.string() + " -CA " + ca_cert_path.string() +
            " -CAkey " + ca_key_path.string() + " -CAcreateserial -out " +
            server_cert_path.string() + " -days 1 -extfile " + server_ext_path.string());

        run("openssl ecparam -genkey -name prime256v1 -out " + client_key_path.string());
        run("openssl req -new -key " + client_key_path.string() + " -out " +
            client_csr_path.string() + " -subj \"/CN=proxygen-test-client\"");
        run("openssl x509 -req -in " + client_csr_path.string() + " -CA " + ca_cert_path.string() +
            " -CAkey " + ca_key_path.string() + " -CAcreateserial -out " +
            client_cert_path.string() + " -days 1 -extfile " + client_ext_path.string());

        std::error_code ec;
        std::filesystem::remove(server_csr_path, ec);
        std::filesystem::remove(client_csr_path, ec);
        std::filesystem::remove(server_ext_path, ec);
        std::filesystem::remove(client_ext_path, ec);
        std::filesystem::remove(dir / (ca_cert_path.stem().string() + ".srl"), ec);
        BOOST_REQUIRE(std::filesystem::exists(ca_cert_path));
        BOOST_REQUIRE(std::filesystem::exists(server_cert_path));
        BOOST_REQUIRE(std::filesystem::exists(client_cert_path));
    }

    ~temp_mtls_material() {
        std::error_code ec;
        for (const auto& p : {ca_key_path, ca_cert_path, server_key_path, server_cert_path,
                              client_key_path, client_cert_path}) {
            std::filesystem::remove(p, ec);
        }
    }
};

// Requirement 10.1-10.2's timeout-enforcement gap (tasks.md Known
// Follow-ups #3): a bare POSIX listener that accepts a TCP connection and
// then never reads or writes anything -- simulating a peer that is
// reachable but never responds, so an RPC sent to it can only ever
// complete via `HTTPTransaction::setIdleTimeout` actually firing, not via
// any other failure path (connection refused, DNS failure, etc.).
// Deliberately raw sockets, not another kythira transport, so this test
// doesn't depend on any of the three transports' own timeout behavior
// being correct as a precondition for testing Proxygen's.
class blackhole_listener {
public:
    explicit blackhole_listener(std::uint16_t port) {
        _fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_REQUIRE(_fd >= 0);
        int reuse = 1;
        ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_REQUIRE(::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        BOOST_REQUIRE(::listen(_fd, 16) == 0);
        _accept_thread = std::thread([this] {
            while (!_stop.load()) {
                sockaddr_in peer{};
                socklen_t peer_len = sizeof(peer);
                int client_fd = ::accept(_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
                if (client_fd < 0) {
                    break;  // ::close(_fd) below unblocks a pending accept().
                }
                // Never read/write/close -- the peer sees a connection that
                // was accepted but that never sends a response.
                std::lock_guard<std::mutex> lock(_mutex);
                _accepted_fds.push_back(client_fd);
            }
        });
    }

    ~blackhole_listener() {
        _stop.store(true);
        ::shutdown(_fd, SHUT_RDWR);
        ::close(_fd);
        if (_accept_thread.joinable()) {
            _accept_thread.join();
        }
        std::lock_guard<std::mutex> lock(_mutex);
        for (int fd : _accepted_fds) {
            ::close(fd);
        }
    }

private:
    int _fd{-1};
    std::atomic<bool> _stop{false};
    std::thread _accept_thread;
    std::mutex _mutex;
    std::vector<int> _accepted_fds;
};

}  // namespace

static_assert(kythira::proxygen_future_default_transport_types<test_transport_types>);
static_assert(kythira::network_client<kythira::proxygen_client<test_transport_types>>);
static_assert(kythira::network_server<kythira::proxygen_server<test_transport_types>>);

// Requirement 15.2: the narrower concept proxygen_client/server actually
// require must reject a Types bundle that merely satisfies transport_types
// but doesn't pin future_template to kythira::future_default.
using http_transport_types_for_proxygen_test =
    kythira::http_transport_types<kythira::json_serializer, recording_metrics,
                                  kythira::executor_default>;
static_assert(kythira::transport_types<http_transport_types_for_proxygen_test>);
static_assert(
    !kythira::proxygen_future_default_transport_types<http_transport_types_for_proxygen_test>);

BOOST_AUTO_TEST_SUITE(proxygen_transport_tests)

BOOST_AUTO_TEST_CASE(request_vote_round_trip_and_connection_reuse,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(2);

    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, test_bind_port, {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 42;
    auto resp = std::move(client.send_request_vote(test_node_id, req,
                                                   kythira::testing::scaled_deadline(3000)))
                    .get();
    BOOST_TEST(resp.term() == 42);
    BOOST_TEST(resp.vote_granted());

    // Second RPC to the same node -- exercises connection reuse (Property 5).
    auto resp2 = std::move(client.send_request_vote(test_node_id, req,
                                                    kythira::testing::scaled_deadline(3000)))
                     .get();
    BOOST_TEST(resp2.vote_granted());

    server.stop();
}

// Requirement 19.3/Property 11: confirms `send_rpc`'s `if constexpr`
// dispatch (Requirement 16.1) actually took the path Property 11 predicts
// -- not merely that the RPC succeeded -- via Requirement 12.6's metrics
// path label, the one compile-time-visible signal that distinguishes the
// two internal code paths from the outside. `expects_folly_fast_path`
// is computed the same way `send_rpc`'s own dispatch condition is
// (`std::same_as<future_template<Response>, kythira::Future<Response>>`),
// not hardcoded to "folly_fast_path" -- under this project's default
// `KYTHIRA_DEFAULT_FUTURE_BACKEND=folly`, `test_transport_types`'s
// `future_template` resolves to `kythira::future_default<T>` =
// `kythira::Future<T>` and this evaluates true; built instead under
// `=stdexec`/`=boost`, `future_default_proxygen_transport_types`'s
// `future_template` resolves to `kythira::stdexec_backend::Future<T>`/
// `kythira::boost_backend::Future<T>` instead, this evaluates false, and
// the very same test then confirms the generic bridge was used instead
// (Requirement 19.3's other, previously-untested direction) without
// needing a second, backend-specific test file.
BOOST_AUTO_TEST_CASE(folly_fast_path_is_taken,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    constexpr bool expects_folly_fast_path =
        std::same_as<test_transport_types::future_template<kythira::request_vote_response<>>,
                     kythira::Future<kythira::request_vote_response<>>>;

    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 1);

    recording_metrics server_metrics;
    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, {}, server_metrics,
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    recording_metrics client_metrics;
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          client_metrics);

    kythira::request_vote_request<> req{};
    req._term = 1;
    auto resp = std::move(client.send_request_vote(test_node_id, req,
                                                   kythira::testing::scaled_deadline(3000)))
                    .get();
    BOOST_TEST(resp.vote_granted());

    auto sent = client_metrics.entries_named("proxygen_http.client.request.sent");
    BOOST_REQUIRE(!sent.empty());
    BOOST_TEST(sent.front().dimensions.at("path") ==
               std::string(expects_folly_fast_path ? "folly_fast_path" : "generic_bridge"));

    server.stop();
}

// Property 6: concurrent RPCs to different target nodes run genuinely
// concurrently, potentially on different EventBase threads of the shared
// folly::IOThreadPoolExecutor, and each gets the right response back.
BOOST_AUTO_TEST_CASE(concurrent_rpcs_to_multiple_nodes,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(4);

    constexpr int node_count = 4;
    std::vector<std::unique_ptr<kythira::proxygen_server<test_transport_types>>> servers;
    std::unordered_map<std::uint64_t, std::string> node_map;
    for (int i = 0; i < node_count; ++i) {
        auto port = static_cast<std::uint16_t>(test_multi_bind_port_base + i);
        auto server = std::make_unique<kythira::proxygen_server<test_transport_types>>(
            test_bind_address, port, kythira::proxygen_server_config{}, recording_metrics{},
            std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
        register_echo_handlers(*server);
        server->start();
        node_map[static_cast<std::uint64_t>(i + 1)] =
            std::string("http://127.0.0.1:") + std::to_string(port);
        servers.push_back(std::move(server));
    }

    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          recording_metrics{});

    std::vector<kythira::request_vote_response<>> responses(static_cast<std::size_t>(node_count));
    std::vector<std::thread> rpc_threads;
    for (int i = 0; i < node_count; ++i) {
        kythira::request_vote_request<> req{};
        req._term = static_cast<std::uint64_t>(100 + i);
        rpc_threads.emplace_back([&client, &responses, i, req] {
            responses[static_cast<std::size_t>(i)] =
                std::move(client.send_request_vote(static_cast<std::uint64_t>(i + 1), req,
                                                   kythira::testing::scaled_deadline(3000)))
                    .get();
        });
    }
    for (auto& t : rpc_threads) {
        t.join();
    }

    for (int i = 0; i < node_count; ++i) {
        const auto& resp = responses[static_cast<std::size_t>(i)];
        BOOST_TEST(resp.term() == static_cast<std::uint64_t>(100 + i));
        BOOST_TEST(resp.vote_granted());
    }

    for (auto& server : servers) {
        server->stop();
    }
}

BOOST_AUTO_TEST_CASE(tls_request_vote_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    temp_tls_material tls;
    folly::IOThreadPoolExecutor io_executor(2);

    kythira::proxygen_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, test_tls_bind_port, server_config, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    // enable_ssl_verification=false: this test's self-signed cert has no CA
    // to chain to -- exercising the encrypted-transport code path (SNI via
    // HTTPConnector::connectSSL, handshake, HTTPUpstreamSession reads/
    // writes) is the point here, not peer certificate validation.
    kythira::proxygen_client_config client_config;
    client_config.enable_ssl_verification = false;
    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(test_tls_bind_port)}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, client_config,
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 99;
    auto resp = std::move(client.send_request_vote(test_node_id, req,
                                                   kythira::testing::scaled_deadline(3000)))
                    .get();
    BOOST_TEST(resp.term() == 99);
    BOOST_TEST(resp.vote_granted());

    server.stop();
}

// Requirement 7.2-7.3: reload_tls_material() validates new material
// all-or-nothing; a server not configured for TLS at all rejects the call
// outright.
BOOST_AUTO_TEST_CASE(server_reload_tls_material,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(2);

    kythira::proxygen_server<test_transport_types> plain_server(
        test_bind_address, static_cast<std::uint16_t>(test_bind_port + 2), {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    BOOST_CHECK_THROW(plain_server.reload_tls_material(), std::exception);

    temp_tls_material tls;
    kythira::proxygen_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::proxygen_server<test_transport_types> tls_server(
        test_bind_address, static_cast<std::uint16_t>(test_bind_port + 3), server_config,
        recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    tls_server.start();
    BOOST_CHECK_NO_THROW(tls_server.reload_tls_material());

    std::filesystem::remove(tls.cert_path);
    BOOST_CHECK_THROW(tls_server.reload_tls_material(), std::exception);
    tls_server.stop();
}

// Requirement 7.1: the client side of the same all-or-nothing reload
// contract.
BOOST_AUTO_TEST_CASE(client_reload_tls_material,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(2);
    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};

    temp_tls_material tls;
    kythira::proxygen_client_config client_config;
    client_config.client_cert_path = tls.cert_path.string();
    client_config.client_key_path = tls.key_path.string();
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, client_config,
                                                          recording_metrics{});
    BOOST_CHECK_NO_THROW(client.reload_tls_material());

    std::filesystem::remove(tls.key_path);
    BOOST_CHECK_THROW(client.reload_tls_material(), std::exception);
}

// Requirement 4.2-4.4: malformed request body -> 400; unregistered path ->
// 404 (surfaced client-side as http_client_error).
BOOST_AUTO_TEST_CASE(malformed_request_handling,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 4);

    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    std::string response_body;
    unsigned status_code = 0;
    std::string response_media_type;
    // The server's own default media type and an empty Accept list -- what a
    // peer declaring neither header produces, which is the case these two
    // assertions were written against. An empty body is still bad *bytes*, not
    // a bad *format*, so it must reach the 400 and not the new 415.
    const std::string request_media_type = server.default_media_type();
    server.dispatch("/v1/raft/request_vote", std::vector<std::byte>{}, request_media_type, {},
                    response_body, status_code, response_media_type);
    BOOST_TEST(status_code == 400);

    server.dispatch("/v1/raft/unknown_endpoint", std::vector<std::byte>{}, request_media_type, {},
                    response_body, status_code, response_media_type);
    BOOST_TEST(status_code == 404);

    server.stop();
}

// Requirement 3.5/Property 6: concurrent RPCs to the *same* target node
// must not interleave their I/O -- unlike concurrent_rpcs_to_multiple_nodes
// above (different nodes, different connections/EventBases), this exercises
// the single shared HTTPUpstreamSession's own per-connection ordering
// (design.md's "Why folly::IOThreadPoolExecutor" section), relying on no
// explicit synchronization primitive of this feature's own construction --
// every operation against this node's connection is only ever invoked from
// that connection's own pinned EventBase, by Proxygen's own construction.
// Each concurrent RPC carries a distinct term value; if two in-flight
// requests' responses were ever crossed, at least one thread would observe
// a term that isn't its own.
BOOST_AUTO_TEST_CASE(concurrent_rpcs_to_same_node,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(4);
    auto port = static_cast<std::uint16_t>(test_bind_port + 5);

    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          recording_metrics{});

    constexpr int rpc_count = 16;
    std::vector<kythira::request_vote_response<>> responses(rpc_count);
    std::vector<std::exception_ptr> failures(rpc_count);
    std::vector<std::thread> rpc_threads;
    for (int i = 0; i < rpc_count; ++i) {
        kythira::request_vote_request<> req{};
        req._term = static_cast<std::uint64_t>(1000 + i);
        rpc_threads.emplace_back([&client, &responses, &failures, i, req] {
            // HTTPUpstreamSession (HTTP/1.1) carries only one in-flight
            // transaction at a time -- with 16 threads racing to reuse the
            // single pooled session for this node, newTransaction() can
            // legitimately return nullptr ("session unavailable for new
            // transaction") for whichever caller loses that race while
            // another request is still in flight. That is a real, timing-
            // dependent capacity limit of HTTP/1.1 session reuse, not a
            // correctness bug -- Property 6, which this test otherwise
            // verifies, is about response *content* never crossing between
            // callers, not about zero contention on a shared connection.
            // Retry a bounded number of times on specifically that
            // transient condition; any other exception fails the test
            // immediately, unretried, rather than being masked by a retry.
            constexpr int max_attempts = 10;
            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                try {
                    responses[static_cast<std::size_t>(i)] =
                        std::move(client.send_request_vote(test_node_id, req,
                                                           std::chrono::milliseconds(5000)))
                            .get();
                    return;
                } catch (const std::exception& e) {
                    bool retryable =
                        std::string(e.what()).find("session unavailable") != std::string::npos;
                    if (!retryable || attempt + 1 == max_attempts) {
                        failures[static_cast<std::size_t>(i)] = std::current_exception();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
    }
    for (auto& t : rpc_threads) {
        t.join();
    }

    for (int i = 0; i < rpc_count; ++i) {
        if (failures[static_cast<std::size_t>(i)]) {
            try {
                std::rethrow_exception(failures[static_cast<std::size_t>(i)]);
            } catch (const std::exception& e) {
                BOOST_ERROR("RPC " + std::to_string(i) + " failed: " + e.what());
            }
            continue;
        }
        BOOST_TEST(responses[static_cast<std::size_t>(i)].term() ==
                   static_cast<std::uint64_t>(1000 + i));
        BOOST_TEST(responses[static_cast<std::size_t>(i)].vote_granted());
    }

    server.stop();
}

// Requirement 6.5: mutual TLS -- require_client_cert=true actually enforced
// server-side (VerifyClientCertificate::ALWAYS, build_ssl_context_config()),
// and proxygen_client actually presents its own configured client
// certificate (build_ssl_context(), unlike cpp_httplib_client -- see
// temp_mtls_material's own comment above), so this is a genuine end-to-end
// handshake through this project's own client and server, not a
// config-validation-only check.
BOOST_AUTO_TEST_CASE(mutual_tls_round_trip_with_valid_client_certificate,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    temp_mtls_material mtls;
    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 6);

    kythira::proxygen_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = mtls.server_cert_path.string();
    server_config.ssl_key_path = mtls.server_key_path.string();
    server_config.ca_cert_path = mtls.ca_cert_path.string();
    server_config.require_client_cert = true;
    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, server_config, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    kythira::proxygen_client_config client_config;
    client_config.client_cert_path = mtls.client_cert_path.string();
    client_config.client_key_path = mtls.client_key_path.string();
    client_config.ca_cert_path = mtls.ca_cert_path.string();
    client_config.enable_ssl_verification = true;
    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, client_config,
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 7;
    auto resp = std::move(client.send_request_vote(test_node_id, req,
                                                   kythira::testing::scaled_deadline(3000)))
                    .get();
    BOOST_TEST(resp.term() == 7);
    BOOST_TEST(resp.vote_granted());

    server.stop();
}

// The same mutual-TLS server, but a client presenting no certificate at
// all -- the handshake itself must fail (require_client_cert's whole
// point), not just "the config validates".
BOOST_AUTO_TEST_CASE(mutual_tls_rejects_client_without_certificate,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    temp_mtls_material mtls;
    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 7);

    kythira::proxygen_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = mtls.server_cert_path.string();
    server_config.ssl_key_path = mtls.server_key_path.string();
    server_config.ca_cert_path = mtls.ca_cert_path.string();
    server_config.require_client_cert = true;
    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, server_config, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    // No client_cert_path/client_key_path configured -- valid client-side
    // config (a client cert is optional client-side; see
    // http_ssl_mutual_tls_integration_test.cpp's identical precedent for
    // cpp-httplib), but the server must reject the handshake itself.
    kythira::proxygen_client_config client_config;
    client_config.ca_cert_path = mtls.ca_cert_path.string();
    client_config.enable_ssl_verification = true;
    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, client_config,
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    BOOST_CHECK_THROW(std::move(client.send_request_vote(test_node_id, req,
                                                         kythira::testing::scaled_deadline(3000)))
                          .get(),
                      std::exception);

    server.stop();
}

// Requirement 10.1-10.2: the whole-RPC timeout (connect + send + receive)
// actually bounds a request against a peer that accepts the connection but
// never responds -- HTTPTransaction::setIdleTimeout (already wired,
// proxygen_http_transport_impl.hpp) must actually fire, not merely be
// configured. blackhole_listener (above) is deliberately raw sockets, not
// another kythira transport, so this doesn't depend on any transport's own
// timeout behavior being correct as a precondition for testing Proxygen's.
BOOST_AUTO_TEST_CASE(rpc_times_out_against_unresponsive_peer,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 8);
    blackhole_listener listener(port);

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    constexpr auto per_call_timeout = std::chrono::milliseconds(500);
    auto start = std::chrono::steady_clock::now();
    BOOST_CHECK_THROW(
        std::move(client.send_request_vote(test_node_id, req, per_call_timeout)).get(),
        std::exception);
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Generous upper bound (10x the configured timeout, plus a fixed 3s
    // buffer) to absorb CI/test scheduling jitter while still failing
    // outright if the RPC actually hung until some unrelated, much longer
    // default rather than the per-call timeout that was actually
    // configured. The fixed buffer (rather than a larger multiplier alone)
    // absorbs backend-independent fixed overhead -- a real CI run under
    // KYTHIRA_DEFAULT_FUTURE_BACKEND=boost measured 5.77s against a plain
    // 10x/5.0s bound, comfortably within 8.0s but not within 5.0s.
    BOOST_TEST(elapsed < per_call_timeout * 10 + std::chrono::seconds(3));
}

// Property 12 (design.md): forcing the generic bridge (Requirement 14) via
// send_rpc_via_generic_bridge_for_test, the test-only escape hatch
// (proxygen_http_transport.hpp), and confirming it produces the same
// externally-observable result as whichever path send_request_vote's own
// ordinary dispatch actually takes -- which is what makes Requirement 17's
// benchmark comparison of the two paths a fair one (same behavior,
// different cost). `expects_forced_call_differs_from_ordinary_call` is
// computed the same way as `folly_fast_path_is_taken`'s own
// `expects_folly_fast_path` (above): under this project's default
// KYTHIRA_DEFAULT_FUTURE_BACKEND=folly, send_request_vote's ordinary
// dispatch takes the fast path while the forced call takes the generic
// bridge -- two genuinely different paths, so their metrics labels differ.
// Built instead under =stdexec/=boost, send_request_vote's ordinary
// dispatch already *is* the generic bridge (Requirement 16.1's condition
// never holds), so the escape hatch's "forced" call is indistinguishable
// from the ordinary one -- both calls still succeed and both still produce
// the correct response either way, which is what this test actually
// checks; only the metrics-label assertions below are conditioned on which
// case applies.
BOOST_AUTO_TEST_CASE(generic_bridge_forced_matches_fast_path_result,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    constexpr bool expects_forced_call_differs_from_ordinary_call =
        std::same_as<test_transport_types::future_template<kythira::request_vote_response<>>,
                     kythira::Future<kythira::request_vote_response<>>>;

    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 9);

    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    recording_metrics client_metrics;
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          client_metrics);

    kythira::request_vote_request<> fast_req{};
    fast_req._term = 11;
    auto fast_resp = std::move(client.send_request_vote(test_node_id, fast_req,
                                                        kythira::testing::scaled_deadline(3000)))
                         .get();

    kythira::request_vote_request<> bridged_req{};
    bridged_req._term = 12;
    auto bridged_resp =
        std::move(client.send_rpc_via_generic_bridge_for_test<kythira::request_vote_request<>,
                                                              kythira::request_vote_response<>>(
                      test_node_id, kythira::proxygen_detail::proxygen_endpoint_request_vote,
                      bridged_req, kythira::testing::scaled_deadline(3000)))
            .get();

    BOOST_TEST(fast_resp.term() == 11);
    BOOST_TEST(fast_resp.vote_granted());
    BOOST_TEST(bridged_resp.term() == 12);
    BOOST_TEST(bridged_resp.vote_granted());

    auto sent = client_metrics.entries_named("proxygen_http.client.request.sent");
    BOOST_REQUIRE(sent.size() == 2);
    BOOST_TEST(sent[1].dimensions.at("path") == "generic_bridge");  // the forced call, always.
    BOOST_TEST(sent[0].dimensions.at("path") ==
               std::string(expects_forced_call_differs_from_ordinary_call ? "folly_fast_path"
                                                                          : "generic_bridge"));

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
