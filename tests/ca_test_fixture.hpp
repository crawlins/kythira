#pragma once

/// @file ca_test_fixture.hpp
/// @brief Test fixture collapsing "set up a CA" + "bootstrap a client" into two
///        calls, so tests wiring up TLS/mTLS/DTLS don't re-derive the
///        construct -> issue -> materialize-to-files sequence by hand every time.
///
/// Mirrors the setup-on-construction pattern already used by fixtures such as
/// `raft::test::LocalNetworkFixture` / `raft_multi_node_test_fixture.hpp`.
///
/// Network-service mode (`ca_test_fixture_options::start_network_service`)
/// launches the `ca_service --serve` executable as a child process. Callers that
/// use this mode must define `CA_SERVICE_PATH` (the absolute path to the built
/// `ca_service` binary, e.g. via `target_compile_definitions(... PRIVATE
/// CA_SERVICE_PATH="${CMAKE_BINARY_DIR}/ca_service")`); it falls back to bare
/// `"ca_service"` (resolved via `$PATH`) when undefined.

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CA_SERVICE_PATH
#define CA_SERVICE_PATH "ca_service"
#endif

extern char** environ;

namespace raft::testing {

struct ca_test_fixture_options {
    ca_options ca{};
    bool start_network_service{false};
    std::string serve_bind_address{"127.0.0.1:0"};  // :0 = OS-assigned ephemeral port
    std::chrono::seconds startup_timeout{10};
    // TLS for the spawned ca_service --serve child (network-service mode
    // only) — empty means plain HTTP, matching ca_service's own default.
    // Needed by ca_bootstrap_client_test.cpp: pinned-fingerprint bootstrap
    // is meaningless without a real TLS listener to fingerprint.
    std::string tls_cert_path;
    std::string tls_key_path;
};

namespace detail {

// Binds a throwaway socket to port 0, reads back the OS-assigned port, and
// closes it. Small TOCTOU race between this and the child process's own bind()
// is inherent to this "find a free port" pattern and accepted here, matching
// common test-fixture practice.
inline auto find_free_port(const std::string& host) -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("ca_test_fixture: socket() failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("ca_test_fixture: bind() failed while probing for a free port");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("ca_test_fixture: getsockname() failed");
    }
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

inline auto generate_token() -> std::string {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream out;
    out << std::hex << dist(gen) << dist(gen);
    return out.str();
}

}  // namespace detail

/// Sets up a certificate authority — in-process by default, or a real,
/// network-reachable `ca_service --serve` child process when
/// `start_network_service` is set — and hands out ready-to-use leaf
/// certificates via `bootstrap_client()`. The fixture owns every
/// `temp_cert_files` it returns for its own lifetime.
class ca_test_fixture {
public:
    explicit ca_test_fixture(ca_test_fixture_options options = {}) : _options(std::move(options)) {
        if (_options.start_network_service) {
            start_service();
        } else {
            _ca = std::make_unique<certificate_authority>(_options.ca);
        }
    }

    ~ca_test_fixture() {
        if (_service_pid) {
            ::kill(*_service_pid, SIGTERM);
            int status = 0;
            ::waitpid(*_service_pid, &status, 0);
        }
    }

    ca_test_fixture(const ca_test_fixture&) = delete;
    ca_test_fixture& operator=(const ca_test_fixture&) = delete;
    ca_test_fixture(ca_test_fixture&&) = delete;
    ca_test_fixture& operator=(ca_test_fixture&&) = delete;

    [[nodiscard]] auto root_certificate_pem() const -> const std::string& {
        return _ca ? _ca->root_certificate_pem() : *_service_root_pem;
    }

    // Network-service mode only — the bearer token and base URL of the
    // spawned ca_service --serve child, for callers (e.g.
    // ca_bootstrap_client_test.cpp) that need to talk to it directly rather
    // than through bootstrap_client()/renew().
    [[nodiscard]] auto service_auth_token() const -> const std::string& { return _auth_token; }

    [[nodiscard]] auto service_base_url() const -> std::string {
        return (_service_tls_enabled ? "https://" : "http://") + _service_host + ":" +
               std::to_string(_service_port);
    }

    [[nodiscard]] auto bootstrap_client(std::string client_id, std::vector<std::string> dns_names,
                                        std::vector<std::string> ip_addresses = {},
                                        bool server_auth = true, bool client_auth = true,
                                        std::chrono::seconds validity = std::chrono::hours(24 * 30))
        -> const temp_cert_files& {
        leaf_certificate_options opts;
        opts.subject.common_name = client_id;
        opts.dns_names = dns_names;
        opts.ip_addresses = ip_addresses;
        opts.server_auth = server_auth;
        opts.client_auth = client_auth;
        opts.validity = validity;

        pem_material material = _ca ? _ca->issue(opts) : bootstrap_via_service(opts);

        auto files = std::make_unique<temp_cert_files>(material);
        auto& ref = *files;
        _issued.push_back(std::move(files));
        _bootstrapped[client_id] = {opts, _issued.size() - 1};
        return ref;
    }

    /// Re-issues a certificate for a `client_id` previously passed to
    /// `bootstrap_client()`, using the exact same options captured at that
    /// call, and atomically replaces the material at the same
    /// `temp_cert_files` paths previously returned for that `client_id`.
    /// Throws `std::invalid_argument` for an unknown `client_id`.
    [[nodiscard]] auto renew(const std::string& client_id) -> const temp_cert_files& {
        auto it = _bootstrapped.find(client_id);
        if (it == _bootstrapped.end()) {
            throw std::invalid_argument("ca_test_fixture::renew: unknown client_id: " + client_id);
        }
        const auto& opts = it->second.options;
        pem_material material = _ca ? _ca->issue(opts) : bootstrap_via_service(opts);

        auto& files = *_issued[it->second.index];
        files.replace_atomically(material);
        return files;
    }

protected:
    struct bootstrapped_entry {
        leaf_certificate_options options;
        std::size_t index;
    };

    ca_test_fixture_options _options;
    std::unique_ptr<certificate_authority> _ca;
    std::vector<std::unique_ptr<temp_cert_files>> _issued;
    std::unordered_map<std::string, bootstrapped_entry> _bootstrapped;

    // Network-service mode state.
    std::optional<std::string> _service_root_pem;
    std::optional<pid_t> _service_pid;
    std::string _service_host;
    int _service_port{0};
    std::string _auth_token;
    bool _service_tls_enabled{false};

    auto bootstrap_via_service(const leaf_certificate_options& opts) -> pem_material {
        auto csr = generate_key_and_csr(opts);

        boost::json::object body;
        body["csr_pem"] = csr.csr_pem;
        boost::json::array dns_arr;
        for (const auto& d : opts.dns_names) dns_arr.push_back(boost::json::string(d));
        body["dns_names"] = dns_arr;
        boost::json::array ip_arr;
        for (const auto& ip : opts.ip_addresses) ip_arr.push_back(boost::json::string(ip));
        body["ip_addresses"] = ip_arr;
        body["server_auth"] = opts.server_auth;
        body["client_auth"] = opts.client_auth;
        body["validity_days"] = static_cast<int>(
            std::chrono::duration_cast<std::chrono::hours>(opts.validity).count() / 24);

        std::string scheme_host_port = (_service_tls_enabled ? "https://" : "http://") +
                                       _service_host + ":" + std::to_string(_service_port);
        httplib::Client client(scheme_host_port);
        client.enable_server_certificate_verification(false);  // self-signed test cert
        httplib::Headers headers = {{"Authorization", "Bearer " + _auth_token}};
        auto res = client.Post("/v1/certificates", headers, boost::json::serialize(body),
                               "application/json");
        if (!res || res->status != 200) {
            throw std::runtime_error("ca_test_fixture: POST /v1/certificates failed: " +
                                     (res ? std::to_string(res->status) + " " + res->body
                                          : httplib::to_string(res.error())));
        }

        auto response_obj = boost::json::parse(res->body).as_object();
        pem_material material;
        material.certificate_pem = std::string(response_obj["certificate_pem"].as_string());
        material.chain_pem = std::string(response_obj["chain_pem"].as_string());
        material.private_key_pem = csr.private_key_pem;
        return material;
    }

    auto start_service() -> void {
        auto colon = _options.serve_bind_address.rfind(':');
        std::string host = colon == std::string::npos
                               ? _options.serve_bind_address
                               : _options.serve_bind_address.substr(0, colon);
        _service_host = host;
        _service_port = detail::find_free_port(host);
        _auth_token = detail::generate_token();
        _service_tls_enabled = !_options.tls_cert_path.empty();

        std::string bind_arg = host + ":" + std::to_string(_service_port);
        std::vector<std::string> argv_strs = {CA_SERVICE_PATH, "--serve", bind_arg,
                                              "--provider",    "local",   "--auth-token",
                                              _auth_token};
        if (_service_tls_enabled) {
            argv_strs.push_back("--tls-cert");
            argv_strs.push_back(_options.tls_cert_path);
            argv_strs.push_back("--tls-key");
            argv_strs.push_back(_options.tls_key_path);
        }
        std::vector<char*> argv;
        argv.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) argv.push_back(s.data());
        argv.push_back(nullptr);

        pid_t pid = 0;
        int rc = posix_spawn(&pid, CA_SERVICE_PATH, nullptr, nullptr, argv.data(), environ);
        if (rc != 0) {
            throw std::runtime_error("ca_test_fixture: posix_spawn(ca_service) failed: " +
                                     std::string(std::strerror(rc)));
        }
        _service_pid = pid;

        std::string scheme_host_port = (_service_tls_enabled ? "https://" : "http://") +
                                       _service_host + ":" + std::to_string(_service_port);
        httplib::Client health_client(scheme_host_port);
        health_client.enable_server_certificate_verification(false);  // self-signed test cert
        health_client.set_connection_timeout(1, 0);
        httplib::Headers auth_headers = {{"Authorization", "Bearer " + _auth_token}};
        auto deadline = std::chrono::steady_clock::now() + _options.startup_timeout;
        bool healthy = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto res = health_client.Get("/healthz", auth_headers);
            if (res && res->status == 200) {
                healthy = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!healthy) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            _service_pid.reset();
            throw std::runtime_error(
                "ca_test_fixture: ca_service --serve did not become healthy within "
                "startup_timeout");
        }

        httplib::Headers headers = {{"Authorization", "Bearer " + _auth_token}};
        auto root_res = health_client.Get("/v1/root-ca", headers);
        if (!root_res || root_res->status != 200) {
            throw std::runtime_error("ca_test_fixture: GET /v1/root-ca failed");
        }
        _service_root_pem = root_res->body;
    }
};

}  // namespace raft::testing
