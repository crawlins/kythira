// ca_service — provisions a shared root CA and per-service leaf certificates for
// Docker/Podman multi-container test scenarios (oneshot mode), or serves them over
// an authenticated HTTP API for cross-host use (--serve mode).
//
// Oneshot usage:
//   ca_service --out-dir <path> --service <name>[:alt1,alt2,...] [--service ...]
//              [--domain <suffix>] [--validity-days <n>] [--resolve-ips]
//
// Output layout under --out-dir:
//   root_ca.pem
//   <service-name>/cert.pem
//   <service-name>/key.pem   (mode 0600)
//   <service-name>/chain.pem
//
// Serve usage:
//   ca_service --serve <bind-address>:<port> [--provider local|aws-acm-pca]
//              [--acm-pca-arn <arn>] [--aws-region <region>] [--aws-endpoint-override <url>]
//              [--auth-token <token>] [--tls-cert <path> --tls-key <path>]
//
// --serve is mutually exclusive with --out-dir/--service. Every request must
// carry `Authorization: Bearer <token>` (from --auth-token or
// $CA_SERVICE_AUTH_TOKEN); --serve refuses to start with no token configured.
//
//   GET  /healthz                     -> 200 once the provider is ready
//   GET  /v1/root-ca                  -> 200, body = root/CA certificate PEM
//   POST /v1/certificates             -> JSON {csr_pem, dns_names, ip_addresses,
//                                              server_auth, client_auth, validity_days}
//                                        -> 200 JSON {certificate_pem, chain_pem}
//   POST /v1/certificates/revoke      -> JSON {serial} (local provider only; 501 otherwise)
//   GET  /v1/crl                      -> local provider only; 501 otherwise

#include <raft/ca_http_helpers.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#ifdef KYTHIRA_HAS_AWS_ACM_PCA
#include <raft/aws_acm_pca_provider.hpp>
#include <raft/aws_acm_pca_provider_impl.hpp>
#include <aws/core/Aws.h>
#endif

#include <httplib.h>
#include <boost/json.hpp>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Oneshot mode (unchanged from the original ca_service; see file header).
// ---------------------------------------------------------------------------

struct service_spec {
    std::string name;
    std::vector<std::string> alt_names;
};

struct cli_options {
    std::vector<service_spec> services;
    std::string out_dir;
    std::string domain;
    int validity_days{30};
    bool resolve_ips{false};
};

struct serve_options {
    std::string bind_address_and_port;
    std::string provider{"local"};
    std::string acm_pca_arn;
    std::string aws_region;
    std::string aws_endpoint_override;
    std::string auth_token;
    std::string tls_cert_path;
    std::string tls_key_path;
    bool print_root_fingerprint{false};
};

[[noreturn]] void usage_error(const std::string& message) {
    std::cerr
        << "ca_service: " << message << "\n\n"
        << "Usage: ca_service --out-dir <path> --service <name>[:alt1,alt2,...] "
           "[--service ...]\n"
        << "                  [--domain <suffix>] [--validity-days <n>] [--resolve-ips]\n"
        << "   or: ca_service --serve <bind-address>:<port> [--provider local|aws-acm-pca]\n"
        << "                  [--acm-pca-arn <arn>] [--aws-region <region>]\n"
        << "                  [--aws-endpoint-override <url>] [--auth-token <token>]\n"
        << "                  [--tls-cert <path> --tls-key <path>] [--print-root-fingerprint]\n";
    std::exit(1);
}

// Splits "name:alt1,alt2" into a service_spec.
service_spec parse_service_arg(const std::string& arg) {
    service_spec spec;
    auto colon = arg.find(':');
    if (colon == std::string::npos) {
        spec.name = arg;
        return spec;
    }
    spec.name = arg.substr(0, colon);
    std::string alts = arg.substr(colon + 1);
    std::stringstream ss(alts);
    std::string alt;
    while (std::getline(ss, alt, ',')) {
        if (!alt.empty()) spec.alt_names.push_back(alt);
    }
    return spec;
}

const char* env_or(const char* key, const char* fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? v : fallback;
}

cli_options parse_oneshot_args(int argc, char** argv, int start) {
    cli_options opts;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--service") {
            opts.services.push_back(parse_service_arg(next()));
        } else if (arg == "--out-dir") {
            opts.out_dir = next();
        } else if (arg == "--domain") {
            opts.domain = next();
        } else if (arg == "--validity-days") {
            try {
                opts.validity_days = std::stoi(next());
            } catch (const std::exception&) {
                usage_error("--validity-days requires an integer argument");
            }
        } else if (arg == "--resolve-ips") {
            opts.resolve_ips = true;
        } else if (arg == "-h" || arg == "--help") {
            usage_error("");
        } else {
            usage_error("unrecognized argument: " + arg);
        }
    }
    if (opts.out_dir.empty()) usage_error("--out-dir is required");
    if (opts.services.empty()) usage_error("at least one --service is required");
    return opts;
}

// Resolves `dns_name` via getaddrinfo, returning dotted-decimal/IPv6 literal
// addresses. Returns an empty vector (never throws) when resolution fails — at
// provisioning time a peer's own container may not have started yet, so only
// self-resolution is expected to succeed reliably.
std::vector<std::string> resolve_ips_for(const std::string& dns_name) {
    std::vector<std::string> out;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(dns_name.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        return out;
    }
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {};
        void* addr = nullptr;
        if (p->ai_family == AF_INET) {
            addr = &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr;
        } else if (p->ai_family == AF_INET6) {
            addr = &reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        if (inet_ntop(p->ai_family, addr, buf, sizeof(buf)) != nullptr) {
            out.emplace_back(buf);
        }
    }
    freeaddrinfo(res);
    return out;
}

void write_file(const std::filesystem::path& path, const std::string& content,
                bool restrict_perms) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open " + path.string() + " for writing");
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    if (restrict_perms) {
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }
}

int run_oneshot(const cli_options& opts) {
    std::filesystem::create_directories(opts.out_dir);

    raft::testing::certificate_authority ca;
    write_file(std::filesystem::path(opts.out_dir) / "root_ca.pem", ca.root_certificate_pem(),
               /*restrict_perms=*/false);

    for (const auto& spec : opts.services) {
        std::vector<std::string> dns_names;
        std::string fqdn = spec.name;
        if (!opts.domain.empty()) fqdn += "." + opts.domain;
        dns_names.push_back(fqdn);
        for (const auto& alt : spec.alt_names) dns_names.push_back(alt);

        std::vector<std::string> ip_addresses;
        if (opts.resolve_ips) {
            for (const auto& name : dns_names) {
                auto resolved = resolve_ips_for(name);
                ip_addresses.insert(ip_addresses.end(), resolved.begin(), resolved.end());
            }
        }

        raft::testing::leaf_certificate_options leaf_opts;
        leaf_opts.subject.common_name = spec.name;
        leaf_opts.dns_names = dns_names;
        leaf_opts.ip_addresses = ip_addresses;
        leaf_opts.server_auth = true;
        leaf_opts.client_auth = true;
        leaf_opts.validity = std::chrono::hours(24 * opts.validity_days);

        auto material = ca.issue(leaf_opts);

        auto service_dir = std::filesystem::path(opts.out_dir) / spec.name;
        std::filesystem::create_directories(service_dir);
        write_file(service_dir / "cert.pem", material.certificate_pem, /*restrict_perms=*/false);
        write_file(service_dir / "key.pem", material.private_key_pem, /*restrict_perms=*/true);
        write_file(service_dir / "chain.pem", material.chain_pem, /*restrict_perms=*/false);

        std::cout << "ca_service: issued certificate for '" << spec.name << "' ("
                  << dns_names.size() << " DNS SAN, " << ip_addresses.size() << " IP SAN)\n";
    }

    std::cout << "ca_service: wrote root CA and " << opts.services.size()
              << " service certificate(s) to " << opts.out_dir << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Serve mode
// ---------------------------------------------------------------------------

// Type-erases local_certificate_provider / aws_acm_pca_provider behind one
// interface so the HTTP route handlers below don't need to know which backend
// is in use — the same seam certificate_provider draws for any other caller.
class any_certificate_provider {
public:
    template<raft::testing::certificate_provider P>
    explicit any_certificate_provider(P& p) : _self(std::make_unique<model<P>>(p)) {}

    auto root_certificate_pem() -> kythira::Future<std::string> {
        return _self->root_certificate_pem();
    }
    auto sign_csr(std::string csr_pem, raft::testing::csr_signing_options options)
        -> kythira::Future<raft::testing::pem_material> {
        return _self->sign_csr(std::move(csr_pem), std::move(options));
    }

private:
    struct concept_t {
        virtual ~concept_t() = default;
        virtual auto root_certificate_pem() -> kythira::Future<std::string> = 0;
        virtual auto sign_csr(std::string, raft::testing::csr_signing_options)
            -> kythira::Future<raft::testing::pem_material> = 0;
    };
    template<typename P> struct model : concept_t {
        P& provider;
        explicit model(P& p) : provider(p) {}
        auto root_certificate_pem() -> kythira::Future<std::string> override {
            return provider.root_certificate_pem();
        }
        auto sign_csr(std::string csr_pem, raft::testing::csr_signing_options options)
            -> kythira::Future<raft::testing::pem_material> override {
            return provider.sign_csr(std::move(csr_pem), std::move(options));
        }
    };
    std::unique_ptr<concept_t> _self;
};

serve_options parse_serve_args(int argc, char** argv, int start) {
    serve_options opts;
    bool saw_out_dir = false;
    bool saw_service = false;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--provider") {
            opts.provider = next();
        } else if (arg == "--acm-pca-arn") {
            opts.acm_pca_arn = next();
        } else if (arg == "--aws-region") {
            opts.aws_region = next();
        } else if (arg == "--aws-endpoint-override") {
            opts.aws_endpoint_override = next();
        } else if (arg == "--auth-token") {
            opts.auth_token = next();
        } else if (arg == "--tls-cert") {
            opts.tls_cert_path = next();
        } else if (arg == "--tls-key") {
            opts.tls_key_path = next();
        } else if (arg == "--print-root-fingerprint") {
            opts.print_root_fingerprint = true;
        } else if (arg == "--out-dir" || arg == "--service") {
            usage_error(arg + " cannot be combined with --serve");
        } else if (arg == "-h" || arg == "--help") {
            usage_error("");
        } else {
            usage_error("unrecognized argument: " + arg);
        }
    }
    (void)saw_out_dir;
    (void)saw_service;

    if (opts.provider != "local" && opts.provider != "aws-acm-pca") {
        usage_error("--provider must be 'local' or 'aws-acm-pca'");
    }
    if (!opts.tls_cert_path.empty() != !opts.tls_key_path.empty()) {
        usage_error("--tls-cert and --tls-key must be given together");
    }
    if (opts.print_root_fingerprint) {
        // Requirement 19.2: prints the fingerprint and exits without binding
        // any port — needs neither an auth token nor a provider selection,
        // only the TLS listener material whose root it fingerprints.
        if (opts.tls_cert_path.empty()) {
            usage_error(
                "--print-root-fingerprint requires --tls-cert/--tls-key (nothing to fingerprint "
                "without TLS configured)");
        }
        return opts;
    }
    if (opts.auth_token.empty()) {
        opts.auth_token = env_or("CA_SERVICE_AUTH_TOKEN", "");
    }
    if (opts.auth_token.empty()) {
        usage_error("--serve requires --auth-token or $CA_SERVICE_AUTH_TOKEN (fail closed)");
    }
    return opts;
}

// Parses "host:port", where host may itself contain no colons (IPv6 literals
// are out of scope for this CLI convenience parser).
std::pair<std::string, int> split_bind_address(const std::string& bind) {
    auto colon = bind.rfind(':');
    if (colon == std::string::npos) usage_error("--serve requires <bind-address>:<port>");
    std::string host = bind.substr(0, colon);
    int port = 0;
    try {
        port = std::stoi(bind.substr(colon + 1));
    } catch (const std::exception&) {
        usage_error("--serve: invalid port in " + bind);
    }
    return {host, port};
}

std::atomic<httplib::Server*> g_server{nullptr};

void on_signal(int) {
    auto* srv = g_server.load();
    if (srv != nullptr) srv->stop();
}

// Reads opts.tls_cert_path, finds its root certificate (self-signed member,
// or the last PEM block if none is self-signed), and prints its SHA-256
// fingerprint to stdout — shared by --print-root-fingerprint and the
// startup log line printed once TLS is actually up and running.
void print_root_fingerprint_line(const std::string& tls_cert_path, std::ostream& out) {
    std::ifstream f(tls_cert_path, std::ios::binary);
    if (!f) {
        std::cerr << "ca_service: cannot open --tls-cert " << tls_cert_path << "\n";
        std::exit(1);
    }
    std::string bundle((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try {
        auto root = raft::testing::root_cert_from_pem_bundle(bundle);
        out << "ca_service: root certificate SHA-256 fingerprint: "
            << raft::testing::sha256_fingerprint_hex(root.get()) << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "ca_service: failed to compute root fingerprint: " << ex.what() << "\n";
        std::exit(1);
    }
}

int run_serve(const serve_options& opts) {
    if (opts.print_root_fingerprint) {
        // Requirement 19.2: prints and exits without binding any port.
        print_root_fingerprint_line(opts.tls_cert_path, std::cout);
        return 0;
    }

    // Certificate material owned for the lifetime of the process; the type-erased
    // provider below borrows from whichever of these is actually in use.
    std::unique_ptr<raft::testing::certificate_authority> local_ca;
    std::unique_ptr<raft::testing::local_certificate_provider> local_provider;
#ifdef KYTHIRA_HAS_AWS_ACM_PCA
    std::unique_ptr<raft::testing::aws_acm_pca_provider> acm_provider;
    Aws::SDKOptions aws_sdk_options;
    if (opts.provider == "aws-acm-pca") {
        Aws::InitAPI(aws_sdk_options);
    }
#endif
    std::unique_ptr<any_certificate_provider> provider;

    if (opts.provider == "local") {
        local_ca = std::make_unique<raft::testing::certificate_authority>();
        local_provider = std::make_unique<raft::testing::local_certificate_provider>(*local_ca);
        provider = std::make_unique<any_certificate_provider>(*local_provider);
    } else {
#ifdef KYTHIRA_HAS_AWS_ACM_PCA
        raft::testing::aws_acm_pca_provider_config cfg;
        cfg.certificate_authority_arn = opts.acm_pca_arn;
        cfg.aws.region = opts.aws_region;
        cfg.aws.endpoint_override = opts.aws_endpoint_override;
        if (cfg.certificate_authority_arn.empty()) {
            std::cerr << "ca_service: --provider aws-acm-pca requires --acm-pca-arn\n";
            return 1;
        }
        acm_provider = std::make_unique<raft::testing::aws_acm_pca_provider>(std::move(cfg));
        provider = std::make_unique<any_certificate_provider>(*acm_provider);
#else
        std::cerr << "ca_service: built without KYTHIRA_HAS_AWS_ACM_PCA — --provider aws-acm-pca "
                     "is unavailable\n";
        return 1;
#endif
    }

    auto [host, port] = split_bind_address(opts.bind_address_and_port);

    std::unique_ptr<httplib::Server> plain_server;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    std::unique_ptr<httplib::SSLServer> ssl_server;
#endif
    httplib::Server* server = nullptr;

    if (!opts.tls_cert_path.empty()) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        ssl_server = std::make_unique<httplib::SSLServer>(opts.tls_cert_path.c_str(),
                                                          opts.tls_key_path.c_str());
        if (!ssl_server->is_valid()) {
            std::cerr << "ca_service: failed to initialize TLS with cert " << opts.tls_cert_path
                      << " / key " << opts.tls_key_path << "\n";
            return 1;
        }
        // Request (not require) a client certificate: bearer-token-only routes
        // keep working for callers presenting none; POST /v1/certificates/renew
        // is the only route that actually needs one (Requirement 15.3).
        SSL_CTX_set_verify(ssl_server->ssl_context(), SSL_VERIFY_PEER,
                           raft::testing::accept_any_peer_certificate);
        server = ssl_server.get();
        // Requirement 19.1: printed once at startup so an operator distributing
        // a bearer token out-of-band can pin this fingerprint for
        // ca_bootstrap_client's first-contact trust check without parsing logs
        // beyond this one line.
        print_root_fingerprint_line(opts.tls_cert_path, std::cerr);
#else
        std::cerr << "ca_service: built without TLS support — cannot honor --tls-cert/--tls-key\n";
        return 1;
#endif
    } else {
        plain_server = std::make_unique<httplib::Server>();
        server = plain_server.get();
        std::cerr
            << "ca_service: WARNING: --serve running without TLS (no --tls-cert/--tls-key given) — "
               "suitable only for a private network (e.g. inside one docker-compose network)\n";
    }

    g_server.store(server);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string bearer_prefix = "Bearer ";
    server->set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        // POST /v1/certificates/renew authenticates via the caller's own mTLS
        // client certificate instead of the bearer token (Requirement 15.3) —
        // its own handler performs that check.
        if (req.method == "POST" && req.path == "/v1/certificates/renew") {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        auto auth = req.get_header_value("Authorization");
        if (auth != bearer_prefix + opts.auth_token) {
            res.status = 401;
            res.set_content(R"({"error":"unauthorized"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server->Get("/healthz",
                [](const httplib::Request&, httplib::Response& res) { res.status = 200; });

    server->Get("/v1/root-ca", [&](const httplib::Request&, httplib::Response& res) {
        try {
            auto pem = provider->root_certificate_pem().get();
            res.set_content(pem, "application/x-pem-file");
        } catch (const std::exception& ex) {
            res.status = 502;
            res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                            "application/json");
        }
    });

    server->Post("/v1/certificates", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = boost::json::parse(req.body).as_object();
            auto* csr_val = body.if_contains("csr_pem");
            if (csr_val == nullptr || !csr_val->is_string()) {
                res.status = 400;
                res.set_content(R"({"error":"csr_pem is required"})", "application/json");
                return;
            }
            std::string csr_pem = std::string(csr_val->as_string());
            auto options = raft::testing::parse_csr_signing_options(body);

            auto material = provider->sign_csr(csr_pem, options).get();
            res.set_content(boost::json::serialize(raft::testing::pem_material_to_json(material)),
                            "application/json");
        } catch (const std::invalid_argument& ex) {
            res.status = 400;
            res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                            "application/json");
        } catch (const std::exception& ex) {
            res.status = 502;
            res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                            "application/json");
        }
    });

    server->Post("/v1/certificates/renew", [&](const httplib::Request& req,
                                               httplib::Response& res) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (req.ssl == nullptr) {
            res.status = 401;
            res.set_content(R"({"error":"renew requires an mTLS connection"})", "application/json");
            return;
        }
        X509* peer_cert = SSL_get1_peer_certificate(req.ssl);
        if (peer_cert == nullptr) {
            res.status = 401;
            res.set_content(R"({"error":"no client certificate presented"})", "application/json");
            return;
        }

        try {
            auto root_pem = provider->root_certificate_pem().get();
            if (!raft::testing::cert_chains_to_root(peer_cert, root_pem)) {
                X509_free(peer_cert);
                res.status = 401;
                res.set_content(
                    R"({"error":"presented certificate does not chain to this CA's root"})",
                    "application/json");
                return;
            }

            auto options = raft::testing::options_from_presented_cert(peer_cert);
            X509_free(peer_cert);

            auto body = boost::json::parse(req.body).as_object();
            auto* csr_val = body.if_contains("csr_pem");
            if (csr_val == nullptr || !csr_val->is_string()) {
                res.status = 400;
                res.set_content(R"({"error":"csr_pem is required"})", "application/json");
                return;
            }
            std::string csr_pem = std::string(csr_val->as_string());
            if (auto* v = body.if_contains("validity_days"); v && v->is_number()) {
                options.validity = std::chrono::hours(24 * v->to_number<int>());
            }

            auto material = provider->sign_csr(csr_pem, options).get();
            res.set_content(boost::json::serialize(raft::testing::pem_material_to_json(material)),
                            "application/json");
        } catch (const std::invalid_argument& ex) {
            res.status = 400;
            res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                            "application/json");
        } catch (const std::exception& ex) {
            res.status = 502;
            res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                            "application/json");
        }
#else
        res.status = 401;
        res.set_content(R"({"error":"built without TLS support — renew is unavailable"})", "application/json");
#endif
    });

    server->Post(
        "/v1/certificates/revoke", [&](const httplib::Request& req, httplib::Response& res) {
            if (opts.provider != "local") {
                res.status = 501;
                res.set_content(R"({"error":"not_implemented"})", "application/json");
                return;
            }
            try {
                auto body = boost::json::parse(req.body).as_object();
                auto* serial_val = body.if_contains("serial");
                if (serial_val == nullptr) {
                    res.status = 400;
                    res.set_content(R"({"error":"serial is required"})", "application/json");
                    return;
                }
                std::uint64_t serial = 0;
                if (serial_val->is_string()) {
                    serial = std::stoull(std::string(serial_val->as_string()));
                } else {
                    serial = serial_val->to_number<std::uint64_t>();
                }
                raft::testing::pem_material fake;
                fake.serial = serial;
                local_ca->revoke(fake);
                res.status = 200;
                res.set_content(R"({"revoked":true})", "application/json");
            } catch (const std::invalid_argument& ex) {
                res.status = 400;
                res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                                "application/json");
            } catch (const std::exception& ex) {
                res.status = 502;
                res.set_content(boost::json::serialize(boost::json::object{{"error", ex.what()}}),
                                "application/json");
            }
        });

    server->Get("/v1/crl", [&](const httplib::Request&, httplib::Response& res) {
        if (opts.provider != "local") {
            res.status = 501;
            res.set_content(R"({"error":"not_implemented"})", "application/json");
            return;
        }
        res.set_content(local_ca->crl_pem(), "application/x-pem-file");
    });

    std::cout << "ca_service: serving on " << host << ":" << port << " (provider=" << opts.provider
              << ")\n";
    server->listen(host, port);

    g_server.store(nullptr);
#ifdef KYTHIRA_HAS_AWS_ACM_PCA
    if (opts.provider == "aws-acm-pca") {
        Aws::ShutdownAPI(aws_sdk_options);
    }
#endif
    std::cout << "ca_service: shut down cleanly\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--serve") {
                // <bind-address>:<port> is the one positional argument to
                // --serve; it's optional only for --print-root-fingerprint
                // (Requirement 19.2 — that mode never binds a port at all),
                // detected here by the next token starting with "--" instead
                // of looking like an address.
                bool has_bind_address =
                    (i + 1 < argc) && !std::string(argv[i + 1]).starts_with("--");
                serve_options opts = parse_serve_args(argc, argv, i + (has_bind_address ? 2 : 1));
                if (has_bind_address) {
                    opts.bind_address_and_port = argv[i + 1];
                } else if (!opts.print_root_fingerprint) {
                    usage_error("--serve requires <bind-address>:<port>");
                }
                return run_serve(opts);
            }
        }
        auto opts = parse_oneshot_args(argc, argv, 1);
        return run_oneshot(opts);
    } catch (const std::exception& ex) {
        std::cerr << "ca_service: error: " << ex.what() << "\n";
        return 1;
    }
}
