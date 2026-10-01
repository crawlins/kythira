// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// cpp-httplib front end for net_bind. An httplib::Server owns exactly one
// listening socket, so listening on every address a bind address resolves
// to ("*", or "localhost" with 127.0.0.1 and ::1) takes one server per
// address, each configured identically by the same callback.

#include <raft/net_bind.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <httplib.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kythira::net_bind {

// Splits "host:port" where host may be a bracketed IPv6 literal
// ("[::1]:80") or "*". A missing port yields `default_port`.
inline auto split_host_port(const std::string& spec, int default_port)
    -> std::pair<std::string, int> {
    if (spec.starts_with('[')) {
        auto close = spec.find(']');
        if (close == std::string::npos) {
            throw std::invalid_argument("unterminated '[' in address '" + spec + "'");
        }
        auto host = spec.substr(1, close - 1);
        if (close + 1 < spec.size() && spec[close + 1] == ':') {
            return {host, std::stoi(spec.substr(close + 2))};
        }
        return {host, default_port};
    }
    auto colon = spec.rfind(':');
    if (colon == std::string::npos) return {spec, default_port};
    return {spec.substr(0, colon), std::stoi(spec.substr(colon + 1))};
}

// N httplib servers, one per resolved address, all on one port.
template<typename Server = httplib::Server> class httplib_listeners {
public:
    using configure_fn = std::function<void(Server&)>;
    using make_fn = std::function<std::unique_ptr<Server>()>;

    httplib_listeners() = default;
    ~httplib_listeners() { stop(); }

    httplib_listeners(const httplib_listeners&) = delete;
    auto operator=(const httplib_listeners&) -> httplib_listeners& = delete;
    httplib_listeners(httplib_listeners&&) = delete;
    auto operator=(httplib_listeners&&) -> httplib_listeners& = delete;

    // Resolves `address` (see resolve_bind_addresses()), builds one server
    // per address with `make` and `configure`, and binds each to `port`.
    // With port 0 every server takes the ephemeral port the first one got.
    // An address whose family the kernel lacks is skipped when there are
    // several. Returns the bound port. Throws std::invalid_argument for an
    // unusable address and std::runtime_error when a bind fails.
    auto bind(
        const std::string& address, std::uint16_t port, const configure_fn& configure,
        const char* who, const make_fn& make = [] { return std::make_unique<Server>(); })
        -> std::uint16_t {
        auto endpoints = resolve_bind_addresses(address, who);
        std::vector<std::unique_ptr<Server>> servers;
        for (const auto& ep : endpoints) {
            if (endpoints.size() > 1 && !family_supported(ep.addr.ss_family)) continue;
            auto server = make();
            configure(*server);
            server->set_socket_options([](socket_t sock) {
                httplib::default_socket_options(sock);
                // Harmless on an IPv4 socket. On IPv6 it keeps "::" from also
                // claiming IPv4, so "*" can bind 0.0.0.0 beside it.
                int on = 1;
                ::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
            });
            auto host = endpoint_host(ep);
            if (port == 0) {
                int bound = server->bind_to_any_port(host);
                if (bound <= 0) fail(who, host, port);
                port = static_cast<std::uint16_t>(bound);
            } else if (!server->bind_to_port(host, port)) {
                fail(who, host, port);
            }
            servers.push_back(std::move(server));
        }
        if (servers.empty()) {
            throw std::runtime_error(std::string(who) + ": no usable address family for '" +
                                     address + "'");
        }
        stop();
        for (auto& slot : _raw) slot.store(nullptr);
        std::lock_guard lock(_mu);
        _servers = std::move(servers);
        for (std::size_t i = 0; i < _servers.size() && i < k_max_raw; ++i) {
            _raw[i].store(_servers[i].get());
        }
        _port = port;
        _finished = 0;
        return port;
    }

    // Runs every server's accept loop on its own thread.
    auto start() -> void {
        std::lock_guard lock(_mu);
        for (auto& server : _servers) {
            _threads.emplace_back([this, s = server.get()] {
                s->listen_after_bind();
                std::lock_guard done_lock(_mu);
                ++_finished;
                _done.notify_all();
            });
        }
    }

    // Blocks until every accept loop has returned (after stop()).
    auto wait() -> void {
        std::unique_lock lock(_mu);
        _done.wait(lock, [this] { return _finished >= _threads.size(); });
    }

    // Blocks until every server is accepting.
    auto wait_until_ready() -> void {
        for (auto& server : snapshot()) server->wait_until_ready();
    }

    // Stops every server without joining; safe from a signal handler that
    // previously called httplib::Server::stop() directly.
    auto request_stop() -> void {
        for (auto& slot : _raw) {
            auto* server = slot.load();
            if (server == nullptr) break;
            server->stop();
        }
    }

    // Stops every server and joins its thread.
    auto stop() -> void {
        for (auto& server : snapshot()) server->stop();
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(_mu);
            threads.swap(_threads);
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

    // The servers, in bind order. Valid until the next bind().
    [[nodiscard]] auto servers() const -> std::vector<Server*> { return snapshot(); }

private:
    static constexpr std::size_t k_max_raw = 8;

    [[noreturn]] static auto fail(const char* who, const std::string& host, std::uint16_t port)
        -> void {
        throw std::runtime_error(std::string(who) + ": failed to bind " + host + ":" +
                                 std::to_string(port));
    }

    auto snapshot() const -> std::vector<Server*> {
        std::lock_guard lock(_mu);
        std::vector<Server*> out;
        for (const auto& s : _servers) out.push_back(s.get());
        return out;
    }

    // Lock-free copies of the server pointers for request_stop(), which may
    // run in a signal handler and so must not take _mu.
    std::array<std::atomic<Server*>, k_max_raw> _raw{};
    mutable std::mutex _mu;
    std::condition_variable _done;
    std::vector<std::unique_ptr<Server>> _servers;
    std::vector<std::thread> _threads;
    std::size_t _finished = 0;
    std::uint16_t _port = 0;
};

}  // namespace kythira::net_bind
