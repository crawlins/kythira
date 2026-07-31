#pragma once

/// @file gcp_privateca_certificate_provider_impl.hpp
/// @brief Method definitions for `gcp_privateca_certificate_provider`. Included
///        by any translation unit that constructs the provider (e.g. the
///        `ca_service` binary and the unit test). Compiled only when
///        `KYTHIRA_HAS_GCP_PRIVATECA` is defined.

#include <raft/future_default.hpp>
#include <raft/gcp_privateca_certificate_provider.hpp>

#ifdef KYTHIRA_HAS_GCP_PRIVATECA

#include <google/cloud/credentials.h>
#include <google/cloud/options.h>
#include <google/protobuf/duration.pb.h>

#include <stdexcept>
#include <string>

namespace raft::testing {

namespace gcp_privateca_detail {

inline auto make_options(const kythira::gcp_client_config& gcp) -> google::cloud::Options {
    google::cloud::Options opts;
    if (!gcp.credentials_json.empty()) {
        opts.set<google::cloud::UnifiedCredentialsOption>(
            google::cloud::MakeServiceAccountCredentials(gcp.credentials_json));
    } else {
        opts.set<google::cloud::UnifiedCredentialsOption>(
            google::cloud::MakeGoogleDefaultCredentials());
    }
    if (!gcp.endpoint_override.empty()) {
        opts.set<google::cloud::EndpointOption>(gcp.endpoint_override);
    }
    return opts;
}

inline auto make_client(const kythira::gcp_client_config& gcp)
    -> google::cloud::privateca_v1::CertificateAuthorityServiceClient {
    return google::cloud::privateca_v1::CertificateAuthorityServiceClient(
        google::cloud::privateca_v1::MakeCertificateAuthorityServiceConnection(make_options(gcp)));
}

/// `projects/{project}/locations/{location}/caPools/{pool}`.
inline auto ca_pool_path(const gcp_privateca_certificate_provider_config& cfg) -> std::string {
    return "projects/" + cfg.gcp.project_id + "/locations/" + cfg.location + "/caPools/" +
           cfg.ca_pool_id;
}

}  // namespace gcp_privateca_detail

inline gcp_privateca_certificate_provider::gcp_privateca_certificate_provider(
    gcp_privateca_certificate_provider_config config)
    : _config(std::move(config)), _client(gcp_privateca_detail::make_client(_config.gcp)) {
    if (_config.gcp.project_id.empty()) {
        throw std::invalid_argument(
            "gcp_privateca_certificate_provider: project_id must be non-empty");
    }
    if (_config.location.empty()) {
        throw std::invalid_argument(
            "gcp_privateca_certificate_provider: location must be non-empty");
    }
    if (_config.ca_pool_id.empty()) {
        throw std::invalid_argument(
            "gcp_privateca_certificate_provider: ca_pool_id must be non-empty");
    }
}

inline auto gcp_privateca_certificate_provider::root_certificate_pem()
    -> kythira::future_default<std::string> {
    try {
        {
            std::lock_guard<std::mutex> lock(_root_cache_mutex);
            if (_root_cache) {
                return kythira::future_factory_default::makeReadyFuture(std::string(*_root_cache));
            }
        }

        fiu_do_on("raft/gcp/privateca/get_certificate_authority",
                  throw std::runtime_error("fault: raft/gcp/privateca/get_certificate_authority"););

        const std::string pool = gcp_privateca_detail::ca_pool_path(_config);

        // Resolve the CA whose root we return: the pinned one, or the pool's
        // first ENABLED CA.
        std::string ca_name;
        if (!_config.certificate_authority_id.empty()) {
            ca_name = pool + "/certificateAuthorities/" + _config.certificate_authority_id;
        } else {
            google::cloud::security::privateca::v1::ListCertificateAuthoritiesRequest req;
            req.set_parent(pool);
            for (auto const& maybe_ca : _client.ListCertificateAuthorities(req)) {
                if (!maybe_ca) {
                    throw std::runtime_error("gcp privateca ListCertificateAuthorities: " +
                                             maybe_ca.status().message());
                }
                if (maybe_ca->state() ==
                    google::cloud::security::privateca::v1::CertificateAuthority::ENABLED) {
                    ca_name = maybe_ca->name();
                    break;
                }
            }
            if (ca_name.empty()) {
                throw std::runtime_error(
                    "gcp privateca: no ENABLED certificate authority in pool " + pool);
            }
        }

        auto ca = _client.GetCertificateAuthority(ca_name);
        if (!ca) {
            throw std::runtime_error("gcp privateca GetCertificateAuthority: " +
                                     ca.status().message());
        }

        // The CA's own certificate chain, root last. Concatenate the PEM blocks
        // so callers get the full chain-to-root, mirroring how
        // aws_acm_pca_provider returns the CA certificate PEM.
        std::string root_pem;
        for (const auto& pem : ca->pem_ca_certificates()) {
            root_pem += pem;
            if (!root_pem.empty() && root_pem.back() != '\n') {
                root_pem += '\n';
            }
        }
        if (root_pem.empty()) {
            throw std::runtime_error("gcp privateca: certificate authority " + ca_name +
                                     " returned no PEM certificates");
        }

        {
            std::lock_guard<std::mutex> lock(_root_cache_mutex);
            _root_cache = root_pem;
        }
        return kythira::future_factory_default::makeReadyFuture(std::move(root_pem));
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<std::string>(
            std::make_exception_ptr(std::runtime_error(
                std::string("gcp_privateca_certificate_provider::root_certificate_pem: ") +
                ex.what())));
    }
}

inline auto gcp_privateca_certificate_provider::sign_csr(std::string csr_pem,
                                                         csr_signing_options options)
    -> kythira::future_default<pem_material> {
    try {
        fiu_do_on("raft/gcp/privateca/create_certificate",
                  throw std::runtime_error("fault: raft/gcp/privateca/create_certificate"););

        google::cloud::security::privateca::v1::CreateCertificateRequest req;
        req.set_parent(gcp_privateca_detail::ca_pool_path(_config));
        if (!_config.certificate_authority_id.empty()) {
            req.set_issuing_certificate_authority_id(_config.certificate_authority_id);
        }

        auto* cert = req.mutable_certificate();
        cert->set_pem_csr(csr_pem);
        if (!_config.certificate_template.empty()) {
            cert->set_certificate_template(_config.certificate_template);
        }
        // Prefer the per-request validity when the caller supplied one; else the
        // configured default.
        auto validity = options.validity.count() > 0
                            ? std::chrono::duration_cast<std::chrono::seconds>(options.validity)
                            : _config.validity;
        cert->mutable_lifetime()->set_seconds(static_cast<std::int64_t>(validity.count()));

        auto issued = _client.CreateCertificate(req);
        if (!issued) {
            throw std::runtime_error("gcp privateca CreateCertificate: " +
                                     issued.status().message());
        }

        pem_material out;
        out.certificate_pem = issued->pem_certificate();
        // pem_certificate_chain is root-last; prepend the leaf so chain_pem is a
        // full leaf→root bundle, matching aws_acm_pca_provider's chain_pem shape.
        out.chain_pem = out.certificate_pem;
        if (!out.chain_pem.empty() && out.chain_pem.back() != '\n') {
            out.chain_pem += '\n';
        }
        for (const auto& pem : issued->pem_certificate_chain()) {
            out.chain_pem += pem;
            if (!out.chain_pem.empty() && out.chain_pem.back() != '\n') {
                out.chain_pem += '\n';
            }
        }
        // serial left 0 — CAS identifies certificates by resource name, not the
        // local monotonic-counter scheme certificate_authority uses.
        return kythira::future_factory_default::makeReadyFuture(std::move(out));
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<pem_material>(
            std::make_exception_ptr(std::runtime_error(
                std::string("gcp_privateca_certificate_provider::sign_csr: ") + ex.what())));
    }
}

inline auto gcp_privateca_certificate_provider::revoke(std::string serial)
    -> kythira::future_default<void> {
    try {
        fiu_do_on("raft/gcp/privateca/revoke_certificate",
                  throw std::runtime_error("fault: raft/gcp/privateca/revoke_certificate"););

        google::cloud::security::privateca::v1::RevokeCertificateRequest req;
        // CAS addresses a certificate by resource name; `serial` here is the
        // certificate resource name (or id) the caller recorded at issuance.
        req.set_name(serial);
        req.set_reason(google::cloud::security::privateca::v1::RevocationReason::
                           REVOCATION_REASON_UNSPECIFIED);

        auto revoked = _client.RevokeCertificate(req);
        if (!revoked) {
            throw std::runtime_error("gcp privateca RevokeCertificate: " +
                                     revoked.status().message());
        }
        return kythira::future_factory_default::makeFuture();
    } catch (const std::exception& ex) {
        return kythira::future_factory_default::makeExceptionalFuture<void>(
            std::make_exception_ptr(std::runtime_error(
                std::string("gcp_privateca_certificate_provider::revoke: ") + ex.what())));
    }
}

}  // namespace raft::testing

#endif  // KYTHIRA_HAS_GCP_PRIVATECA
