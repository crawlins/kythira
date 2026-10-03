// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file issuing_tls_material_source.hpp
/// @brief A TLS material source that obtains and renews its own certificate
/// from a `certificate_provider` (.kiro/specs/grpc-tls-reload/,
/// Requirement 6, Task 7).
///
/// `certificate_provider` answers "sign this CSR"; a transport needs "what is
/// my identity now, and tell me when it changes". This adapter is the one
/// bridge between them, so issuance backends (local CA, ACME, AWS ACM PCA,
/// GCP Private CA, OCI) stay stateless.
///
/// Each issuance generates a fresh key in-process, builds a CSR, has the
/// provider sign it, fetches the provider's root for the trust bundle and
/// publishes the result. Renewal is scheduled once a configured fraction of
/// the certificate's validity has elapsed. A failure keeps the current
/// material, emits `tls_material_source.renewal.failed` and retries with
/// capped exponential backoff; a certificate that expires meanwhile emits
/// `tls_material_source.expired`. A provider whose `chain_pem` does not start
/// with the leaf gets the leaf prepended and emits
/// `tls_material_source.chain.leaf_prepended`, so the provider bug stays
/// visible. Nothing empty is ever published, and the key never touches disk
/// here.

#include <raft/certificate_provider.hpp>
#include <raft/metrics.hpp>
#include <raft/pem_chain.hpp>
#include <raft/tls_material_source.hpp>

#include <openssl/asn1.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace kythira {

/// @brief How an `issuing_tls_material_source` requests and renews.
struct issuing_tls_material_options {
    /// Subject of the CSR. The common name is usually the node's name.
    ::raft::testing::distinguished_name subject;
    /// Key type generated for every issuance (fresh key each time).
    ::raft::testing::key_algorithm algorithm{::raft::testing::key_algorithm::ecdsa_p256};
    /// SANs, key usages and requested validity passed to `sign_csr`.
    ::raft::testing::csr_signing_options signing;
    /// Renew once this fraction of the certificate's validity has elapsed.
    /// Two thirds matches ca-cluster-rpc-mtls Requirement 7.
    double renew_at_fraction{2.0 / 3.0};
    /// First retry delay after a failure; doubles on each further failure.
    std::chrono::milliseconds initial_backoff{std::chrono::seconds(1)};
    /// Upper bound on the retry delay.
    std::chrono::milliseconds max_backoff{std::chrono::minutes(5)};
};

namespace tls_material_detail {

/// @brief The validity window of the first certificate in @p pem.
inline auto certificate_validity(const std::string& pem)
    -> std::pair<std::chrono::system_clock::time_point, std::chrono::system_clock::time_point> {
    auto cert = read_certificate(pem);
    if (!cert) {
        throw std::invalid_argument("issued certificate does not parse");
    }
    auto to_time_point = [](const ASN1_TIME* t) {
        std::tm tm{};
        if (ASN1_TIME_to_tm(t, &tm) != 1) {
            throw std::invalid_argument("issued certificate has an unreadable validity time");
        }
        return std::chrono::system_clock::from_time_t(::timegm(&tm));
    };
    return {to_time_point(X509_get0_notBefore(cert.get())),
            to_time_point(X509_get0_notAfter(cert.get()))};
}

}  // namespace tls_material_detail

/// @brief A self-refreshing source backed by a `certificate_provider`.
///
/// The constructor attempts the first issuance synchronously. If it
/// succeeds the source starts at generation 1; if not, the failure is
/// reported and the source stays at generation 0 while the background thread
/// retries. A transport refuses a source at generation 0, so a node never
/// starts serving TLS without material.
template<::raft::testing::certificate_provider P, typename Metrics = noop_metrics>
requires kythira::metrics<Metrics>
class issuing_tls_material_source final : public basic_tls_material_source {
public:
    issuing_tls_material_source(std::shared_ptr<P> provider, issuing_tls_material_options options,
                                Metrics metrics = {}, failure_handler on_failure = {})
        : basic_tls_material_source(std::move(on_failure)),
          _provider(std::move(provider)),
          _options(std::move(options)),
          _metrics(std::move(metrics)) {
        if (!_provider) {
            throw std::invalid_argument("issuing_tls_material_source: provider is null");
        }
        if (!(_options.renew_at_fraction > 0.0 && _options.renew_at_fraction < 1.0)) {
            throw std::invalid_argument(
                "issuing_tls_material_source: renew_at_fraction must be in (0, 1)");
        }
        if (_options.initial_backoff.count() <= 0 ||
            _options.max_backoff < _options.initial_backoff) {
            throw std::invalid_argument(
                "issuing_tls_material_source: need 0 < initial_backoff <= max_backoff");
        }
        auto next = attempt();
        _thread = std::jthread([this, next](std::stop_token stop) { run(stop, next); });
    }

    ~issuing_tls_material_source() override { stop(); }

    issuing_tls_material_source(const issuing_tls_material_source&) = delete;
    auto operator=(const issuing_tls_material_source&) -> issuing_tls_material_source& = delete;

    /// @brief Issues a new certificate now, whatever the schedule says.
    /// Throws if issuance fails; the current material stays.
    auto refresh() -> void override {
        std::lock_guard<std::mutex> lock(_issue_mutex);
        issue();
    }

    [[nodiscard]] auto self_refreshing() const -> bool override { return true; }

    /// @brief When the current certificate is due for renewal, if one exists.
    [[nodiscard]] auto renewal_due() const -> std::optional<std::chrono::system_clock::time_point> {
        std::lock_guard<std::mutex> lock(_issue_mutex);
        return _renew_at;
    }

    /// @brief Stops and joins the renewal thread. Idempotent.
    auto stop() -> void {
        if (_thread.joinable()) {
            _thread.request_stop();
            _thread.join();
        }
    }

private:
    using clock = std::chrono::system_clock;

    // One issuance: fresh key, CSR, sign, roots, publish. Caller holds
    // _issue_mutex.
    auto issue() -> void {
        ::raft::testing::leaf_certificate_options leaf;
        leaf.subject = _options.subject;
        leaf.algorithm = _options.algorithm;
        leaf.dns_names = _options.signing.dns_names;
        leaf.ip_addresses = _options.signing.ip_addresses;
        leaf.server_auth = _options.signing.server_auth;
        leaf.client_auth = _options.signing.client_auth;
        leaf.validity = _options.signing.validity;
        auto csr = ::raft::testing::generate_key_and_csr(leaf);

        auto signed_cert = _provider->sign_csr(csr.csr_pem, _options.signing).get();
        auto roots = _provider->root_certificate_pem().get();

        auto [not_before, not_after] =
            tls_material_detail::certificate_validity(signed_cert.certificate_pem);
        if (not_after <= not_before) {
            throw std::invalid_argument("issued certificate has an empty validity window");
        }
        // Guard against a provider that returns the issuer chain without the
        // leaf: publishing it would pair the CA's certificate with the leaf's
        // key. Prepend the leaf and say so, rather than fail forever or
        // correct silently (`.kiro/specs/oci-ca-chain-leaf/` Requirement 3).
        std::string chain = signed_cert.chain_pem;
        if (chain.empty()) {
            chain = signed_cert.certificate_pem;
        } else {
            auto certs = pem_chain::split_certificates(chain);
            if (certs.empty() ||
                !pem_chain::same_certificate(certs.front(), signed_cert.certificate_pem)) {
                chain = pem_chain::leaf_first(signed_cert.certificate_pem, chain);
                emit("tls_material_source.chain.leaf_prepended");
            }
        }
        tls_material m{.certificate_chain_pem = std::move(chain),
                       .private_key_pem = std::move(csr.private_key_pem),
                       .root_certificates_pem = std::move(roots)};
        publish(std::move(m), "issuing_tls_material_source");

        auto lifetime =
            std::chrono::duration_cast<std::chrono::milliseconds>(not_after - not_before);
        _renew_at = not_before + std::chrono::duration_cast<clock::duration>(
                                     lifetime * _options.renew_at_fraction);
        _not_after = not_after;
        _expired_reported = false;
        _backoff = _options.initial_backoff;
    }

    // Issues once, reporting any failure. Returns when to try next.
    auto attempt() -> clock::time_point {
        std::lock_guard<std::mutex> lock(_issue_mutex);
        try {
            issue();
            return *_renew_at;
        } catch (const std::exception& e) {
            report_failure(std::string("issuing_tls_material_source: issuance failed: ") +
                           e.what());
            emit("tls_material_source.renewal.failed");
            if (_not_after && clock::now() >= *_not_after && !_expired_reported) {
                _expired_reported = true;
                emit("tls_material_source.expired");
            }
            auto delay = _backoff;
            _backoff = std::min(_backoff * 2, _options.max_backoff);
            return clock::now() + std::chrono::duration_cast<clock::duration>(delay);
        }
    }

    auto run(std::stop_token stop, clock::time_point next) -> void {
        std::mutex wait_mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock(wait_mutex);
                // Wake at the renewal time, and also when the current
                // certificate expires, so `expired` is reported promptly.
                auto until = next;
                {
                    std::lock_guard<std::mutex> issue_lock(_issue_mutex);
                    if (_not_after && !_expired_reported && *_not_after < until) {
                        until = *_not_after;
                    }
                }
                wake.wait_until(lock, stop, until, [] { return false; });
            }
            if (stop.stop_requested()) return;
            {
                std::lock_guard<std::mutex> issue_lock(_issue_mutex);
                // An explicit refresh() renewed early: follow its schedule.
                if (_renew_at && *_renew_at > next) {
                    next = *_renew_at;
                    continue;
                }
                if (clock::now() < next) {
                    if (_not_after && clock::now() >= *_not_after && !_expired_reported) {
                        _expired_reported = true;
                        emit("tls_material_source.expired");
                    }
                    continue;
                }
            }
            next = attempt();
        }
    }

    auto emit(std::string_view name) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_one();
        metric.emit();
    }

    std::shared_ptr<P> _provider;
    issuing_tls_material_options _options;
    Metrics _metrics;
    mutable std::mutex _issue_mutex;
    std::optional<clock::time_point> _renew_at;                    // Guarded by _issue_mutex.
    std::optional<clock::time_point> _not_after;                   // Guarded by _issue_mutex.
    bool _expired_reported{false};                                 // Guarded by _issue_mutex.
    std::chrono::milliseconds _backoff{_options.initial_backoff};  // Guarded by _issue_mutex.
    std::jthread _thread;
};

}  // namespace kythira
