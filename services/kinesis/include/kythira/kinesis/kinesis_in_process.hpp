// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file kinesis_in_process.hpp
/// @brief What the Kinesis module publishes as `kinesis.put_records`
///        (.kiro/specs/service-colocation/ task 5, Requirement 7): the
///        producer calls, taken from a co-located module without a network
///        hop.
///
/// A request reaches the same front end the HTTPS listener feeds, so it is
/// authenticated by its SigV4 signature and authorized on the signer's
/// policies exactly as it would be over the network (Requirement 7.2).
/// Only the calls a producer makes are taken: PutRecords, PutRecord and
/// DescribeStreamSummary. Anything else answers 400, so the interface
/// cannot become a way round the listener for the rest of the API.
///
/// The request's source address is the loopback address, since the call
/// never left the host: an `aws:SourceIp` condition sees 127.0.0.1.

#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_operations.hpp>

#include <raft/module_interfaces.hpp>

#include <boost/json.hpp>

#include <array>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>

namespace kythira::kinesis {

/// @tparam FrontEnd Anything with `handle(const front_end_request&)` to an
///         `aws_json::http_response`: `front_end<Authorizer>` in the
///         daemon, a fake in tests.
template<typename FrontEnd>
class in_process_producer final : public service_node::aws_json_endpoint {
public:
    /// The operations taken, without the target prefix.
    static constexpr std::array<std::string_view, 3> k_operations = {"DescribeStreamSummary",
                                                                     "PutRecord", "PutRecords"};

    /// @param front_end Outlives this object, or `close()` is called
    ///        before it goes.
    explicit in_process_producer(FrontEnd& front_end) : _front_end(&front_end) {}

    [[nodiscard]] auto call(const service_node::caller& /*who*/,
                            const service_node::aws_json_request& request)
        -> service_node::aws_json_response override {
        const std::shared_lock lock(_mutex);
        if (_front_end == nullptr) {
            return refusal(503, "ServiceUnavailableException", "The Kinesis module has stopped.");
        }
        if (!taken(target_of(request))) {
            return refusal(400, "UnknownOperationException",
                           "Only PutRecords, PutRecord and DescribeStreamSummary are taken "
                           "in-process.");
        }
        front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._headers = request.headers;
        r._body = request.body;
        r._source_ip = "127.0.0.1";
        r._secure = true;
        const auto answer = _front_end->handle(r);
        return {static_cast<int>(answer.status), answer.body};
    }

    /// Waits out calls in flight; every later call answers 503.
    void close() {
        const std::unique_lock lock(_mutex);
        _front_end = nullptr;
    }

private:
    [[nodiscard]] static auto target_of(const service_node::aws_json_request& request)
        -> std::string_view {
        for (const auto& [name, value] : request.headers) {
            if (detail::iequals(name, "x-amz-target")) {
                return value;
            }
        }
        return {};
    }

    [[nodiscard]] static auto taken(std::string_view target) -> bool {
        if (!target.starts_with(target_prefix) || target.size() <= target_prefix.size() ||
            target[target_prefix.size()] != '.') {
            return false;
        }
        const auto op = target.substr(target_prefix.size() + 1);
        for (const auto o : k_operations) {
            if (op == o) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static auto refusal(int status, std::string_view type, std::string_view message)
        -> service_node::aws_json_response {
        return {status, boost::json::serialize(
                            boost::json::object{{"__type", type}, {"message", message}})};
    }

    std::shared_mutex _mutex;
    FrontEnd* _front_end;
};

}  // namespace kythira::kinesis
