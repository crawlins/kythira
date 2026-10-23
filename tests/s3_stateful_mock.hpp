// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file s3_stateful_mock.hpp
/// @brief An in-memory S3 that holds a cluster's state, for tests that run
///        `aws_s3_client` and `multi_raft_node --persistence s3` with no cloud
///        account (`.kiro/specs/multi-raft-node-s3-persistence/` task 6.1).
///
/// A cpp-httplib server, path-style only, answering the five operations the
/// object engine uses:
///
/// - PUT `/<bucket>/<key>`: stores the body; ETag is the quoted MD5 hex;
///   `If-None-Match: *` and `If-Match` are honoured with 412
///   `PreconditionFailed`; a `Content-MD5` that does not match the body is 400
///   `BadDigest`. PUT `/<bucket>` creates the bucket.
/// - GET `/<bucket>/<key>`: the body and its ETag, or 404 `NoSuchKey`.
/// - DELETE `/<bucket>/<key>`, with `If-Match`.
/// - GET `/<bucket>?list-type=2`: ListObjectsV2 with `prefix`, `max-keys` and
///   `continuation-token`.
///
/// Every bucket exists: a test should not have to create one before the
/// cluster can start, and the engine never asks. Signatures are not verified,
/// for the reason `aws_s3_client_unit_test`'s recording mock gives: the SDK
/// signs, and a mock that checked it would test the SDK.
///
/// It binds to `localhost`, not `127.0.0.1`: the SDK signs the `Host` header
/// it sends, and object-backup-s3-endpoint Requirement 5.2 found that a
/// literal there and a name in the endpoint disagree on some versions.
///
/// **Test knobs**, all off by default: a delay applied to PUT only (so listing
/// and recovery stay fast), a fail-next-N rule by key prefix answering 500
/// `InternalError`, per-prefix PUT counters, and the high-water mark of PUTs
/// in flight at once.
///
/// It is a separate header from the recording mock in
/// `aws_s3_client_unit_test.cpp`, which programs one answer per request and
/// cannot hold a cluster's state.

#include <raft/object_store_persistence.hpp>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::testing {

class s3_stateful_mock {
public:
    struct stored_object {
        std::string body;
        std::string etag;  ///< Quoted, as S3 sends it.
    };

    s3_stateful_mock() {
        // Three hosts with several groups each, every group with up to
        // `append_concurrency` PUTs in flight, outnumber cpp-httplib's
        // default pool; a queued request would read as S3 latency.
        _server.new_task_queue = [] { return new httplib::ThreadPool(64); };
        _server.Put(R"(/([^/]+)/(.+))", [this](const httplib::Request& req,
                                               httplib::Response& res) { handle_put(req, res); });
        _server.Put(R"(/([^/]+)/?)", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;  // CreateBucket: every bucket exists already.
        });
        _server.Get(R"(/([^/]+)/(.+))", [this](const httplib::Request& req,
                                               httplib::Response& res) { handle_get(req, res); });
        _server.Get(R"(/([^/]+)/?)", [this](const httplib::Request& req, httplib::Response& res) {
            handle_list(req, res);
        });
        _server.Delete(R"(/([^/]+)/(.+))",
                       [this](const httplib::Request& req, httplib::Response& res) {
                           handle_delete(req, res);
                       });
        _port = _server.bind_to_any_port("localhost");
        if (_port <= 0) {
            throw std::runtime_error("s3_stateful_mock: cannot bind localhost");
        }
        _thread = std::thread([this] { _server.listen_after_bind(); });
        _server.wait_until_ready();
    }

    ~s3_stateful_mock() {
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    s3_stateful_mock(const s3_stateful_mock&) = delete;
    auto operator=(const s3_stateful_mock&) -> s3_stateful_mock& = delete;
    s3_stateful_mock(s3_stateful_mock&&) = delete;
    auto operator=(s3_stateful_mock&&) -> s3_stateful_mock& = delete;

    [[nodiscard]] auto endpoint() const -> std::string {
        return "http://localhost:" + std::to_string(_port);
    }
    [[nodiscard]] auto port() const -> int { return _port; }

    // ── Knobs ────────────────────────────────────────────────────────────────

    /// Every PUT waits this long before it is applied and answered.
    auto set_put_delay(std::chrono::milliseconds delay) -> void {
        _put_delay_ms.store(delay.count());
    }

    /// The next `count` PUTs whose key starts with `key_prefix` answer 500
    /// `InternalError` and store nothing.
    auto fail_next_puts(std::string key_prefix, std::size_t count) -> void {
        std::lock_guard lock(_mu);
        _fail_prefix = std::move(key_prefix);
        _fail_remaining = count;
    }

    // ── Observations ─────────────────────────────────────────────────────────

    /// Injected failures not yet consumed; zero once every one has fired.
    [[nodiscard]] auto pending_failures() const -> std::size_t {
        std::lock_guard lock(_mu);
        return _fail_remaining;
    }

    /// Successful PUTs (any bucket) whose key starts with `key_prefix`.
    [[nodiscard]] auto put_count(std::string_view key_prefix) const -> std::size_t {
        std::lock_guard lock(_mu);
        return static_cast<std::size_t>(
            std::count_if(_put_log.begin(), _put_log.end(),
                          [&](const std::string& key) { return key.starts_with(key_prefix); }));
    }

    /// Every successful PUT's key, in the order they were applied.
    [[nodiscard]] auto put_log() const -> std::vector<std::string> {
        std::lock_guard lock(_mu);
        return _put_log;
    }

    /// The most PUTs that were in the handler at once since the last reset.
    [[nodiscard]] auto max_concurrent_puts() const -> std::size_t { return _put_high_water.load(); }

    auto reset_counters() -> void {
        std::lock_guard lock(_mu);
        _put_log.clear();
        _put_high_water.store(0);
    }

    [[nodiscard]] auto keys(const std::string& bucket, std::string_view prefix = {}) const
        -> std::vector<std::string> {
        std::lock_guard lock(_mu);
        std::vector<std::string> out;
        const auto b = _buckets.find(bucket);
        if (b == _buckets.end()) {
            return out;
        }
        for (auto it = b->second.lower_bound(std::string(prefix));
             it != b->second.end() && it->first.starts_with(prefix); ++it) {
            out.push_back(it->first);
        }
        return out;
    }

    [[nodiscard]] auto object(const std::string& bucket, const std::string& key) const
        -> std::optional<stored_object> {
        std::lock_guard lock(_mu);
        const auto b = _buckets.find(bucket);
        if (b == _buckets.end()) {
            return std::nullopt;
        }
        const auto it = b->second.find(key);
        if (it == b->second.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    static auto error(httplib::Response& res, int status, std::string_view code,
                      std::string_view message) -> void {
        res.status = status;
        res.set_content(std::string(R"(<?xml version="1.0" encoding="UTF-8"?><Error><Code>)") +
                            std::string(code) + "</Code><Message>" + std::string(message) +
                            "</Message></Error>",
                        "application/xml");
    }

    /// By value: the header accessor returns a temporary.
    static auto unquote(std::string_view s) -> std::string {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
            s = s.substr(1, s.size() - 2);
        }
        return std::string(s);
    }

    static auto xml_escape(std::string_view s) -> std::string {
        std::string out;
        for (const char c : s) {
            switch (c) {
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                default:
                    out.push_back(c);
            }
        }
        return out;
    }

    /// Base64 of the 16 raw bytes `hex` spells: what `Content-MD5` carries.
    static auto md5_hex_to_base64(std::string_view hex) -> std::string {
        static constexpr std::string_view k_alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<unsigned char> raw;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            raw.push_back(
                static_cast<unsigned char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
        }
        std::string out;
        std::size_t i = 0;
        for (; i + 2 < raw.size(); i += 3) {
            const std::uint32_t v = (raw[i] << 16U) | (raw[i + 1] << 8U) | raw[i + 2];
            out += k_alphabet[(v >> 18U) & 63U];
            out += k_alphabet[(v >> 12U) & 63U];
            out += k_alphabet[(v >> 6U) & 63U];
            out += k_alphabet[v & 63U];
        }
        if (raw.size() - i == 1) {
            const std::uint32_t v = raw[i] << 16U;
            out += k_alphabet[(v >> 18U) & 63U];
            out += k_alphabet[(v >> 12U) & 63U];
            out += "==";
        } else if (raw.size() - i == 2) {
            const std::uint32_t v = (raw[i] << 16U) | (raw[i + 1] << 8U);
            out += k_alphabet[(v >> 18U) & 63U];
            out += k_alphabet[(v >> 12U) & 63U];
            out += k_alphabet[(v >> 6U) & 63U];
            out += '=';
        }
        return out;
    }

    auto handle_put(const httplib::Request& req, httplib::Response& res) -> void {
        const auto in_flight = _puts_in_flight.fetch_add(1) + 1;
        auto high = _put_high_water.load();
        while (in_flight > high && !_put_high_water.compare_exchange_weak(high, in_flight)) {
        }
        struct leave {
            std::atomic<std::size_t>& n;
            ~leave() { n.fetch_sub(1); }
        } guard{_puts_in_flight};

        if (const auto delay = _put_delay_ms.load(); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        }

        const std::string bucket = req.matches[1];
        const std::string key = req.matches[2];
        const auto hex = kythira::object_store_persistence_detail::md5_hex(req.body);
        if (req.has_header("Content-MD5") &&
            req.get_header_value("Content-MD5") != md5_hex_to_base64(hex)) {
            error(res, 400, "BadDigest", "The Content-MD5 you specified did not match");
            return;
        }

        std::lock_guard lock(_mu);
        if (_fail_remaining > 0 && key.starts_with(_fail_prefix)) {
            --_fail_remaining;
            error(res, 500, "InternalError", "injected by s3_stateful_mock");
            return;
        }
        auto& objects = _buckets[bucket];
        const auto existing = objects.find(key);
        if (req.has_header("If-None-Match") && req.get_header_value("If-None-Match") == "*" &&
            existing != objects.end()) {
            error(res, 412, "PreconditionFailed", "At least one of the preconditions failed");
            return;
        }
        if (req.has_header("If-Match")) {
            const auto want = unquote(req.get_header_value("If-Match"));
            if (existing == objects.end() || unquote(existing->second.etag) != want) {
                error(res, 412, "PreconditionFailed", "At least one of the preconditions failed");
                return;
            }
        }
        stored_object stored{req.body, "\"" + hex + "\""};
        res.set_header("ETag", stored.etag);
        objects[key] = std::move(stored);
        _put_log.push_back(key);
        res.status = 200;
    }

    auto handle_get(const httplib::Request& req, httplib::Response& res) -> void {
        const std::string bucket = req.matches[1];
        const std::string key = req.matches[2];
        std::lock_guard lock(_mu);
        const auto& objects = _buckets[bucket];
        const auto it = objects.find(key);
        if (it == objects.end()) {
            error(res, 404, "NoSuchKey", "The specified key does not exist.");
            return;
        }
        res.set_header("ETag", it->second.etag);
        res.set_content(it->second.body, "application/octet-stream");
    }

    auto handle_delete(const httplib::Request& req, httplib::Response& res) -> void {
        const std::string bucket = req.matches[1];
        const std::string key = req.matches[2];
        std::lock_guard lock(_mu);
        auto& objects = _buckets[bucket];
        const auto it = objects.find(key);
        if (req.has_header("If-Match")) {
            const auto want = unquote(req.get_header_value("If-Match"));
            if (it == objects.end() || unquote(it->second.etag) != want) {
                error(res, 412, "PreconditionFailed", "At least one of the preconditions failed");
                return;
            }
        }
        if (it != objects.end()) {
            objects.erase(it);
        }
        res.status = 204;
    }

    auto handle_list(const httplib::Request& req, httplib::Response& res) -> void {
        const std::string bucket = req.matches[1];
        const std::string prefix = req.get_param_value("prefix");
        const std::string token = req.get_param_value("continuation-token");
        std::size_t max_keys = 1000;
        if (req.has_param("max-keys")) {
            max_keys = std::max<std::size_t>(1, std::stoul(req.get_param_value("max-keys")));
        }

        std::vector<std::pair<std::string, stored_object>> page;
        bool truncated = false;
        {
            std::lock_guard lock(_mu);
            const auto& objects = _buckets[bucket];
            // The token is the last key of the previous page: opaque to the
            // client, and stable while keys are added behind it.
            auto it = token.empty() ? objects.lower_bound(prefix) : objects.upper_bound(token);
            for (; it != objects.end() && it->first.starts_with(prefix); ++it) {
                if (page.size() == max_keys) {
                    truncated = true;
                    break;
                }
                page.emplace_back(it->first, it->second);
            }
        }

        std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)"
                          R"(<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)";
        xml += "<Name>" + xml_escape(bucket) + "</Name><Prefix>" + xml_escape(prefix) +
               "</Prefix><KeyCount>" + std::to_string(page.size()) + "</KeyCount><MaxKeys>" +
               std::to_string(max_keys) + "</MaxKeys><IsTruncated>" +
               (truncated ? "true" : "false") + "</IsTruncated>";
        if (!token.empty()) {
            xml += "<ContinuationToken>" + xml_escape(token) + "</ContinuationToken>";
        }
        if (truncated) {
            xml += "<NextContinuationToken>" + xml_escape(page.back().first) +
                   "</NextContinuationToken>";
        }
        for (const auto& [key, object] : page) {
            xml += "<Contents><Key>" + xml_escape(key) + "</Key><ETag>" + xml_escape(object.etag) +
                   "</ETag><Size>" + std::to_string(object.body.size()) +
                   "</Size><StorageClass>STANDARD</StorageClass></Contents>";
        }
        xml += "</ListBucketResult>";
        res.set_content(xml, "application/xml");
    }

    httplib::Server _server;
    int _port{};
    std::thread _thread;

    mutable std::mutex _mu;
    std::map<std::string, std::map<std::string, stored_object>> _buckets;
    std::vector<std::string> _put_log;
    std::string _fail_prefix;
    std::size_t _fail_remaining{0};

    std::atomic<std::int64_t> _put_delay_ms{0};
    std::atomic<std::size_t> _puts_in_flight{0};
    std::atomic<std::size_t> _put_high_water{0};
};

}  // namespace kythira::testing
