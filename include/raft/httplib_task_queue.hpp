// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A cpp-httplib task queue whose worker count follows demand, up to a bound.
//
// cpp-httplib serves each accepted connection on one task-queue worker for
// the connection's whole keep-alive life, so its default `ThreadPool` (a fixed
// max(8, cores - 1) workers) serves that many connections at once and parks
// every later one until a worker frees up. A peer that opens a pool of
// keep-alive connections can hold every worker that way, and RPCs on the
// connections still queued time out unserved. Raising the fixed count instead
// would start `max_concurrent_connections` threads (100 by default) in every
// listener whether or not anything connects.
//
// This queue starts a worker only when a connection arrives with none idle,
// up to `max_workers`; past that, connections wait in the queue for a worker
// to finish one, which bounds how many are served at once without refusing
// any. Workers are kept once started, so the count settles at the peak demand
// seen. Refusing connections over the limit is `gated_task_queue`, below,
// which wraps this one.

#include <raft/http_connection_gate.hpp>

#include <httplib.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace kythira::net_bind {

class growing_task_queue final : public httplib::TaskQueue {
public:
    explicit growing_task_queue(std::size_t max_workers) : _max_workers(max_workers) {
        if (max_workers == 0) {
            throw std::invalid_argument("growing_task_queue needs at least one worker");
        }
    }

    ~growing_task_queue() override { shutdown(); }

    growing_task_queue(const growing_task_queue&) = delete;
    auto operator=(const growing_task_queue&) -> growing_task_queue& = delete;

    auto enqueue(std::function<void()> fn) -> bool override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_shutdown) {
            return false;
        }
        _jobs.push_back(std::move(fn));
        // Comparing against idle workers rather than testing for none: two
        // connections accepted before one idle worker wakes would otherwise
        // both wait on it.
        if (_jobs.size() > _idle && _workers.size() < _max_workers) {
            try {
                _workers.emplace_back([this] { run(); });
                return true;
            } catch (const std::system_error&) {
                // Out of threads: an existing worker gets to it eventually;
                // with none, refusing makes httplib close the socket.
                if (_workers.empty()) {
                    _jobs.pop_back();
                    return false;
                }
            }
        }
        _ready.notify_one();
        return true;
    }

    // Runs every queued job before returning, as httplib's own ThreadPool
    // does. Idempotent, so the destructor can call it again.
    auto shutdown() -> void override {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _shutdown = true;
            workers.swap(_workers);
        }
        _ready.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    [[nodiscard]] auto worker_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _workers.size();
    }

private:
    auto run() -> void {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                ++_idle;
                _ready.wait(lock, [this] { return _shutdown || !_jobs.empty(); });
                --_idle;
                if (_jobs.empty()) {
                    break;
                }
                fn = std::move(_jobs.front());
                _jobs.pop_front();
            }
            fn();
        }
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT) && !defined(OPENSSL_IS_BORINGSSL) && \
    !defined(LIBRESSL_VERSION_NUMBER)
        OPENSSL_thread_stop();
#endif
    }

    const std::size_t _max_workers;
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<std::function<void()>> _jobs;
    std::vector<std::thread> _workers;
    std::size_t _idle{0};
    bool _shutdown{false};
};

// `max_concurrent_connections` for cpp_httplib_server
// (.kiro/specs/http-server-request-limits/). httplib calls `enqueue` once per
// accepted socket, before reading from it or starting a TLS handshake, and
// closes the socket when `enqueue` returns false. So a connection past the
// limit is refused here, at the cost of one accept(). An admitted connection
// holds its slot until httplib's job for it, which serves the whole
// keep-alive session and closes the socket, returns.
//
// One gate per cpp_httplib_server, shared by the queue of every listener it
// owns, so a "*" bind's 0.0.0.0 and :: listeners draw on one limit.
class gated_task_queue final : public httplib::TaskQueue {
public:
    gated_task_queue(std::shared_ptr<kythira::http_detail::connection_gate> gate,
                     std::size_t max_workers)
        : _gate(std::move(gate)), _inner(max_workers) {}

    auto enqueue(std::function<void()> fn) -> bool override {
        auto slot = _gate->try_acquire();
        if (!slot) {
            return false;
        }
        // std::function needs a copyable callable, and a slot is move-only.
        auto held = std::make_shared<kythira::http_detail::connection_gate::slot>(std::move(slot));
        // If the inner queue refuses the job (shutting down, out of threads),
        // the lambda is destroyed with it and the slot goes back at once.
        return _inner.enqueue([held = std::move(held), fn = std::move(fn)]() mutable {
            fn();
            held->release();
        });
    }

    auto shutdown() -> void override { _inner.shutdown(); }

private:
    std::shared_ptr<kythira::http_detail::connection_gate> _gate;
    growing_task_queue _inner;
};

}  // namespace kythira::net_bind
