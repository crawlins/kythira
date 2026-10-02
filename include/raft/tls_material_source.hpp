// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file tls_material_source.hpp
/// @brief A transport-neutral source of TLS material (certificate chain,
/// private key, trusted roots) that can change while a transport runs
/// (.kiro/specs/grpc-tls-reload/, Requirement 5).
///
/// A source holds the current material as an immutable snapshot, counts each
/// publish as a new generation, and tells subscribers when it publishes.
/// Three implementations live here:
///
/// - `static_tls_material_source`: fixed PEM strings. Never changes.
/// - `file_tls_material_source`: PEM files, re-read on `refresh()` and, when
///   given a poll interval, on its own thread whenever a file's mtime moves.
/// - `issuing_tls_material_source<P>` (issuing_tls_material_source.hpp):
///   obtains and renews its own certificate from a `certificate_provider`.
///
/// Nothing here depends on a particular transport. Every source validates
/// material before publishing it, so a subscriber only ever sees a complete
/// generation whose key matches its certificate.

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace kythira {

/// @brief One generation of TLS material.
struct tls_material {
    /// Identity certificate chain, leaf first. Empty means no identity.
    std::string certificate_chain_pem;
    /// Private key for the leaf. Empty exactly when the chain is empty.
    std::string private_key_pem;
    /// Trusted roots. Empty means the consumer's default (system roots on a
    /// client; no client verification on a server).
    std::string root_certificates_pem;

    auto operator==(const tls_material&) const -> bool = default;
};

namespace tls_material_detail {

struct bio_deleter {
    auto operator()(BIO* b) const -> void { BIO_free(b); }
};
struct x509_deleter {
    auto operator()(X509* x) const -> void { X509_free(x); }
};
struct pkey_deleter {
    auto operator()(EVP_PKEY* k) const -> void { EVP_PKEY_free(k); }
};

inline auto read_certificate(const std::string& pem) -> std::unique_ptr<X509, x509_deleter> {
    std::unique_ptr<BIO, bio_deleter> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) return nullptr;
    return std::unique_ptr<X509, x509_deleter>(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

inline auto read_private_key(const std::string& pem) -> std::unique_ptr<EVP_PKEY, pkey_deleter> {
    std::unique_ptr<BIO, bio_deleter> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!bio) return nullptr;
    return std::unique_ptr<EVP_PKEY, pkey_deleter>(
        PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

}  // namespace tls_material_detail

/// @brief Throws `std::invalid_argument` unless @p pem holds a PEM certificate.
inline auto validate_certificate_pem(const std::string& pem, std::string_view label) -> void {
    if (pem.empty()) {
        throw std::invalid_argument(std::string(label) + ": certificate PEM is empty");
    }
    if (!tls_material_detail::read_certificate(pem)) {
        throw std::invalid_argument(std::string(label) + ": not a valid PEM certificate");
    }
}

/// @brief Throws `std::invalid_argument` unless @p pem holds a PEM private key.
inline auto validate_private_key_pem(const std::string& pem, std::string_view label) -> void {
    if (pem.empty()) {
        throw std::invalid_argument(std::string(label) + ": private key PEM is empty");
    }
    if (!tls_material_detail::read_private_key(pem)) {
        throw std::invalid_argument(std::string(label) + ": not a valid PEM private key");
    }
}

/// @brief Throws `std::invalid_argument` unless the first certificate in
/// @p cert_pem and the key in @p key_pem parse and belong together.
inline auto validate_certificate_key_pair(const std::string& cert_pem, const std::string& key_pem,
                                          std::string_view label) -> void {
    validate_certificate_pem(cert_pem, label);
    validate_private_key_pem(key_pem, label);
    auto cert = tls_material_detail::read_certificate(cert_pem);
    auto key = tls_material_detail::read_private_key(key_pem);
    if (X509_check_private_key(cert.get(), key.get()) != 1) {
        throw std::invalid_argument(std::string(label) +
                                    ": private key does not match certificate");
    }
}

/// @brief Throws `std::invalid_argument` unless @p m is complete and
/// self-consistent: an identity is a chain and a matching key or neither,
/// roots (when present) parse, and the material is not empty altogether.
inline auto validate_tls_material(const tls_material& m, std::string_view label) -> void {
    const bool has_chain = !m.certificate_chain_pem.empty();
    const bool has_key = !m.private_key_pem.empty();
    if (has_chain != has_key) {
        throw std::invalid_argument(std::string(label) +
                                    ": certificate chain and private key must both be set or "
                                    "both be empty");
    }
    if (!has_chain && m.root_certificates_pem.empty()) {
        throw std::invalid_argument(std::string(label) + ": TLS material is empty");
    }
    if (has_chain) {
        validate_certificate_key_pair(m.certificate_chain_pem, m.private_key_pem, label);
    }
    if (!m.root_certificates_pem.empty()) {
        validate_certificate_pem(m.root_certificates_pem, std::string(label) + " roots");
    }
}

/// @brief Where TLS material comes from, and how a consumer learns it changed.
///
/// Abstract rather than a concept so non-template config structs can hold a
/// `std::shared_ptr<tls_material_source>`.
class tls_material_source {
public:
    using callback =
        std::function<void(std::shared_ptr<const tls_material>, std::uint64_t generation)>;

    /// @brief Subscriber bookkeeping shared by a source and its
    /// subscriptions, so a subscription may outlive its source.
    ///
    /// `mutex` is held for as long as any callback runs. That is what lets a
    /// subscription's destructor promise that, once it returns, its callback
    /// is neither running nor about to start. It is recursive so a callback
    /// may drop its own subscription without deadlocking.
    struct registry {
        std::recursive_mutex mutex;
        std::uint64_t next_id{1};
        std::map<std::uint64_t, callback> callbacks;
    };

    /// @brief RAII handle for one subscription. Destroying or `reset()`ing it
    /// unsubscribes and waits for an in-progress callback to finish.
    class subscription {
    public:
        subscription() = default;
        subscription(std::weak_ptr<registry> r, std::uint64_t id)
            : _registry(std::move(r)), _id(id) {}
        subscription(subscription&& other) noexcept
            : _registry(std::move(other._registry)), _id(std::exchange(other._id, 0)) {}
        auto operator=(subscription&& other) noexcept -> subscription& {
            if (this != &other) {
                reset();
                _registry = std::move(other._registry);
                _id = std::exchange(other._id, 0);
            }
            return *this;
        }
        subscription(const subscription&) = delete;
        auto operator=(const subscription&) -> subscription& = delete;
        ~subscription() { reset(); }

        auto reset() -> void {
            if (auto r = _registry.lock(); r && _id != 0) {
                std::lock_guard<std::recursive_mutex> lock(r->mutex);
                r->callbacks.erase(_id);
            }
            _registry.reset();
            _id = 0;
        }

        [[nodiscard]] auto active() const -> bool { return _id != 0 && !_registry.expired(); }

    private:
        std::weak_ptr<registry> _registry;
        std::uint64_t _id{0};
    };

    virtual ~tls_material_source() = default;

    /// @brief The current material. Null only before the first publish.
    [[nodiscard]] virtual auto current() const -> std::shared_ptr<const tls_material> = 0;
    /// @brief How many times material has been published. 0 means none yet.
    [[nodiscard]] virtual auto generation() const -> std::uint64_t = 0;
    /// @brief Calls @p cb after every later publish, until the returned
    /// handle is destroyed. Callbacks must not throw; one that does is
    /// reported as a failure and ignored.
    [[nodiscard]] virtual auto subscribe(callback cb) -> subscription = 0;
    /// @brief Re-reads the backing store now and publishes if it changed.
    /// Throws `std::invalid_argument` or `std::runtime_error` on invalid or
    /// unreadable material, and the previous material stays current.
    virtual auto refresh() -> void = 0;
    /// @brief True when the source notices changes on its own (file polling,
    /// renewal), so a consumer subscribes instead of polling.
    [[nodiscard]] virtual auto self_refreshing() const -> bool = 0;
};

/// @brief The bookkeeping every source shares: the current snapshot, the
/// generation counter, subscribers, and a failure hook.
class basic_tls_material_source : public tls_material_source {
public:
    /// Called with a description whenever a refresh, poll, renewal or
    /// subscriber callback fails. The source keeps its previous material.
    using failure_handler = std::function<void(std::string_view what)>;

    [[nodiscard]] auto current() const -> std::shared_ptr<const tls_material> override {
        std::lock_guard<std::mutex> lock(_state_mutex);
        return _current;
    }

    [[nodiscard]] auto generation() const -> std::uint64_t override { return _generation.load(); }

    [[nodiscard]] auto subscribe(callback cb) -> subscription override {
        std::lock_guard<std::recursive_mutex> lock(_registry->mutex);
        auto id = _registry->next_id++;
        _registry->callbacks.emplace(id, std::move(cb));
        return subscription(_registry, id);
    }

    auto set_failure_handler(failure_handler handler) -> void {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _on_failure = std::move(handler);
    }

    /// @brief How many failures have been reported, for tests and probes.
    [[nodiscard]] auto failure_count() const -> std::uint64_t { return _failures.load(); }

protected:
    explicit basic_tls_material_source(failure_handler on_failure = {})
        : _on_failure(std::move(on_failure)) {}

    /// @brief Validates @p m and, when it differs from the current material,
    /// makes it current under the next generation and notifies subscribers.
    /// Returns the generation now current. Throws `std::invalid_argument`
    /// without changing anything when @p m is invalid.
    auto publish(tls_material m, std::string_view label) -> std::uint64_t {
        validate_tls_material(m, label);
        auto snapshot = std::make_shared<const tls_material>(std::move(m));
        std::uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            if (_current && *_current == *snapshot) {
                return _generation.load();
            }
            _current = snapshot;
            gen = _generation.load() + 1;
            _generation.store(gen);
        }
        // Serialised by the registry mutex, which also keeps notifications
        // in generation order when two publishes race.
        std::lock_guard<std::recursive_mutex> lock(_registry->mutex);
        if (_current_notified >= gen) {
            return gen;  // A later generation was already delivered.
        }
        auto latest = current();
        auto latest_gen = generation();
        _current_notified = latest_gen;
        for (auto& [id, cb] : _registry->callbacks) {
            try {
                cb(latest, latest_gen);
            } catch (const std::exception& e) {
                report_failure(std::string("tls_material_source subscriber failed: ") + e.what());
            } catch (...) {
                report_failure("tls_material_source subscriber failed");
            }
        }
        return gen;
    }

    auto report_failure(std::string_view what) -> void {
        _failures.fetch_add(1);
        failure_handler handler;
        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            handler = _on_failure;
        }
        if (handler) handler(what);
    }

private:
    mutable std::mutex _state_mutex;
    std::shared_ptr<const tls_material> _current;
    std::atomic<std::uint64_t> _generation{0};
    std::atomic<std::uint64_t> _failures{0};
    std::uint64_t _current_notified{0};  // Guarded by _registry->mutex.
    failure_handler _on_failure;
    std::shared_ptr<registry> _registry = std::make_shared<registry>();
};

/// @brief Fixed material from PEM strings. `refresh()` re-applies the same
/// material, which is a successful no-op.
class static_tls_material_source final : public basic_tls_material_source {
public:
    /// Throws `std::invalid_argument` when @p material is invalid.
    explicit static_tls_material_source(tls_material material) {
        publish(std::move(material), "static_tls_material_source");
    }

    auto refresh() -> void override {}
    [[nodiscard]] auto self_refreshing() const -> bool override { return false; }
};

/// @brief Paths a `file_tls_material_source` reads. Leave a path empty for
/// material that is not wanted, e.g. no roots on a server without mutual TLS.
struct tls_material_paths {
    std::string certificate_chain_path;
    std::string private_key_path;
    std::string root_certificates_path;
};

/// @brief Material read from PEM files.
///
/// Files are read whole on every load. Writers are expected to replace them
/// atomically (write a temporary file, then `rename()`), as
/// `certificate_authority::replace_atomically()` does. A read that still
/// catches a certificate and key from different writes fails validation and
/// leaves the previous material current, rather than publishing half an
/// update.
///
/// With a poll interval the source is self-refreshing: a thread compares
/// every path's mtime against the values from the last successful load and
/// calls `refresh()` when one differs. A failed load is reported and retried
/// at the next poll, and the thread keeps running.
class file_tls_material_source final : public basic_tls_material_source {
public:
    /// Loads the files once and throws (`std::invalid_argument` or
    /// `std::runtime_error`) if they are unreadable or invalid, so a
    /// constructed source is always at generation 1 or later.
    explicit file_tls_material_source(
        tls_material_paths paths,
        std::optional<std::chrono::milliseconds> poll_interval = std::nullopt,
        failure_handler on_failure = {})
        : basic_tls_material_source(std::move(on_failure)), _paths(std::move(paths)) {
        load();
        if (poll_interval) {
            if (poll_interval->count() <= 0) {
                throw std::invalid_argument("file_tls_material_source: poll interval must be > 0");
            }
            _poll_thread = std::jthread(
                [this, interval = *poll_interval](std::stop_token stop) { poll(stop, interval); });
        }
    }

    ~file_tls_material_source() override { stop_polling(); }

    file_tls_material_source(const file_tls_material_source&) = delete;
    auto operator=(const file_tls_material_source&) -> file_tls_material_source& = delete;

    auto refresh() -> void override {
        try {
            load();
        } catch (const std::exception& e) {
            report_failure(std::string("file_tls_material_source: ") + e.what());
            throw;
        }
    }

    [[nodiscard]] auto self_refreshing() const -> bool override { return _poll_thread.joinable(); }

    [[nodiscard]] auto paths() const -> const tls_material_paths& { return _paths; }

    /// @brief Stops and joins the poll thread. Idempotent.
    auto stop_polling() -> void {
        if (_poll_thread.joinable()) {
            _poll_thread.request_stop();
            _poll_thread.join();
        }
    }

private:
    struct mtimes {
        std::optional<std::filesystem::file_time_type> chain, key, roots;
        auto operator==(const mtimes&) const -> bool = default;
    };

    static auto read_file(const std::string& path) -> std::string {
        if (path.empty()) return {};
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read " + path);
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }

    static auto mtime_of(const std::string& path)
        -> std::optional<std::filesystem::file_time_type> {
        if (path.empty()) return std::nullopt;
        std::error_code ec;
        auto t = std::filesystem::last_write_time(path, ec);
        if (ec) return std::nullopt;
        return t;
    }

    auto current_mtimes() const -> mtimes {
        return {mtime_of(_paths.certificate_chain_path), mtime_of(_paths.private_key_path),
                mtime_of(_paths.root_certificates_path)};
    }

    auto load() -> void {
        std::lock_guard<std::mutex> lock(_load_mutex);
        // Taken before reading, so a write that lands mid-read moves the
        // mtime past what is recorded and the next poll reads again.
        auto before = current_mtimes();
        tls_material m{.certificate_chain_pem = read_file(_paths.certificate_chain_path),
                       .private_key_pem = read_file(_paths.private_key_path),
                       .root_certificates_pem = read_file(_paths.root_certificates_path)};
        publish(std::move(m), "file_tls_material_source");
        _loaded_mtimes = before;
    }

    auto poll(std::stop_token stop, std::chrono::milliseconds interval) -> void {
        std::mutex wait_mutex;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
            {
                std::unique_lock<std::mutex> lock(wait_mutex);
                wake.wait_for(lock, stop, interval, [] { return false; });
            }
            if (stop.stop_requested()) return;
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(_load_mutex);
                changed = current_mtimes() != _loaded_mtimes;
            }
            if (!changed) continue;
            try {
                refresh();
            } catch (const std::exception&) {
                // Reported by refresh(); the old material stays and the
                // unchanged recorded mtimes make the next poll try again.
            }
        }
    }

    tls_material_paths _paths;
    std::mutex _load_mutex;
    mtimes _loaded_mtimes;  // Guarded by _load_mutex.
    std::jthread _poll_thread;
};

}  // namespace kythira
