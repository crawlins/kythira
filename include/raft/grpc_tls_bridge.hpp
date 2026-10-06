// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grpc_tls_bridge.hpp
/// @brief Hands TLS material to gRPC so a running server, and existing
/// channels' future handshakes, pick up new material without being rebuilt
/// (.kiro/specs/grpc-tls-reload/, Task 3). Private to the gRPC transport.
///
/// gRPC offers no API to swap a built server's credentials. Its TLS
/// credentials instead hold a certificate provider and ask it for material on
/// every handshake, so changing what the provider returns is how material
/// changes. gRPC 1.71.0 (pinned by vcpkg-overlays/grpc) has no public
/// provider that can be updated in memory -- only
/// `StaticDataCertificateProvider` and `FileWatcherCertificateProvider` -- so
/// this bridge takes the design's file-watcher path:
///
/// - It owns a private staging directory (mode 0700, under $XDG_RUNTIME_DIR
///   or the system temporary directory) and points a
///   `FileWatcherCertificateProvider` at `<staging>/current/{key,chain,roots}.pem`.
/// - To publish generation N it writes `gen-N/` (files mode 0600), fsyncs,
///   atomically repoints the `current` symlink with `rename()`, and removes
///   `gen-(N-2)`, keeping one generation back for a watcher mid-read.
/// - The watcher re-reads once per `refresh_interval` (1s minimum), which is
///   the bound on how long new material takes to apply.
///
/// What the watcher does and does not guarantee, read from 1.71.0's
/// `FileWatcherCertificateProvider::ForceUpdate()` and recorded in the
/// spec's tasks.md:
///
/// - It never checks that the key matches the certificate. It detects a torn
///   identity read only by comparing each file's mtime before and after, at
///   one-second (`time_t`) resolution. Two generations written in the same
///   second would look identical to it, so every generation's files get an
///   mtime strictly greater than the last one's. A repoint that lands
///   between reading the key and the chain is then always seen and retried.
/// - It reads the roots and the identity separately. A repoint between those
///   two reads can pair one generation's roots with the next's identity
///   until the watcher's next pass, one `refresh_interval` later. The key
///   and chain still always match.
///
/// Everything is validated here, before it reaches the watcher, because the
/// watcher logs and skips bad files: a failed reload would otherwise be
/// invisible to the caller and to metrics.

#include <raft/grpc_exceptions.hpp>
#include <raft/tls_material_source.hpp>

#include <grpc/grpc_security_constants.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_credentials_options.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::grpc_detail {

class grpc_tls_bridge {
public:
    enum class role {
        server,
        client
    };

    /// @param initial  The first generation. Its shape -- whether it has an
    ///                 identity and whether roots are used -- is fixed for the
    ///                 bridge's lifetime, because gRPC fixes what the
    ///                 provider watches when the credentials are built.
    /// @param require_peer_cert  Server only: request and verify client
    ///                 certificates, which needs roots. Fixed for life
    ///                 (Requirement 7.3).
    /// Throws `grpc_tls_configuration_error` on invalid material or when the
    /// staging directory cannot be created.
    grpc_tls_bridge(role r, bool require_peer_cert, std::chrono::seconds refresh_interval,
                    const tls_material& initial)
        : _role(r), _require_peer_cert(r == role::server && require_peer_cert) {
        if (refresh_interval < std::chrono::seconds(1)) {
            throw grpc_tls_configuration_error(
                "grpc tls: tls_refresh_interval must be at least 1 second (gRPC's minimum)");
        }
        _has_identity = !initial.certificate_chain_pem.empty();
        _watch_roots =
            _role == role::server ? _require_peer_cert : !initial.root_certificates_pem.empty();
        validate(initial);
        if (!_has_identity && !_watch_roots) {
            throw grpc_tls_configuration_error("grpc tls: no TLS material to serve");
        }
        make_staging_directory();
        try {
            stage(initial);
            auto current = _staging / "current";
            _provider = std::make_shared<grpc::experimental::FileWatcherCertificateProvider>(
                _has_identity ? (current / "key.pem").string() : std::string{},
                _has_identity ? (current / "chain.pem").string() : std::string{},
                _watch_roots ? (current / "roots.pem").string() : std::string{},
                static_cast<unsigned int>(refresh_interval.count()));
        } catch (...) {
            remove_staging_directory();
            throw;
        }
        _applied = initial;
    }

    ~grpc_tls_bridge() {
        _provider.reset();  // Joins the watcher thread before its files go.
        remove_staging_directory();
    }

    grpc_tls_bridge(const grpc_tls_bridge&) = delete;
    auto operator=(const grpc_tls_bridge&) -> grpc_tls_bridge& = delete;

    [[nodiscard]] auto server_credentials() const -> std::shared_ptr<grpc::ServerCredentials> {
        grpc::experimental::TlsServerCredentialsOptions options(_provider);
        options.watch_identity_key_cert_pairs();
        if (_require_peer_cert) {
            options.watch_root_certs();
            options.set_cert_request_type(
                GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
        } else {
            options.set_cert_request_type(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
        }
        return grpc::experimental::TlsServerCredentials(options);
    }

    /// Server certificates are always verified, as they were with
    /// `grpc::SslCredentials`; gRPC adds its hostname verifier by default.
    [[nodiscard]] auto channel_credentials() const -> std::shared_ptr<grpc::ChannelCredentials> {
        grpc::experimental::TlsChannelCredentialsOptions options;
        options.set_certificate_provider(_provider);
        if (_watch_roots) {
            options.watch_root_certs();
        }
        if (_has_identity) {
            options.watch_identity_key_cert_pairs();
        }
        options.set_verify_server_certs(true);
        return grpc::experimental::TlsCredentials(options);
    }

    /// @brief Validates @p m and stages it as the next generation. Returns
    /// true when it was new, false when identical to what is applied. Throws
    /// `grpc_tls_configuration_error`, changing nothing, when @p m is
    /// invalid or changes the material's shape.
    auto apply(const tls_material& m) -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        validate(m);
        if (m == _applied) {
            return false;
        }
        stage(m);
        _applied = m;
        return true;
    }

    [[nodiscard]] auto staging_directory() const -> const std::filesystem::path& {
        return _staging;
    }

private:
    auto validate(const tls_material& m) const -> void {
        const bool identity = !m.certificate_chain_pem.empty() || !m.private_key_pem.empty();
        if (_role == role::server && !identity) {
            throw grpc_tls_configuration_error(
                "grpc_server: TLS needs a server certificate and private key");
        }
        if (identity != _has_identity) {
            throw grpc_tls_configuration_error(
                "grpc tls: a reload cannot add or remove the certificate identity");
        }
        if (_watch_roots && m.root_certificates_pem.empty()) {
            throw grpc_tls_configuration_error(
                _role == role::server
                    ? "grpc_server: require_client_cert set but no CA certificate is configured"
                    : "grpc_client: a reload cannot remove the trusted roots");
        }
        if (_role == role::client && !_watch_roots && !m.root_certificates_pem.empty()) {
            throw grpc_tls_configuration_error(
                "grpc_client: a reload cannot add trusted roots to a client using system roots");
        }
        try {
            if (identity) {
                validate_certificate_key_pair(
                    m.certificate_chain_pem, m.private_key_pem,
                    _role == role::server ? "grpc_server cert/key" : "grpc_client cert/key");
            }
            if (_watch_roots) {
                validate_certificate_pem(m.root_certificates_pem, _role == role::server
                                                                      ? "grpc_server ca_cert_pem"
                                                                      : "grpc_client ca_cert_pem");
            }
        } catch (const std::invalid_argument& e) {
            throw grpc_tls_configuration_error(e.what());
        }
    }

    auto make_staging_directory() -> void {
        std::filesystem::path base;
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
            runtime != nullptr && *runtime != '\0' && ::access(runtime, W_OK | X_OK) == 0) {
            base = runtime;
        } else {
            std::error_code ec;
            base = std::filesystem::temp_directory_path(ec);
            if (ec) {
                base = "/tmp";
            }
        }
        std::string tmpl = (base / "kythira-grpc-tls-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {  // mkdtemp creates it 0700.
            throw grpc_tls_configuration_error("grpc tls: cannot create staging directory under " +
                                               base.string());
        }
        _staging = tmpl;
    }

    auto remove_staging_directory() noexcept -> void {
        if (_staging.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(_staging, ec);
    }

    static auto fail(const std::string& what) -> void {
        throw grpc_tls_configuration_error("grpc tls: staging failed: " + what);
    }

    // Writes one file with mode 0600, stamps its mtime, and fsyncs it.
    auto write_file(const std::filesystem::path& path, const std::string& contents,
                    const timespec& mtime) const -> void {
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            fail("open " + path.string());
        }
        std::size_t written = 0;
        while (written < contents.size()) {
            auto n = ::write(fd, contents.data() + written, contents.size() - written);
            if (n < 0) {
                ::close(fd);
                fail("write " + path.string());
            }
            written += static_cast<std::size_t>(n);
        }
        const timespec times[2] = {mtime, mtime};
        if (::futimens(fd, times) != 0 || ::fsync(fd) != 0) {
            ::close(fd);
            fail("stamp " + path.string());
        }
        ::close(fd);
    }

    static auto fsync_directory(const std::filesystem::path& dir) -> void {
        int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    // Publishes @p m as the next generation: gen-N/, then the `current`
    // symlink, then pruning gen-(N-2).
    auto stage(const tls_material& m) -> void {
        const auto gen = _generation + 1;
        const auto dir = _staging / ("gen-" + std::to_string(gen));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);  // Left by an earlier failed attempt.
        if (::mkdir(dir.c_str(), 0700) != 0) {
            fail("mkdir " + dir.string());
        }

        // Strictly increasing whole seconds, so the watcher's time_t mtime
        // comparison can always tell this generation from the last.
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        const auto stamp = std::max(now, _last_mtime + 1);
        const timespec mtime{.tv_sec = static_cast<time_t>(stamp), .tv_nsec = 0};

        try {
            write_file(dir / "chain.pem", m.certificate_chain_pem, mtime);
            write_file(dir / "key.pem", m.private_key_pem, mtime);
            write_file(dir / "roots.pem", m.root_certificates_pem, mtime);
            fsync_directory(dir);

            const auto link_tmp = _staging / "current.tmp";
            std::filesystem::remove(link_tmp, ec);
            if (::symlink(dir.filename().c_str(), link_tmp.c_str()) != 0) {
                fail("symlink " + link_tmp.string());
            }
            if (::rename(link_tmp.c_str(), (_staging / "current").c_str()) != 0) {
                fail("rename onto " + (_staging / "current").string());
            }
            fsync_directory(_staging);
        } catch (...) {
            std::filesystem::remove_all(dir, ec);
            throw;
        }

        _generation = gen;
        _last_mtime = stamp;
        if (gen > 2) {
            std::filesystem::remove_all(_staging / ("gen-" + std::to_string(gen - 2)), ec);
        }
    }

    role _role;
    bool _require_peer_cert;
    bool _has_identity{false};
    bool _watch_roots{false};
    std::filesystem::path _staging;
    std::uint64_t _generation{0};
    std::int64_t _last_mtime{0};
    tls_material _applied;
    std::mutex _mutex;
    std::shared_ptr<grpc::experimental::FileWatcherCertificateProvider> _provider;
};

/// @brief Where one side of the transport gets its TLS material from: PEM
/// strings, file paths, or a material source. Exactly one kind may be used.
struct grpc_tls_inputs {
    std::string cert_pem;
    std::string key_pem;
    std::string ca_pem;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    std::shared_ptr<tls_material_source> source;
    /// Whether the CA fields are material at all. A server that does not
    /// require client certificates ignores them, as it always has.
    bool use_ca{true};
};

/// @brief The reload surface shared by `grpc_server` and `grpc_client`
/// (.kiro/specs/grpc-tls-reload/, Requirements 1-5, 7): resolves the one
/// material source, owns the bridge, applies explicit reloads, subscribes to
/// self-refreshing sources, and runs the file poll for auto-reload.
///
/// Lock order: a source's own locks (held while it notifies subscribers)
/// come before `_reload_mutex`, and nothing holds `_reload_mutex` while
/// calling into the source, so a source publishing on one thread and an
/// explicit reload on another cannot deadlock, and neither can a reload
/// whose refresh() publishes to this very subscriber.
class grpc_tls_reloader {
public:
    /// Called with "succeeded" or "failed" and the source's generation.
    using emitter = std::function<void(std::string_view outcome, std::uint64_t generation)>;

    grpc_tls_reloader(grpc_tls_bridge::role r, std::string who, grpc_tls_inputs in,
                      bool require_peer_cert, std::chrono::seconds refresh_interval, emitter emit)
        : _who(std::move(who)), _emit(std::move(emit)) {
        _source = resolve(r, in);
        if (!_source) {
            if (r == grpc_tls_bridge::role::server) {
                throw grpc_tls_configuration_error(
                    _who + ": TLS is enabled but no server certificate and key are configured");
            }
            return;  // A client trusting the system roots, with no identity.
        }
        auto current = _source->current();
        if (_source->generation() == 0 || !current) {
            throw grpc_tls_configuration_error(
                _who + ": material_source has not published any material yet (generation 0)");
        }
        _bridge =
            std::make_unique<grpc_tls_bridge>(r, require_peer_cert, refresh_interval, *current);
        _loaded_mtimes = file_mtimes();
        if (_source->self_refreshing()) {
            _subscription = _source->subscribe(
                [this](std::shared_ptr<const tls_material> m, std::uint64_t gen) {
                    try {
                        apply(*m, gen);
                    } catch (const std::exception&) {
                        // apply() emitted the failure; the old material stays.
                    }
                });
        }
    }

    ~grpc_tls_reloader() {
        _subscription.reset();  // Waits out a callback already running.
        disable_auto_reload();
    }

    grpc_tls_reloader(const grpc_tls_reloader&) = delete;
    auto operator=(const grpc_tls_reloader&) -> grpc_tls_reloader& = delete;

    [[nodiscard]] auto server_credentials() const -> std::shared_ptr<grpc::ServerCredentials> {
        return _bridge->server_credentials();
    }

    [[nodiscard]] auto channel_credentials() const -> std::shared_ptr<grpc::ChannelCredentials> {
        if (!_bridge) {
            return grpc::SslCredentials(grpc::SslCredentialsOptions{});
        }
        return _bridge->channel_credentials();
    }

    /// @brief Re-reads the configured material, validates it and applies it.
    /// Throws `grpc_tls_configuration_error`, keeping the old material.
    auto reload() -> void {
        if (!_source) {
            // System roots and no identity: nothing to re-read.
            _emit("succeeded", 0);
            return;
        }
        auto before = file_mtimes();
        try {
            _source->refresh();
        } catch (const std::exception& e) {
            _emit("failed", _source->generation());
            throw grpc_tls_configuration_error(_who + ": reload failed: " + e.what());
        }
        apply(*_source->current(), _source->generation());
        std::lock_guard<std::mutex> lock(_reload_mutex);
        _loaded_mtimes = before;
    }

    /// @brief Polls the certificate files and reloads when one changes.
    /// Throws `std::logic_error` unless the material came from `*_path`
    /// fields: static PEM strings have no file to watch, and a material
    /// source detects its own changes.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void {
        if (!_file_backed) {
            throw std::logic_error(
                _who +
                ": enable_auto_reload() needs certificate files (*_path fields); PEM "
                "strings never change and a material_source detects its own changes");
        }
        if (poll_interval.count() <= 0) {
            throw std::invalid_argument(_who + ": auto-reload poll interval must be > 0");
        }
        disable_auto_reload();
        _poll_thread = std::jthread([this, poll_interval](std::stop_token stop) {
            std::mutex wait_mutex;
            std::condition_variable_any wake;
            while (!stop.stop_requested()) {
                {
                    std::unique_lock<std::mutex> lock(wait_mutex);
                    wake.wait_for(lock, stop, poll_interval, [] { return false; });
                }
                if (stop.stop_requested()) {
                    return;
                }
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lock(_reload_mutex);
                    changed = file_mtimes() != _loaded_mtimes;
                }
                if (!changed) {
                    continue;
                }
                try {
                    reload();
                } catch (const std::exception&) {
                    // Emitted as failed; retried at the next poll because the
                    // recorded mtimes did not move.
                }
            }
        });
    }

    /// @brief Stops and joins the poll thread. Idempotent.
    auto disable_auto_reload() -> void {
        if (_poll_thread.joinable()) {
            _poll_thread.request_stop();
            _poll_thread.join();
        }
    }

    [[nodiscard]] auto auto_reload_enabled() const -> bool { return _poll_thread.joinable(); }
    [[nodiscard]] auto source() const -> const std::shared_ptr<tls_material_source>& {
        return _source;
    }

private:
    auto resolve(grpc_tls_bridge::role r, const grpc_tls_inputs& in)
        -> std::shared_ptr<tls_material_source> {
        const bool ca_pem = in.use_ca && !in.ca_pem.empty();
        const bool ca_path = in.use_ca && !in.ca_path.empty();
        const bool any_pem = !in.cert_pem.empty() || !in.key_pem.empty() || ca_pem;
        const bool any_path = !in.cert_path.empty() || !in.key_path.empty() || ca_path;
        const char* cert = r == grpc_tls_bridge::role::server ? "server_cert" : "client_cert";
        const char* key = r == grpc_tls_bridge::role::server ? "server_key" : "client_key";

        if (in.source) {
            if (any_pem || any_path) {
                throw grpc_tls_configuration_error(
                    _who +
                    ": material_source is the only TLS input when set; clear the *_pem "
                    "and *_path fields");
            }
            return in.source;
        }
        auto both = [&](bool pem, bool path, const std::string& item) {
            if (pem && path) {
                throw grpc_tls_configuration_error(_who + ": both " + item + "_pem and " + item +
                                                   "_path are set; use one");
            }
        };
        both(!in.cert_pem.empty(), !in.cert_path.empty(), cert);
        both(!in.key_pem.empty(), !in.key_path.empty(), key);
        both(ca_pem, ca_path, "ca_cert");
        if (any_pem && any_path) {
            throw grpc_tls_configuration_error(
                _who +
                ": TLS material must come entirely from *_pem fields or entirely from "
                "*_path fields");
        }
        try {
            if (any_path) {
                _file_backed = true;
                return std::make_shared<file_tls_material_source>(tls_material_paths{
                    .certificate_chain_path = in.cert_path,
                    .private_key_path = in.key_path,
                    .root_certificates_path = ca_path ? in.ca_path : std::string{}});
            }
            if (any_pem) {
                return std::make_shared<static_tls_material_source>(
                    tls_material{.certificate_chain_pem = in.cert_pem,
                                 .private_key_pem = in.key_pem,
                                 .root_certificates_pem = ca_pem ? in.ca_pem : std::string{}});
            }
        } catch (const grpc_tls_configuration_error&) {
            throw;
        } catch (const std::exception& e) {
            throw grpc_tls_configuration_error(_who + ": " + e.what());
        }
        return nullptr;
    }

    auto apply(const tls_material& m, std::uint64_t generation) -> void {
        std::lock_guard<std::mutex> lock(_reload_mutex);
        try {
            _bridge->apply(m);
        } catch (const std::exception&) {
            _emit("failed", generation);
            throw;
        }
        _emit("succeeded", generation);
    }

    using mtime_list = std::vector<std::optional<std::filesystem::file_time_type>>;

    [[nodiscard]] auto file_mtimes() const -> mtime_list {
        mtime_list out;
        if (!_file_backed) {
            return out;
        }
        const auto& paths = static_cast<const file_tls_material_source&>(*_source).paths();
        for (const auto* p : {&paths.certificate_chain_path, &paths.private_key_path,
                              &paths.root_certificates_path}) {
            if (p->empty()) {
                continue;
            }
            std::error_code ec;
            auto t = std::filesystem::last_write_time(*p, ec);
            out.push_back(ec ? std::nullopt : std::optional(t));
        }
        return out;
    }

    std::string _who;
    emitter _emit;
    bool _file_backed{false};
    std::shared_ptr<tls_material_source> _source;
    std::unique_ptr<grpc_tls_bridge> _bridge;
    std::mutex _reload_mutex;
    mtime_list _loaded_mtimes;  // Guarded by _reload_mutex.
    tls_material_source::subscription _subscription;
    std::jthread _poll_thread;
};

}  // namespace kythira::grpc_detail
