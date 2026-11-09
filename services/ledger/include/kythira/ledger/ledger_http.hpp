// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ledger API over HTTPS with mutual TLS (.kiro/specs/verifiable-ledger/
// task 7, Requirements 8.1-8.3): glue between raft/beast_https_server.hpp
// and ledger_api's transport-free api_request/api_response.
//
//  - Identity. The server is configured with the Kythira CA bundle as
//    client_ca_path, so OpenSSL verifies any certificate a client presents
//    and refuses the handshake on one that does not chain. The principal
//    the role table is keyed on is the certificate's first URI SAN (the
//    identity the Kythira CA issues, e.g. spiffe://kythira/ledger/app); a
//    request without a verified certificate gets 401 before it reaches the
//    API. GET /healthz alone needs no certificate, for load balancers.
//  - Limits. Connection cap, header and body size and the timeouts are the
//    server's (http-server-request-limits), set from the node's config;
//    ledger_http_limits() gives the ledger's defaults.
//  - Bodies. Content-Type application/ion marks an Ion body (text or
//    binary); Accept application/ion asks for Ion binary back. Anything
//    else is JSON, which the API parses as Ion text.
//  - Paths and query values are percent-decoded; a malformed escape is a
//    400.
//  - Threads. The server runs one thread per connection, but ion-c 1.1.3
//    keeps a system symbol table per thread (a THREAD_LOCAL_STORAGE
//    pointer in ion_symbol_table.c) whose heap blocks, about 128 KiB, are
//    never freed when the thread exits. Parsing Ion on connection threads
//    would leak that much per connection, so the handler does no Ion work
//    itself: it hands each request to an ion_worker_pool, a fixed set of
//    long-lived threads, and waits. That also caps how many requests run
//    API work at once, whatever the connection limit.

#include <kythira/ledger/ledger_api.hpp>

#include <raft/beast_https_server.hpp>

#include <boost/beast/http.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace kythira::ledger {

/// The server limits the ledger uses unless the node's config says
/// otherwise: a 4 MiB body holds a full transaction of 128 KiB documents
/// with room to spare.
inline auto ledger_http_limits(https::server_config config = {}) -> https::server_config {
    config.max_body_bytes = std::uint64_t{4} * 1024 * 1024;
    config.max_header_bytes = 16 * 1024;
    config.max_connections = 512;
    return config;
}

/// Decodes %XX escapes (and '+' as a space when `plus_is_space`, as in a
/// query string). nullopt when an escape is malformed.
inline auto percent_decode(std::string_view in, bool plus_is_space) -> std::optional<std::string> {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) {
                return std::nullopt;
            }
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else if (c == '+' && plus_is_space) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Percent-encodes everything but unreserved characters (RFC 3986), for a
/// path segment or a query value.
inline auto percent_encode(std::string_view in) -> std::string {
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::string out;
    for (const char c : in) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(digits[u >> 4]);
            out.push_back(digits[u & 0x0F]);
        }
    }
    return out;
}

/// Whether a media type header names Ion (parameters ignored).
inline auto is_ion_media_type(std::string_view value) -> bool {
    const auto semi = value.find(';');
    auto type = value.substr(0, semi);
    while (!type.empty() && type.back() == ' ') {
        type.remove_suffix(1);
    }
    return type == "application/ion";
}

/// Builds the API's request from an HTTP request. nullopt when the target
/// is malformed.
inline auto to_api_request(const https::request& req, const https::request_context& ctx)
    -> std::optional<api_request> {
    namespace http = boost::beast::http;
    api_request out;
    out.method = std::string(req.method_string());
    const std::string_view target = req.target();
    const auto q = target.find('?');
    auto path = percent_decode(target.substr(0, q), false);
    if (!path) {
        return std::nullopt;
    }
    out.path = std::move(*path);
    if (q != std::string_view::npos) {
        std::string_view rest = target.substr(q + 1);
        while (!rest.empty()) {
            const auto amp = rest.find('&');
            const auto pair = rest.substr(0, amp);
            rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
            if (pair.empty()) {
                continue;
            }
            const auto eq = pair.find('=');
            auto key = percent_decode(pair.substr(0, eq), true);
            auto value = percent_decode(
                eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1), true);
            if (!key || !value) {
                return std::nullopt;
            }
            out.query[std::move(*key)] = std::move(*value);
        }
    }
    out.body = req.body();
    out.body_is_ion = is_ion_media_type(req[http::field::content_type]);
    out.want_ion = is_ion_media_type(req[http::field::accept]);
    if (ctx.client_verified && !ctx.client_uri_sans.empty()) {
        out.principal = ctx.client_uri_sans.front();
    }
    return out;
}

/// An HTTP response carrying an API response.
inline auto to_http_response(const api_response& r, const https::request& req) -> https::response {
    namespace http = boost::beast::http;
    https::response res{static_cast<http::status>(r.status), req.version()};
    res.set(http::field::content_type, r.content_type);
    res.set(http::field::cache_control, "no-store");
    res.body() = r.body;
    res.keep_alive(req.keep_alive());
    res.prepare_payload();
    return res;
}

/// A fixed set of long-lived threads that run the API's Ion work (see the
/// file comment). run() blocks the caller until its task has run.
class ion_worker_pool {
public:
    explicit ion_worker_pool(std::size_t threads) {
        threads = threads == 0 ? 1 : threads;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    }

    ion_worker_pool(const ion_worker_pool&) = delete;
    auto operator=(const ion_worker_pool&) -> ion_worker_pool& = delete;
    ion_worker_pool(ion_worker_pool&&) = delete;
    auto operator=(ion_worker_pool&&) -> ion_worker_pool& = delete;

    /// Lets queued tasks finish, then joins. Each joined thread leaves its
    /// ion-c symbol table behind, so a pool should live as long as the
    /// process; shared() never stops.
    ~ion_worker_pool() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            t.join();
        }
    }

    template<typename Fn> auto run(Fn&& fn) -> std::invoke_result_t<Fn> {
        std::packaged_task<std::invoke_result_t<Fn>()> task(std::forward<Fn>(fn));
        auto result = task.get_future();
        {
            const std::lock_guard lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("ion worker pool: stopped");
            }
            queue_.emplace_back([&task] { task(); });
        }
        cv_.notify_one();
        return result.get();  // the task outlives the wait: it is on this stack
    }

    /// The process-wide pool: one thread per hardware thread, at least
    /// four. Never destroyed, so its threads and their ion-c state live
    /// until exit.
    static auto shared() -> ion_worker_pool& {
        static auto* pool = new ion_worker_pool(  // NOLINT(cppcoreguidelines-owning-memory)
            std::max<std::size_t>(4, std::thread::hardware_concurrency()));
        return *pool;
    }

private:
    auto work() -> void {
        for (;;) {
            std::function<void()> next;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    return;  // stopping, and nothing left to run
                }
                next = std::move(queue_.front());
                queue_.pop_front();
            }
            next();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

/// The HTTPS handler for a ledger API: `api` is usually
/// [&](const api_request& r) { return ledger_api.handle(r); }. The API runs
/// on `pool` (the shared pool by default), never on the connection thread.
inline auto make_ledger_handler(std::function<api_response(const api_request&)> api,
                                ion_worker_pool* pool = nullptr) -> https::handler {
    if (pool == nullptr) {
        pool = &ion_worker_pool::shared();
    }
    return [api = std::move(api), pool](const https::request& req,
                                        const https::request_context& ctx) -> https::response {
        // Fixed messages, written without ion-c: this runs on the
        // connection thread.
        auto error = [&req](int status, std::string_view code, std::string_view message) {
            std::string body = R"({"code":")";
            body.append(code).append(R"(","message":")");
            body.append(message).append(R"(","retryable":false})");
            return to_http_response({status, "application/json", std::move(body)}, req);
        };
        if (req.method() == boost::beast::http::verb::get && req.target() == "/healthz") {
            return to_http_response({200, "application/json", R"({"status":"ok"})"}, req);
        }
        if (!ctx.client_verified || ctx.client_uri_sans.empty()) {
            return error(401, "unauthenticated",
                         "a Kythira CA client certificate with a URI SAN is required");
        }
        auto parsed = to_api_request(req, ctx);
        if (!parsed) {
            return error(400, "invalid_request", "malformed request target");
        }
        return to_http_response(pool->run([&api, &parsed] { return api(*parsed); }), req);
    };
}

}  // namespace kythira::ledger
