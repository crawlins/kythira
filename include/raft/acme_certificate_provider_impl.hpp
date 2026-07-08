#pragma once

/// @file acme_certificate_provider_impl.hpp
/// @brief Method definitions for `acme_certificate_provider`. Header-only
///        (mirrors `aws_acm_pca_provider_impl.hpp`'s split — declarations in
///        the public header, definitions here — both ultimately inline;
///        included directly by consumers, not compiled into a separate .cpp).

#include <raft/acme_certificate_provider.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#ifdef KYTHIRA_HAS_LDNS
#include <ldns/ldns.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace raft::testing {

namespace acme_detail {

struct split_url_result {
    std::string origin;  // "scheme://host:port"
    std::string path;
};

[[nodiscard]] inline auto split_url(const std::string& url) -> split_url_result {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw std::invalid_argument("acme_certificate_provider: malformed URL: " + url);
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return {url, "/"};
    }
    return {url.substr(0, path_start), url.substr(path_start)};
}

// httplib::Client(scheme_host_port) auto-selects TLS internally when the
// scheme is "https" and the build has CPPHTTPLIB_OPENSSL_SUPPORT.
[[nodiscard]] inline auto make_client(const std::string& origin)
    -> std::unique_ptr<httplib::Client> {
    auto client = std::make_unique<httplib::Client>(origin);
    client->set_connection_timeout(10, 0);
    client->set_read_timeout(30, 0);
    client->enable_server_certificate_verification(
        false);  // test server / self-signed intermediate CAs
    return client;
}

[[nodiscard]] inline auto require_header(const httplib::Result& res, const std::string& name)
    -> std::string {
    if (!res)
        throw std::runtime_error("acme_certificate_provider: request failed: " +
                                 httplib::to_string(res.error()));
    auto v = res->get_header_value(name);
    if (v.empty())
        throw std::runtime_error("acme_certificate_provider: response missing " + name + " header");
    return v;
}

[[nodiscard]] inline auto der_csr_from_pem(const std::string& csr_pem)
    -> std::vector<unsigned char> {
    BIO* bio = BIO_new_mem_buf(csr_pem.data(), static_cast<int>(csr_pem.size()));
    X509_REQ* req = PEM_read_bio_X509_REQ(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (req == nullptr)
        throw std::invalid_argument("acme_certificate_provider: unparseable CSR PEM");
    std::unique_ptr<X509_REQ, void (*)(X509_REQ*)> guard{req, X509_REQ_free};

    unsigned char* der = nullptr;
    int len = i2d_X509_REQ(req, &der);
    if (len < 0) throw std::runtime_error("acme_certificate_provider: i2d_X509_REQ failed");
    std::vector<unsigned char> out(der, der + len);
    OPENSSL_free(der);
    return out;
}

// Splits a PEM bundle (as returned by an ACME certificate-download endpoint:
// leaf followed by zero or more intermediates/root, all concatenated) into
// individual PEM blocks, preserving order.
[[nodiscard]] inline auto split_pem_chain(const std::string& bundle) -> std::vector<std::string> {
    std::vector<std::string> blocks;
    std::size_t pos = 0;
    while (true) {
        auto begin = bundle.find("-----BEGIN CERTIFICATE-----", pos);
        if (begin == std::string::npos) break;
        auto end = bundle.find("-----END CERTIFICATE-----", begin);
        if (end == std::string::npos) break;
        end += std::string("-----END CERTIFICATE-----").size();
        blocks.push_back(bundle.substr(begin, end - begin) + "\n");
        pos = end;
    }
    return blocks;
}

// ── http-01 responder ───────────────────────────────────────────────────────

class http01_responder {
public:
    http01_responder(const std::string& bind_address, std::string token,
                     std::string key_authorization)
        : _token(std::move(token)), _key_authorization(std::move(key_authorization)) {
        auto colon = bind_address.rfind(':');
        std::string host =
            colon == std::string::npos ? bind_address : bind_address.substr(0, colon);
        int port = colon == std::string::npos ? 0 : std::stoi(bind_address.substr(colon + 1));

        _server.Get("/.well-known/acme-challenge/" + _token,
                    [this](const httplib::Request&, httplib::Response& res) {
                        res.set_content(_key_authorization, "text/plain");
                    });
        if (port > 0) {
            // A specific port was requested (production ACME: 80; tests:
            // whatever the CA is configured to validate against) — bind
            // exactly that one, surfacing failure rather than silently
            // listening somewhere the validating server will never check.
            if (!_server.bind_to_port(host, port)) {
                throw std::runtime_error(
                    "acme_certificate_provider: http-01 responder failed to bind " + bind_address);
            }
            _actual_port = port;
        } else {
            _actual_port = _server.bind_to_any_port(host);
            if (_actual_port <= 0) {
                throw std::runtime_error(
                    "acme_certificate_provider: http-01 responder failed to bind " + bind_address);
            }
        }
        _thread = std::jthread([this](std::stop_token) { _server.listen_after_bind(); });
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!_server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~http01_responder() {
        _server.stop();
        if (_thread.joinable()) _thread.join();
    }

    http01_responder(const http01_responder&) = delete;
    http01_responder& operator=(const http01_responder&) = delete;

    [[nodiscard]] auto port() const -> int { return _actual_port; }

private:
    httplib::Server _server;
    std::jthread _thread;
    int _actual_port{0};
    std::string _token;
    std::string _key_authorization;
};

// ── dns-01 responder ─────────────────────────────────────────────────────────
// Publishes/removes a TXT record at _acme-challenge.<identifier>. via RFC
// 2136 DNS UPDATE. Deliberately a small, focused TXT-specific UPDATE sender
// rather than a refactor of rfc2136_ldns_discovery's A/AAAA-only
// send_update() (which is a private method of a separate, independently
// tested class) — same ldns UPDATE-building/TSIG/send sequence, applied to a
// different RR type, to avoid risking a regression in already-working,
// already-tested peer-discovery code under this scope's time constraints.

#ifdef KYTHIRA_HAS_LDNS

inline void send_rfc2136_txt_update(const acme_dns01_config& cfg, const std::string& owner_name,
                                    const std::string& txt_value, bool is_delete) {
    ldns_resolver* raw_res = ldns_resolver_new();
    if (raw_res == nullptr)
        throw std::runtime_error("acme_certificate_provider: ldns_resolver_new failed");
    std::unique_ptr<ldns_resolver, void (*)(ldns_resolver*)> res{raw_res, ldns_resolver_free};

    ldns_resolver_set_port(res.get(), cfg.port);
    {
        ldns_rdf* ns_rdf = nullptr;
        if (ldns_str2rdf_a(&ns_rdf, cfg.server.c_str()) != LDNS_STATUS_OK) {
            ldns_str2rdf_aaaa(&ns_rdf, cfg.server.c_str());
        }
        if (ns_rdf != nullptr) {
            ldns_resolver_push_nameserver(res.get(), ns_rdf);
            ldns_rdf_deep_free(ns_rdf);
        }
    }
    if (!cfg.tsig_key_name.empty()) {
        ldns_resolver_set_tsig_keyname(res.get(), cfg.tsig_key_name.c_str());
        ldns_resolver_set_tsig_algorithm(res.get(), cfg.tsig_algorithm.c_str());
        ldns_resolver_set_tsig_keydata(res.get(), cfg.tsig_key_base64.c_str());
    }

    ldns_rdf* zone_rdf = ldns_dname_new_frm_str(cfg.zone.c_str());
    if (zone_rdf == nullptr)
        throw std::runtime_error("acme_certificate_provider: invalid DNS zone: " + cfg.zone);
    std::unique_ptr<ldns_rdf, void (*)(ldns_rdf*)> zone_guard{zone_rdf, ldns_rdf_deep_free};

    ldns_rdf* owner_rdf = ldns_dname_new_frm_str(owner_name.c_str());
    if (owner_rdf == nullptr)
        throw std::runtime_error("acme_certificate_provider: invalid owner name: " + owner_name);
    std::unique_ptr<ldns_rdf, void (*)(ldns_rdf*)> owner_guard{owner_rdf, ldns_rdf_deep_free};

    ldns_rr_list* update_list = ldns_rr_list_new();
    ldns_rr* rr = ldns_rr_new_frm_type(LDNS_RR_TYPE_TXT);
    if (rr == nullptr)
        throw std::runtime_error("acme_certificate_provider: ldns_rr_new_frm_type failed");
    ldns_rr_set_owner(rr, ldns_rdf_clone(owner_rdf));
    ldns_rr_set_ttl(rr, is_delete ? 0 : cfg.ttl);
    ldns_rr_set_type(rr, LDNS_RR_TYPE_TXT);
    ldns_rr_set_class(rr, LDNS_RR_CLASS_IN);

    if (!is_delete) {
        ldns_rdf* txt_rdf = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_STR, txt_value.c_str());
        if (txt_rdf == nullptr) {
            ldns_rr_free(rr);
            throw std::runtime_error("acme_certificate_provider: failed to build TXT rdata");
        }
        ldns_rr_push_rdf(rr, txt_rdf);
    }
    ldns_rr_list_push_rr(update_list, rr);

    ldns_rr_list* prereq_list = ldns_rr_list_new();
    ldns_rr_list* additional_list = ldns_rr_list_new();
    ldns_pkt* raw_pkt = ldns_update_pkt_new(ldns_rdf_clone(zone_rdf), LDNS_RR_CLASS_IN, prereq_list,
                                            update_list, additional_list);
    if (raw_pkt == nullptr)
        throw std::runtime_error("acme_certificate_provider: ldns_update_pkt_new failed");
    std::unique_ptr<ldns_pkt, void (*)(ldns_pkt*)> pkt{raw_pkt, ldns_pkt_free};

    ldns_pkt_set_opcode(pkt.get(), LDNS_PACKET_UPDATE);
    ldns_pkt_set_random_id(pkt.get());
    if (!cfg.tsig_key_name.empty()) {
        ldns_update_pkt_tsig_add(pkt.get(), res.get());
    }

    ldns_pkt* raw_answer = nullptr;
    ldns_status st = ldns_resolver_send_pkt(&raw_answer, res.get(), pkt.get());
    if (st != LDNS_STATUS_OK || raw_answer == nullptr) {
        throw std::runtime_error(
            std::string("acme_certificate_provider: DNS UPDATE send failed: ") +
            ldns_get_errorstr_by_id(st));
    }
    std::unique_ptr<ldns_pkt, void (*)(ldns_pkt*)> answer{raw_answer, ldns_pkt_free};
    if (ldns_pkt_get_rcode(answer.get()) != LDNS_RCODE_NOERROR) {
        throw std::runtime_error(
            "acme_certificate_provider: DNS UPDATE returned non-NOERROR rcode");
    }
}

class dns01_responder {
public:
    dns01_responder(acme_dns01_config cfg, std::string identifier, std::string digest_base64url)
        : _cfg(std::move(cfg)), _owner_name("_acme-challenge." + identifier + ".") {
        send_rfc2136_txt_update(_cfg, _owner_name, digest_base64url, /*is_delete=*/false);
        _published = true;
    }

    ~dns01_responder() {
        if (_published) {
            try {
                send_rfc2136_txt_update(_cfg, _owner_name, "", /*is_delete=*/true);
            } catch (...) {
                // Best-effort removal (Requirement 18.4) — the record's TTL
                // still bounds its lifetime even if this delete fails.
            }
        }
    }

    dns01_responder(const dns01_responder&) = delete;
    dns01_responder& operator=(const dns01_responder&) = delete;

private:
    acme_dns01_config _cfg;
    std::string _owner_name;
    bool _published{false};
};

#endif  // KYTHIRA_HAS_LDNS

}  // namespace acme_detail

inline acme_certificate_provider::acme_certificate_provider(acme_certificate_provider_config config)
    : _config(std::move(config)) {
    if (_config.account_key_pem.has_value()) {
        _account_key = acme_jws::load_private_key(*_config.account_key_pem);
    } else {
        _account_key = acme_jws::generate_p256_key();
    }
}

inline acme_certificate_provider::~acme_certificate_provider() = default;

inline auto acme_certificate_provider::root_certificate_pem() -> kythira::Future<std::string> {
    std::lock_guard lock(_mutex);
    if (_last_root_pem.empty()) {
        return kythira::FutureFactory::makeExceptionalFuture<std::string>(std::make_exception_ptr(
            std::runtime_error("acme_certificate_provider: no certificate has been obtained yet — "
                               "call sign_csr() first")));
    }
    return kythira::FutureFactory::makeReadyFuture(std::string(_last_root_pem));
}

inline auto acme_certificate_provider::sign_csr(std::string csr_pem, csr_signing_options options)
    -> kythira::Future<pem_material> {
    try {
        std::lock_guard lock(_mutex);

        // ── Directory discovery ─────────────────────────────────────────────
        auto dir_split = acme_detail::split_url(_config.directory_url);
        auto dir_client = acme_detail::make_client(dir_split.origin);
        auto dir_res = dir_client->Get(dir_split.path);
        if (!dir_res || dir_res->status != 200) {
            throw std::runtime_error("acme_certificate_provider: GET directory failed: " +
                                     (dir_res ? std::to_string(dir_res->status) : "no response"));
        }
        auto dir = boost::json::parse(dir_res->body).as_object();
        std::string new_nonce_url = std::string(dir.at("newNonce").as_string());
        std::string new_account_url = std::string(dir.at("newAccount").as_string());
        std::string new_order_url = std::string(dir.at("newOrder").as_string());

        auto fetch_nonce = [&](const std::string& url) -> std::string {
            auto s = acme_detail::split_url(url);
            auto client = acme_detail::make_client(s.origin);
            auto res = client->Get(s.path);
            return acme_detail::require_header(res, "Replay-Nonce");
        };

        // JWS-POSTs `payload` (already-serialized JSON, "" for POST-as-GET) to
        // `url`, using the embedded account JWK if no account is known yet,
        // "kid" otherwise. Returns the parsed response body (empty object for
        // a 204/empty body) with the response object itself available via
        // `out_res` for header inspection (Location, Replay-Nonce, etc.).
        auto jws_post = [&](const std::string& url, const std::string& payload,
                            httplib::Result* out_res = nullptr) -> boost::json::value {
            std::string nonce = fetch_nonce(new_nonce_url);
            boost::json::object header;
            header["nonce"] = nonce;
            header["url"] = url;
            if (_account_url.has_value()) {
                header["kid"] = *_account_url;
            } else {
                header["jwk"] = acme_jws::jwk_from_public_key(_account_key.get());
            }
            auto compact = acme_jws::sign(payload, header, _account_key.get());
            auto dot1 = compact.find('.');
            auto dot2 = compact.find('.', dot1 + 1);
            boost::json::object flattened;
            flattened["protected"] = compact.substr(0, dot1);
            flattened["payload"] = compact.substr(dot1 + 1, dot2 - dot1 - 1);
            flattened["signature"] = compact.substr(dot2 + 1);

            auto s = acme_detail::split_url(url);
            auto client = acme_detail::make_client(s.origin);
            auto res = client->Post(s.path, boost::json::serialize(flattened), "application/json");
            if (!res) {
                throw std::runtime_error("acme_certificate_provider: POST " + url +
                                         " failed: " + httplib::to_string(res.error()));
            }
            if (res->status >= 400) {
                std::string detail = res->body;
                try {
                    detail =
                        boost::json::parse(res->body).as_object().at("detail").as_string().c_str();
                } catch (...) {
                }
                throw std::runtime_error("acme_certificate_provider: POST " + url + " returned " +
                                         std::to_string(res->status) + ": " + detail);
            }
            std::string body = res->body;
            if (out_res != nullptr) *out_res = std::move(res);
            if (body.empty()) return boost::json::object{};
            return boost::json::parse(body);
        };

        // ── Account (re)use ──────────────────────────────────────────────────
        if (!_account_url.has_value()) {
            boost::json::object payload;
            payload["termsOfServiceAgreed"] = true;
            if (!_config.contact.empty()) {
                boost::json::array contacts;
                for (const auto& c : _config.contact) contacts.push_back(boost::json::string(c));
                payload["contact"] = contacts;
            }
            httplib::Result res;
            jws_post(new_account_url, boost::json::serialize(payload), &res);
            _account_url = acme_detail::require_header(res, "Location");
        }

        // ── Order creation ───────────────────────────────────────────────────
        // Identifier type is derived from the value itself via classify()
        // (RFC 8555 §7.1.3 / RFC 8738 §3), not from which of dns_names/
        // ip_addresses the caller happened to put it in — an IP literal
        // mistakenly placed in dns_names is still typed "ip". IP identifiers
        // are ordered first so a mixed order's authorizations are processed
        // IP-before-DNS below (Property 21: both dispatch correctly within
        // the same sign_csr() call, even when the DNS identifier's challenge
        // ultimately can't complete in an environment without real DNS
        // infrastructure).
        auto identifier_type = [](const std::string& value) {
            return acme_identifier::classify(value) == acme_identifier::kind::ip ? "ip" : "dns";
        };
        boost::json::array identifiers;
        for (const auto& ip : options.ip_addresses) {
            identifiers.push_back(
                boost::json::object{{"type", identifier_type(ip)}, {"value", ip}});
        }
        for (const auto& dns_name : options.dns_names) {
            identifiers.push_back(
                boost::json::object{{"type", identifier_type(dns_name)}, {"value", dns_name}});
        }
        if (identifiers.empty()) {
            throw std::invalid_argument(
                "acme_certificate_provider: sign_csr requires at least one identifier");
        }

        httplib::Result order_res;
        auto order_body = jws_post(
            new_order_url,
            boost::json::serialize(boost::json::object{{"identifiers", identifiers}}), &order_res);
        std::string order_url = acme_detail::require_header(order_res, "Location");
        auto order_obj = order_body.as_object();
        std::string finalize_url = std::string(order_obj.at("finalize").as_string());

        std::vector<std::string> authz_urls;
        for (const auto& a : order_obj.at("authorizations").as_array())
            authz_urls.emplace_back(a.as_string());

        // ── Per-identifier challenge completion ──────────────────────────────
        auto poll_until =
            [&](const std::string& url, const char* status_field,
                std::initializer_list<const char*> terminal_states) -> boost::json::object {
            auto deadline = std::chrono::steady_clock::now() + _config.poll_timeout;
            while (true) {
                auto body = jws_post(url, "");
                auto obj = body.as_object();
                std::string status = std::string(obj.at(status_field).as_string());
                for (const char* terminal : terminal_states) {
                    if (status == terminal) return obj;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("acme_certificate_provider: timed out polling " + url +
                                             " (last status: " + status + ")");
                }
                std::this_thread::sleep_for(_config.poll_interval);
            }
        };

        for (const auto& authz_url : authz_urls) {
            auto authz_obj = jws_post(authz_url, "").as_object();
            auto identifier = authz_obj.at("identifier").as_object();
            std::string identifier_type = std::string(identifier.at("type").as_string());
            std::string identifier_value = std::string(identifier.at("value").as_string());

            // Requirement 20.3/20.5 (Property 21): IP identifiers always use
            // http-01 — there's no sensible dns-01 zone for a bare IP.
            auto kind =
                identifier_type == "ip" ? acme_identifier::kind::ip : acme_identifier::kind::dns;
            std::string desired_type = acme_identifier::challenge_for(kind, _config.challenge);

            boost::json::object challenge_obj;
            for (const auto& c : authz_obj.at("challenges").as_array()) {
                if (c.as_object().at("type").as_string() == desired_type) {
                    challenge_obj = c.as_object();
                    break;
                }
            }
            if (challenge_obj.empty()) {
                throw std::runtime_error("acme_certificate_provider: server did not offer a " +
                                         desired_type + " challenge for " + identifier_value);
            }
            std::string challenge_url = std::string(challenge_obj.at("url").as_string());
            std::string token = std::string(challenge_obj.at("token").as_string());
            std::string thumbprint = acme_jws::jwk_thumbprint(_account_key.get());
            std::string key_authorization = token + "." + thumbprint;

            // Responder scoped to this block via RAII — torn down on every
            // exit path (success, failure, or an exception unwinding through
            // here), satisfying Property 19 regardless of outcome.
            if (desired_type == "http-01") {
                acme_detail::http01_responder responder(_config.http01_bind_address, token,
                                                        key_authorization);
                jws_post(challenge_url, "{}");
                poll_until(authz_url, "status", {"valid", "invalid"});
            } else {
#ifdef KYTHIRA_HAS_LDNS
                std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
                SHA256(reinterpret_cast<const unsigned char*>(key_authorization.data()),
                       key_authorization.size(), digest.data());
                std::string digest_b64url = acme_jws::base64url_encode(
                    std::vector<unsigned char>(digest.begin(), digest.end()));
                acme_detail::dns01_responder responder(_config.dns01, identifier_value,
                                                       digest_b64url);
                jws_post(challenge_url, "{}");
                poll_until(authz_url, "status", {"valid", "invalid"});
#else
                throw std::runtime_error(
                    "acme_certificate_provider: dns-01 requested but built without "
                    "KYTHIRA_HAS_LDNS");
#endif
            }

            // Re-check the authorization's final status explicitly — poll_until
            // only guarantees a TERMINAL status was reached, not which one.
            auto final_authz = jws_post(authz_url, "").as_object();
            if (final_authz.at("status").as_string() != "valid") {
                throw std::runtime_error("acme_certificate_provider: authorization for " +
                                         identifier_value + " did not validate (status=" +
                                         std::string(final_authz.at("status").as_string()) + ")");
            }
        }

        // ── Finalization ──────────────────────────────────────────────────────
        auto der = acme_detail::der_csr_from_pem(csr_pem);
        std::string csr_b64url = acme_jws::base64url_encode(der);
        jws_post(finalize_url, boost::json::serialize(boost::json::object{{"csr", csr_b64url}}));

        auto final_order = poll_until(order_url, "status", {"valid", "invalid"});
        if (final_order.at("status").as_string() != "valid") {
            throw std::runtime_error("acme_certificate_provider: order did not finalize (status=" +
                                     std::string(final_order.at("status").as_string()) + ")");
        }
        std::string cert_url = std::string(final_order.at("certificate").as_string());

        // ── Certificate download ─────────────────────────────────────────────
        httplib::Result cert_res;
        std::string nonce = fetch_nonce(new_nonce_url);
        boost::json::object header;
        header["nonce"] = nonce;
        header["url"] = cert_url;
        header["kid"] = *_account_url;
        auto compact = acme_jws::sign("", header, _account_key.get());
        auto dot1 = compact.find('.');
        auto dot2 = compact.find('.', dot1 + 1);
        boost::json::object flattened;
        flattened["protected"] = compact.substr(0, dot1);
        flattened["payload"] = compact.substr(dot1 + 1, dot2 - dot1 - 1);
        flattened["signature"] = compact.substr(dot2 + 1);
        auto cert_split = acme_detail::split_url(cert_url);
        auto cert_client = acme_detail::make_client(cert_split.origin);
        cert_res = cert_client->Post(cert_split.path, boost::json::serialize(flattened),
                                     "application/json");
        if (!cert_res || cert_res->status != 200) {
            throw std::runtime_error("acme_certificate_provider: certificate download failed");
        }

        auto blocks = acme_detail::split_pem_chain(cert_res->body);
        if (blocks.empty()) {
            throw std::runtime_error(
                "acme_certificate_provider: certificate response contained no PEM blocks");
        }

        pem_material result;
        result.certificate_pem = blocks.front();
        result.chain_pem = cert_res->body;
        _last_root_pem = blocks.back();

        return kythira::FutureFactory::makeReadyFuture(std::move(result));
    } catch (const std::exception&) {
        return kythira::FutureFactory::makeExceptionalFuture<pem_material>(
            std::current_exception());
    }
}

}  // namespace raft::testing
