// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_s3_mock: tests/s3_stateful_mock.hpp as a standalone server, for the
// cairn M3 stack (.kiro/specs/cairn-chaos-testing/ task 14.2).
//
// The S3 side listens on S3_PORT (default 9000) on every interface, so the
// chaos_node containers reach it by service name. A control side on
// CONTROL_PORT (default 9001) turns the mock's test knobs, which chaos_ctl's
// fault.s3 drives:
//
//   GET  /health            200 once both servers are up
//   GET  /control           {"put_delay_ms", "fail_prefix_pending",
//                            "puts", "max_concurrent_puts"}
//   POST /control/put_delay {"ms": N}         every PUT waits N ms (0 clears)
//   POST /control/fail_puts {"prefix": P, "count": N}
//                           the next N PUTs under key prefix P answer 500
//   POST /control/reset     delay 0, no pending failures, counters cleared
//
// Objects live in memory only: a restart of this container loses the whole
// cluster's state, which is why chaos_ctl offers no fault that restarts it.

#include "s3_stateful_mock.hpp"

#include <boost/json.hpp>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

extern "C" void request_stop(int /*sig*/) {
    g_stop.store(true);
}

auto env_int(const char* name, int fallback) -> int {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? std::stoi(v) : fallback;
}

auto bad_request(httplib::Response& res, const std::string& why) -> void {
    res.status = 400;
    res.set_content(boost::json::serialize(boost::json::object{{"error", why}}),
                    "application/json");
}

/// A non-negative integer field, or nullopt.
auto uint_field(const boost::json::object& o, const char* key) -> std::optional<std::int64_t> {
    const auto* v = o.if_contains(key);
    if (v == nullptr || !v->is_int64() || v->as_int64() < 0) {
        return std::nullopt;
    }
    return v->as_int64();
}

}  // namespace

int main() {
    const int s3_port = env_int("S3_PORT", 9000);
    const int control_port = env_int("CONTROL_PORT", 9001);
    std::signal(SIGTERM, request_stop);
    std::signal(SIGINT, request_stop);

    try {
        kythira::testing::s3_stateful_mock mock("0.0.0.0", s3_port);
        std::atomic<std::int64_t> delay_ms{0};

        httplib::Server control;
        control.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"status":"ok"})", "application/json");
        });
        control.Get("/control", [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(boost::json::serialize(boost::json::object{
                                {"put_delay_ms", delay_ms.load()},
                                {"fail_prefix_pending", mock.pending_failures()},
                                {"puts", mock.put_count("")},
                                {"max_concurrent_puts", mock.max_concurrent_puts()}}),
                            "application/json");
        });
        control.Post(
            "/control/put_delay", [&](const httplib::Request& req, httplib::Response& res) {
                boost::system::error_code ec;
                auto body = boost::json::parse(req.body, ec);
                const auto ms =
                    !ec && body.is_object() ? uint_field(body.as_object(), "ms") : std::nullopt;
                if (!ms || *ms > 60'000) {
                    bad_request(res, "ms must be an integer in [0, 60000]");
                    return;
                }
                delay_ms.store(*ms);
                mock.set_put_delay(std::chrono::milliseconds{*ms});
                res.set_content(boost::json::serialize(boost::json::object{{"put_delay_ms", *ms}}),
                                "application/json");
            });
        control.Post(
            "/control/fail_puts", [&](const httplib::Request& req, httplib::Response& res) {
                boost::system::error_code ec;
                auto body = boost::json::parse(req.body, ec);
                if (ec || !body.is_object()) {
                    bad_request(res, "body must be a JSON object");
                    return;
                }
                const auto& o = body.as_object();
                const auto count = uint_field(o, "count");
                const auto* prefix = o.if_contains("prefix");
                if (!count || *count > 10'000 || (prefix != nullptr && !prefix->is_string())) {
                    bad_request(res, "count must be an integer in [0, 10000]; prefix a string");
                    return;
                }
                const std::string p = prefix != nullptr ? std::string(prefix->as_string()) : "";
                mock.fail_next_puts(p, static_cast<std::size_t>(*count));
                res.set_content(
                    boost::json::serialize(boost::json::object{{"prefix", p}, {"count", *count}}),
                    "application/json");
            });
        control.Post("/control/reset", [&](const httplib::Request&, httplib::Response& res) {
            delay_ms.store(0);
            mock.set_put_delay(std::chrono::milliseconds{0});
            mock.fail_next_puts("", 0);
            mock.reset_counters();
            res.set_content(R"({"reset":true})", "application/json");
        });

        if (!control.bind_to_port("0.0.0.0", control_port)) {
            std::cerr << "chaos_s3_mock: cannot bind control port " << control_port << "\n";
            return 2;
        }
        std::thread control_thread([&control] { control.listen_after_bind(); });
        std::cerr << "[chaos_s3_mock] s3 on :" << mock.port() << ", control on :" << control_port
                  << "\n";
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        control.stop();
        control_thread.join();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "chaos_s3_mock: " << e.what() << "\n";
        return 2;
    }
}
