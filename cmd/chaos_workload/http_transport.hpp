// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The workload client's transport over cpp-httplib, shared by the
// chaos_workload binary and the docker_chaos fixture that runs the same
// workload from the test process (tests/docker_chaos/chaos_fixture.hpp).

#include "workload.hpp"

#include <httplib.h>

#include <chrono>
#include <string>

namespace chaos_workload {

/// One request per call. A refused connection is the only failure reported
/// as not sent: anything after connect may have reached the server.
[[nodiscard]] inline auto http_transport(std::chrono::milliseconds read_timeout) -> transport {
    return [read_timeout](const std::string& address, const std::string& path,
                          const std::string& body) -> response {
        auto colon = address.rfind(':');
        httplib::Client cli(address.substr(0, colon), std::stoi(address.substr(colon + 1)));
        cli.set_connection_timeout(std::chrono::seconds{1});
        cli.set_read_timeout(read_timeout);
        cli.set_write_timeout(read_timeout);
        auto res = body.empty() ? cli.Get(path) : cli.Post(path, body, "application/json");
        if (!res) {
            const auto err = res.error();
            return response{.status = 0,
                            .body = {},
                            .sent = err != httplib::Error::Connection,
                            .error = httplib::to_string(err)};
        }
        return response{.status = res->status, .body = res->body, .sent = true, .error = {}};
    };
}

}  // namespace chaos_workload
