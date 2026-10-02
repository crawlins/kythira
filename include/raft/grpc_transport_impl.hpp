// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grpc_transport_impl.hpp
/// @brief `grpc_client<Types>` and `grpc_server<Types>` — the gRPC/Protobuf
/// implementation of the `network_client`/`network_server` concept family and
/// its optional extensions (.kiro/specs/grpc-transport/, Tasks 4-9).
///
/// Both use gRPC's callback API (Requirement 8): the client issues unary calls
/// through `stub->async()` and fulfills a `kythira::Promise` from the
/// completion callback, and the server implements each generated
/// `CallbackService` interface, returning a `grpc::ServerUnaryReactor*` per
/// method. Neither ever blocks a gRPC I/O thread on handler execution or
/// promise continuation — every promise fulfillment and every registered
/// handler runs on `Types::executor_type` instead (Requirements 8.2, 8.4).
///
/// Like the HTTP and CoAP transports (which hardcode `folly::Future<T>`), this
/// transport is Folly-oriented: it fulfills `kythira::Promise<T>` and returns
/// `kythira::Future<T>`, so a conforming `grpc_transport_types` sets
/// `future_template` to `kythira::Future` (see `grpc_kythira_transport_types`).
/// The `raft_grpc_transport` CMake target is gated on Folly's availability
/// accordingly.

#include <raft/grpc_transport.hpp>
#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_message_conversion.hpp>
#include <raft/grpc_target.hpp>
#include <raft/grpc_tls_bridge.hpp>
#include <raft/net_bind.hpp>
#include <raft/network.hpp>
#include <raft/future.hpp>

#include "raft.pb.h"
#include "raft.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira {

namespace grpc_detail {

/// @brief Per-call client state kept alive until the completion callback fires.
///
/// gRPC's callback API requires the `ClientContext`, request, and response to
/// outlive the in-flight RPC; a `shared_ptr` to this struct is captured by the
/// completion callback so all of them (plus the promise and the per-call stub)
/// survive exactly as long as the call.
template<typename ProtoRequest, typename ProtoResponse, typename Response>
struct client_call_state {
    grpc::ClientContext context;
    ProtoRequest request;
    ProtoResponse response;
    kythira::Promise<Response> promise;
    std::shared_ptr<void> stub_keepalive;  ///< Keeps the per-call stub alive.
    std::chrono::steady_clock::time_point start;
};

}  // namespace grpc_detail

// ============================================================================
// grpc_client<Types>
// ============================================================================

/// @brief gRPC implementation of `network_client` and every optional client
/// extension concept (Requirements 1.1, 3-5, 15-17).
///
/// The `executor` is caller-owned (mirroring the Beast transport's caller-owned
/// `io_context`): `Types::executor_type` — e.g. `folly::CPUThreadPoolExecutor`
/// — is neither copyable nor movable, so it is passed by reference and every
/// promise fulfillment is posted onto it (Requirement 8.2).
template<typename Types>
requires grpc_transport_types<Types>
class grpc_client {
public:
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    template<typename T> using future_template = typename Types::template future_template<T>;

    grpc_client(std::unordered_map<std::uint64_t, std::string> node_id_to_target_map,
                grpc_client_config config, metrics_type metrics, executor_type& executor)
        : _node_id_to_target(std::move(node_id_to_target_map)),
          _config(std::move(config)),
          _metrics(std::move(metrics)),
          _executor(executor) {
        _channel_credentials = build_channel_credentials();
        check_plaintext_targets();
    }

    ~grpc_client() {
        _tls.reset();  // Stops reload callbacks and polling before members go.
    }

    grpc_client(const grpc_client&) = delete;
    grpc_client& operator=(const grpc_client&) = delete;

    // ── TLS reload (.kiro/specs/grpc-tls-reload/, Requirements 2, 3) ─────────

    /// @brief Re-reads the client's certificate, key and roots, validates
    /// them and applies them to new handshakes on every channel, cached or
    /// new, within `tls_refresh_interval`. In-flight RPCs finish on their
    /// existing connections. Throws `grpc_tls_configuration_error` and keeps
    /// the old material when the new material is invalid, and
    /// `std::logic_error` when TLS is off.
    auto reload_tls_material() -> void {
        if (!_tls) {
            throw std::logic_error("grpc_client: reload_tls_material() requires enable_tls");
        }
        _tls->reload();
    }

    /// @brief Polls the `*_path` files every @p poll_interval and reloads when
    /// one changes. Throws `std::logic_error` when TLS is off or the material
    /// does not come from files.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void {
        if (!_tls) {
            throw std::logic_error("grpc_client: enable_auto_reload() requires enable_tls");
        }
        _tls->enable_auto_reload(poll_interval);
    }

    /// @brief Stops and joins the auto-reload thread, if any.
    auto disable_auto_reload() -> void {
        if (_tls) _tls->disable_auto_reload();
    }

    // ── network_client ──────────────────────────────────────────────────────

    auto send_request_vote(std::uint64_t target, const request_vote_request<>& request,
                           std::chrono::milliseconds timeout)
        -> future_template<request_vote_response<>> {
        auto channel = get_or_create_channel(target);
        auto stub =
            std::shared_ptr<raft::v1::RaftService::Stub>(raft::v1::RaftService::NewStub(channel));
        return call_unary<raft::v1::RequestVoteRequest, raft::v1::RequestVoteResponse,
                          request_vote_response<>>(
            "request_vote", target, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::RequestVoteRequest* req,
                   raft::v1::RequestVoteResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->RequestVote(ctx, req, resp, std::move(cb));
            });
    }

    auto send_append_entries(std::uint64_t target, const append_entries_request<>& request,
                             std::chrono::milliseconds timeout)
        -> future_template<append_entries_response<>> {
        auto channel = get_or_create_channel(target);
        auto stub =
            std::shared_ptr<raft::v1::RaftService::Stub>(raft::v1::RaftService::NewStub(channel));
        return call_unary<raft::v1::AppendEntriesRequest, raft::v1::AppendEntriesResponse,
                          append_entries_response<>>(
            "append_entries", target, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::AppendEntriesRequest* req,
                   raft::v1::AppendEntriesResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->AppendEntries(ctx, req, resp, std::move(cb));
            });
    }

    auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout)
        -> future_template<install_snapshot_response<>> {
        auto channel = get_or_create_channel(target);
        auto stub =
            std::shared_ptr<raft::v1::RaftService::Stub>(raft::v1::RaftService::NewStub(channel));
        return call_unary<raft::v1::InstallSnapshotRequest, raft::v1::InstallSnapshotResponse,
                          install_snapshot_response<>>(
            "install_snapshot", target, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::InstallSnapshotRequest* req,
                   raft::v1::InstallSnapshotResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->InstallSnapshot(ctx, req, resp, std::move(cb));
            });
    }

    // ── network_client_with_pre_vote (Requirement 16) ─────────────────────────

    auto send_request_pre_vote(std::uint64_t target, const request_pre_vote_request<>& request,
                               std::chrono::milliseconds timeout)
        -> future_template<request_pre_vote_response<>> {
        auto channel = get_or_create_channel(target);
        auto stub = std::shared_ptr<raft::v1::RaftElectionExtensionService::Stub>(
            raft::v1::RaftElectionExtensionService::NewStub(channel));
        return call_unary<raft::v1::RequestPreVoteRequest, raft::v1::RequestPreVoteResponse,
                          request_pre_vote_response<>>(
            "request_pre_vote", target, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::RequestPreVoteRequest* req,
                   raft::v1::RequestPreVoteResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->RequestPreVote(ctx, req, resp, std::move(cb));
            });
    }

    // ── network_client_with_timeout_now (dissertation §3.10) ──────────────────
    //
    // Shares `RaftElectionExtensionService` with PreVote: both are decisions
    // about who becomes leader, and a service per RPC would multiply
    // registrations without buying any independence — the server registers the
    // service if EITHER handler is configured, and gRPC core answers the other
    // method with UNIMPLEMENTED on its own.

    auto send_timeout_now(std::uint64_t target, const timeout_now_request<>& request,
                          std::chrono::milliseconds timeout)
        -> future_template<timeout_now_response<>> {
        auto channel = get_or_create_channel(target);
        auto stub = std::shared_ptr<raft::v1::RaftElectionExtensionService::Stub>(
            raft::v1::RaftElectionExtensionService::NewStub(channel));
        return call_unary<raft::v1::TimeoutNowRequest, raft::v1::TimeoutNowResponse,
                          timeout_now_response<>>(
            "timeout_now", target, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::TimeoutNowRequest* req,
                   raft::v1::TimeoutNowResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->TimeoutNow(ctx, req, resp, std::move(cb));
            });
    }

    // ── network_client_with_cluster_join / _cluster_leave (Requirement 15) ────
    // Addressed by contact-address string, not node ID (the joining/leaving
    // node is not yet in the address book) — routed via the address-keyed
    // channel overload (Requirement 15.3).

    auto send_cluster_join_request(const std::string& addr, const cluster_join_request<>& request,
                                   std::chrono::milliseconds timeout)
        -> future_template<cluster_join_response<>> {
        auto channel = get_or_create_channel(addr);
        auto stub = std::shared_ptr<raft::v1::RaftBootstrapService::Stub>(
            raft::v1::RaftBootstrapService::NewStub(channel));
        return call_unary<raft::v1::ClusterJoinRequest, raft::v1::ClusterJoinResponse,
                          cluster_join_response<>>(
            "cluster_join", 0, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::ClusterJoinRequest* req,
                   raft::v1::ClusterJoinResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->ClusterJoin(ctx, req, resp, std::move(cb));
            });
    }

    auto send_cluster_leave_request(const std::string& addr, const cluster_leave_request<>& request,
                                    std::chrono::milliseconds timeout)
        -> future_template<cluster_leave_response<>> {
        auto channel = get_or_create_channel(addr);
        auto stub = std::shared_ptr<raft::v1::RaftBootstrapService::Stub>(
            raft::v1::RaftBootstrapService::NewStub(channel));
        return call_unary<raft::v1::ClusterLeaveRequest, raft::v1::ClusterLeaveResponse,
                          cluster_leave_response<>>(
            "cluster_leave", 0, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::ClusterLeaveRequest* req,
                   raft::v1::ClusterLeaveResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->ClusterLeave(ctx, req, resp, std::move(cb));
            });
    }

    // ── network_client_with_log_fetch (Requirement 17) ────────────────────────

    auto send_fetch_log_entries(std::uint64_t target, const fetch_log_entries_request<>& request,
                                std::chrono::milliseconds timeout)
        -> future_template<fetch_log_entries_response<>> {
        auto channel = get_or_create_channel(target);
        auto stub = std::shared_ptr<raft::v1::RaftPeerReplicationService::Stub>(
            raft::v1::RaftPeerReplicationService::NewStub(channel));
        return call_unary<raft::v1::FetchLogEntriesRequest, raft::v1::FetchLogEntriesResponse,
                          fetch_log_entries_response<>>(
            "fetch_log_entries", target, to_proto(request), stub, timeout,
            [stub](grpc::ClientContext* ctx, const raft::v1::FetchLogEntriesRequest* req,
                   raft::v1::FetchLogEntriesResponse* resp, std::function<void(grpc::Status)> cb) {
                stub->async()->FetchLogEntries(ctx, req, resp, std::move(cb));
            });
    }

private:
    // ── Channel management (Requirement 12.3, Property 6) ─────────────────────

    auto make_channel_arguments() const -> grpc::ChannelArguments {
        grpc::ChannelArguments args;
        args.SetMaxSendMessageSize(static_cast<int>(_config.max_send_message_size));
        args.SetMaxReceiveMessageSize(static_cast<int>(_config.max_receive_message_size));
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,
                    static_cast<int>(std::chrono::milliseconds(_config.keepalive_time).count()));
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
                    static_cast<int>(std::chrono::milliseconds(_config.keepalive_timeout).count()));
        args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
                    _config.keepalive_permit_without_calls ? 1 : 0);
        args.SetString(GRPC_ARG_PRIMARY_USER_AGENT_STRING, _config.user_agent);
        if (!_config.target_name_override.empty()) {
            args.SetSslTargetNameOverride(_config.target_name_override);
        }
        return args;
    }

    auto get_or_create_channel(std::uint64_t target) -> std::shared_ptr<grpc::Channel> {
        std::lock_guard<std::mutex> lock(_mutex);
        if (auto it = _channels.find(target); it != _channels.end()) {
            emit_channel_metric("grpc.client.channel.reused", std::to_string(target));
            return it->second;
        }
        auto target_it = _node_id_to_target.find(target);
        if (target_it == _node_id_to_target.end()) {
            throw grpc_client_error(
                grpc::StatusCode::NOT_FOUND,
                std::format("no gRPC target string configured for node {}", target));
        }
        auto channel = grpc::CreateCustomChannel(target_it->second, _channel_credentials,
                                                 make_channel_arguments());
        _channels.emplace(target, channel);
        emit_channel_metric("grpc.client.channel.created", std::to_string(target));
        return channel;
    }

    // Address-keyed overload for the bootstrap path (Requirement 15.3): the
    // joining/leaving node is not yet a known node ID, so the channel is keyed
    // by its raw contact-address string instead.
    auto get_or_create_channel(const std::string& target_address)
        -> std::shared_ptr<grpc::Channel> {
        std::lock_guard<std::mutex> lock(_mutex);
        if (auto it = _address_channels.find(target_address); it != _address_channels.end()) {
            emit_channel_metric("grpc.client.channel.reused", target_address);
            return it->second;
        }
        // Bootstrap addresses were never vetted at construction. Refuse before
        // creating anything, so a refused address leaves no cached channel
        // behind and a later call with a corrected one is not poisoned.
        require_plaintext_allowed(target_address);
        auto channel = grpc::CreateCustomChannel(target_address, _channel_credentials,
                                                 make_channel_arguments());
        _address_channels.emplace(target_address, channel);
        emit_channel_metric("grpc.client.channel.created", target_address);
        return channel;
    }

    // ── Plaintext gate (.kiro/specs/grpc-plaintext-opt-in/, Requirement 3) ────
    // With TLS off, a channel may only reach this host unless the operator
    // set allow_plaintext. Plaintext Raft RPC is unauthenticated, so a
    // missing certificate must not quietly put the log on the network.

    auto require_plaintext_allowed(const std::string& target) const -> void {
        if (_config.enable_tls || _config.allow_plaintext || grpc_detail::target_is_local(target)) {
            return;
        }
        throw grpc_plaintext_refused_error(
            target, std::format("grpc_client: refusing plaintext, unauthenticated Raft RPC to '{}' "
                                "(enable TLS with enable_tls and ca_cert_pem, dial a loopback "
                                "target, or set allow_plaintext on a network you trust)",
                                target));
    }

    // Every configured target is checked once, here, so the node-keyed
    // channel lookup needs no check of its own. Node IDs are visited in
    // order, so the target an error names does not depend on hash order.
    auto check_plaintext_targets() -> void {
        if (_config.enable_tls) {
            return;
        }
        std::vector<std::uint64_t> ids;
        ids.reserve(_node_id_to_target.size());
        for (const auto& [id, target] : _node_id_to_target) {
            ids.push_back(id);
        }
        std::ranges::sort(ids);
        bool loopback_only = true;
        for (auto id : ids) {
            const auto& target = _node_id_to_target.at(id);
            require_plaintext_allowed(target);
            loopback_only = loopback_only && grpc_detail::target_is_local(target);
        }
        if (_config.allow_plaintext) {
            auto metric = _metrics;
            metric.set_metric_name("grpc.client.plaintext.enabled");
            metric.add_dimension("loopback_only", loopback_only ? "true" : "false");
            metric.add_one();
            metric.emit();
        }
    }

    // ── TLS (Requirement 9) ───────────────────────────────────────────────────

    // TLS material is validated here, at construction, so an invalid
    // certificate, key or pair fails closed with grpc_tls_configuration_error
    // rather than downgrading (Requirement 9.6, Property 7). The credentials
    // read material through a certificate provider, which is what lets
    // reload_tls_material() change it under existing channels
    // (.kiro/specs/grpc-tls-reload/).
    auto build_channel_credentials() -> std::shared_ptr<grpc::ChannelCredentials> {
        if (!_config.enable_tls) {
            return grpc::InsecureChannelCredentials();  // Requirement 9.7 (default off).
        }
        _tls = std::make_unique<grpc_detail::grpc_tls_reloader>(
            grpc_detail::grpc_tls_bridge::role::client, "grpc_client",
            grpc_detail::grpc_tls_inputs{.cert_pem = _config.client_cert_pem,
                                         .key_pem = _config.client_key_pem,
                                         .ca_pem = _config.ca_cert_pem,
                                         .cert_path = _config.client_cert_path,
                                         .key_path = _config.client_key_path,
                                         .ca_path = _config.ca_cert_path,
                                         .source = _config.material_source,
                                         .use_ca = true},
            false, _config.tls_refresh_interval,
            [this](std::string_view outcome, std::uint64_t generation) {
                emit_tls_reload_metric("grpc.client.tls_reload.", outcome, generation);
            });
        return _tls->channel_credentials();
    }

    auto emit_tls_reload_metric(std::string_view prefix, std::string_view outcome,
                                std::uint64_t generation) -> void {
        auto metric = _metrics;
        metric.set_metric_name(std::string(prefix) + std::string(outcome));
        metric.add_dimension("generation", std::to_string(generation));
        metric.add_one();
        metric.emit();
    }

    // ── status mapping (Requirement 11, Property 3) ───────────────────────────

    auto status_to_exception(const grpc::Status& status, std::chrono::milliseconds timeout) const
        -> std::exception_ptr {
        const auto code = status.error_code();
        const std::string message = std::format("gRPC call failed ({}): {}", static_cast<int>(code),
                                                status.error_message());
        switch (code) {
            case grpc::StatusCode::DEADLINE_EXCEEDED:
                return std::make_exception_ptr(grpc_timeout_error(message, timeout));
            case grpc::StatusCode::UNAVAILABLE:
                return std::make_exception_ptr(grpc_connection_error(message));
            case grpc::StatusCode::INVALID_ARGUMENT:
            case grpc::StatusCode::UNIMPLEMENTED:
            case grpc::StatusCode::NOT_FOUND:
            case grpc::StatusCode::FAILED_PRECONDITION:
                return std::make_exception_ptr(grpc_client_error(code, message));
            case grpc::StatusCode::INTERNAL:
            case grpc::StatusCode::UNKNOWN:
            case grpc::StatusCode::DATA_LOSS:
                return std::make_exception_ptr(grpc_server_error(code, message));
            default:
                return std::make_exception_ptr(grpc_transport_error(code, message));
        }
    }

    // ── generic unary call (Requirement 8.1, 8.2, 10, 11, 13.2) ──────────────

    template<typename ProtoRequest, typename ProtoResponse, typename Response, typename Invoke>
    auto call_unary(std::string_view rpc_type, std::uint64_t target_metric, ProtoRequest proto_req,
                    std::shared_ptr<void> stub_keepalive, std::chrono::milliseconds timeout,
                    Invoke invoke) -> future_template<Response> {
        using state_t = grpc_detail::client_call_state<ProtoRequest, ProtoResponse, Response>;
        auto state = std::make_shared<state_t>();
        state->request = std::move(proto_req);
        state->stub_keepalive = std::move(stub_keepalive);
        state->start = std::chrono::steady_clock::now();
        state->context.set_deadline(std::chrono::system_clock::now() +
                                    timeout);  // Requirement 10.1

        auto future = state->promise.getFuture();

        emit_call_metric("grpc.client.call.sent", rpc_type, target_metric);
        emit_size_metric("grpc.client.call.request_size", rpc_type, target_metric,
                         static_cast<double>(state->request.ByteSizeLong()));

        std::function<void(grpc::Status)> completion = [this, state,
                                                        rpc_type = std::string(rpc_type),
                                                        target_metric,
                                                        timeout](grpc::Status status) mutable {
            const auto latency = std::chrono::steady_clock::now() - state->start;
            if (status.ok()) {
                emit_latency_metric("grpc.client.call.latency", rpc_type, target_metric, "ok",
                                    latency);
                emit_size_metric("grpc.client.call.response_size", rpc_type, target_metric,
                                 static_cast<double>(state->response.ByteSizeLong()));
                try {
                    Response resp = from_proto(state->response);
                    _executor.add([p = std::move(state->promise), r = std::move(resp)]() mutable {
                        p.setValue(std::move(r));
                    });
                } catch (...) {
                    auto ex = std::current_exception();
                    _executor.add(
                        [p = std::move(state->promise), ex]() mutable { p.setException(ex); });
                }
            } else {
                emit_latency_metric("grpc.client.call.latency", rpc_type, target_metric, "error",
                                    latency);
                emit_error_metric("grpc.client.error", rpc_type, target_metric,
                                  status.error_code());
                auto ex = status_to_exception(status, timeout);
                _executor.add(
                    [p = std::move(state->promise), ex]() mutable { p.setException(ex); });
            }
        };

        invoke(&state->context, &state->request, &state->response, std::move(completion));
        return future;
    }

    // ── metrics helpers ───────────────────────────────────────────────────────

    auto emit_call_metric(std::string_view name, std::string_view rpc_type, std::uint64_t target)
        -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_one();
        metric.emit();
    }

    auto emit_size_metric(std::string_view name, std::string_view rpc_type, std::uint64_t target,
                          double size) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_value(size);
        metric.emit();
    }

    auto emit_latency_metric(std::string_view name, std::string_view rpc_type, std::uint64_t target,
                             std::string_view status, std::chrono::nanoseconds latency) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_dimension("status", status);
        metric.add_duration(latency);
        metric.emit();
    }

    auto emit_error_metric(std::string_view name, std::string_view rpc_type, std::uint64_t target,
                           grpc::StatusCode code) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("target_node_id", std::to_string(target));
        metric.add_dimension("status_code", std::to_string(static_cast<int>(code)));
        metric.add_one();
        metric.emit();
    }

    auto emit_channel_metric(std::string_view name, std::string_view target) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("target_node_id", target);
        metric.add_one();
        metric.emit();
    }

    std::unordered_map<std::uint64_t, std::string> _node_id_to_target;
    std::unordered_map<std::uint64_t, std::shared_ptr<grpc::Channel>> _channels;
    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> _address_channels;
    std::shared_ptr<grpc::ChannelCredentials> _channel_credentials;
    grpc_client_config _config;
    metrics_type _metrics;
    executor_type& _executor;
    std::mutex _mutex;
    // Null when TLS is off. Declared last so it is destroyed first, but the
    // destructor resets it explicitly anyway: its callbacks use _metrics.
    std::unique_ptr<grpc_detail::grpc_tls_reloader> _tls;
};

// ============================================================================
// grpc_server<Types>
// ============================================================================

/// @brief gRPC implementation of `network_server` and every optional server
/// extension concept (Requirements 1.2, 6-7, 15-17).
///
/// `grpc_server` inherits every generated `CallbackService` interface so one
/// object hosts all four services on one listener. Only the services whose
/// handlers were registered before `start()` are added to the
/// `grpc::ServerBuilder`; a client that calls an unregistered extension service
/// therefore gets `UNIMPLEMENTED` from gRPC core with no application code
/// (Requirement 6.3, Property 5).
template<typename Types>
requires grpc_transport_types<Types>
class grpc_server final : public raft::v1::RaftService::CallbackService,
                          public raft::v1::RaftElectionExtensionService::CallbackService,
                          public raft::v1::RaftBootstrapService::CallbackService,
                          public raft::v1::RaftPeerReplicationService::CallbackService {
public:
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;

    grpc_server(std::string bind_address, std::uint16_t bind_port, grpc_server_config config,
                metrics_type metrics, executor_type& executor)
        : _bind_address(std::move(bind_address)),
          _bind_port(bind_port),
          _config(std::move(config)),
          _metrics(std::move(metrics)),
          _executor(executor) {
        // Build + validate the TLS credentials at construction so invalid TLS
        // material fails closed here with grpc_tls_configuration_error, rather
        // than only when start() is called (Requirement 9.6 / Property 7) —
        // symmetric with the client's _channel_credentials.
        _server_credentials = build_server_credentials();
    }

    ~grpc_server() override {
        _tls.reset();  // Stops reload callbacks and polling before members go.
        if (is_running()) {
            stop();  // Requirement 7.4: never leak the listening port/threads.
        }
    }

    // ── TLS reload (.kiro/specs/grpc-tls-reload/, Requirements 1, 3) ─────────

    /// @brief Re-reads the server's certificate, key and (with
    /// `require_client_cert`) roots, validates them and applies them to every
    /// handshake that starts within `tls_refresh_interval` of returning.
    /// Established connections and streams are not closed. Throws
    /// `grpc_tls_configuration_error` and keeps serving the old material when
    /// the new material is invalid, and `std::logic_error` when TLS is off.
    auto reload_tls_material() -> void {
        if (!_tls) {
            throw std::logic_error("grpc_server: reload_tls_material() requires enable_tls");
        }
        _tls->reload();
    }

    /// @brief Polls the `*_path` files every @p poll_interval and reloads when
    /// one changes; a failed reload is reported and retried at the next poll.
    /// Throws `std::logic_error` when TLS is off or the material does not come
    /// from files. `stop()` and the destructor turn it off.
    auto enable_auto_reload(std::chrono::seconds poll_interval) -> void {
        if (!_tls) {
            throw std::logic_error("grpc_server: enable_auto_reload() requires enable_tls");
        }
        _tls->enable_auto_reload(poll_interval);
    }

    /// @brief Stops and joins the auto-reload thread, if any.
    auto disable_auto_reload() -> void {
        if (_tls) _tls->disable_auto_reload();
    }

    grpc_server(const grpc_server&) = delete;
    grpc_server& operator=(const grpc_server&) = delete;

    // ── handler registration (Requirement 6.1, 15-17) ────────────────────────

    auto register_request_vote_handler(
        std::function<request_vote_response<>(const request_vote_request<>&)> handler) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _request_vote_handler = std::move(handler);
    }

    auto register_append_entries_handler(
        std::function<append_entries_response<>(const append_entries_request<>&)> handler) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _append_entries_handler = std::move(handler);
    }

    auto register_install_snapshot_handler(
        std::function<install_snapshot_response<>(const install_snapshot_request<>&)> handler)
        -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _install_snapshot_handler = std::move(handler);
    }

    auto register_request_pre_vote_handler(
        std::function<request_pre_vote_response<>(const request_pre_vote_request<>&)> handler)
        -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _request_pre_vote_handler = std::move(handler);
    }

    auto register_timeout_now_handler(
        std::function<timeout_now_response<>(const timeout_now_request<>&)> handler) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _timeout_now_handler = std::move(handler);
    }

    auto register_cluster_join_handler(
        std::function<cluster_join_response<>(const cluster_join_request<>&)> handler) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _cluster_join_handler = std::move(handler);
    }

    auto register_cluster_leave_handler(
        std::function<cluster_leave_response<>(const cluster_leave_request<>&)> handler) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _cluster_leave_handler = std::move(handler);
    }

    auto register_fetch_log_entries_handler(
        std::function<fetch_log_entries_response<>(const fetch_log_entries_request<>&)> handler)
        -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _fetch_log_entries_handler = std::move(handler);
    }

    // ── lifecycle (Requirement 7) ─────────────────────────────────────────────

    auto start() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_running.load()) {
            return;  // Requirement 7.3 / 19.5: safe to call repeatedly.
        }

        grpc::ServerBuilder builder;
        // One listening port per address the bind address resolves to, so a
        // name such as "localhost" serves both 127.0.0.1 and ::1 (net_bind.hpp:
        // names come from /etc/hosts only and must be this host's). An IPv6
        // address is bracketed; formatting "::1" as "::1:7000" was misparsed.
        // "*" becomes gRPC's own "[::]", whose wildcard handling already listens
        // on IPv4 and IPv6 alike.
        //
        // The three-argument overload reports back which port was actually
        // bound. That matters when _bind_port is 0: gRPC then asks the kernel
        // for a free port, and this is the only way to learn which one. It also
        // sharpens failure detection -- gRPC sets a selected port to 0 when a
        // bind fails, which is checked below alongside BuildAndStart's result.
        std::vector<std::string> listen_addresses;
        std::uint16_t port = _bind_port;
        try {
            if (_bind_address == "*") {
                listen_addresses.push_back("[::]");
            } else {
                auto endpoints =
                    kythira::net_bind::resolve_bind_addresses(_bind_address, "grpc_server");
                for (const auto& ep : endpoints) {
                    if (endpoints.size() > 1 &&
                        !kythira::net_bind::family_supported(ep.addr.ss_family)) {
                        continue;
                    }
                    auto host = kythira::net_bind::endpoint_host(ep);
                    listen_addresses.push_back(ep.addr.ss_family == AF_INET6 ? "[" + host + "]"
                                                                             : host);
                }
            }
            // Several addresses on port 0 must still share one port, but gRPC picks
            // an ephemeral port per address. Reserve one from the kernel first and
            // ask gRPC for that.
            if (port == 0 && listen_addresses.size() > 1) {
                auto fds = kythira::net_bind::open_listeners(
                    kythira::net_bind::resolve_bind_addresses(_bind_address, "grpc_server"), 0,
                    "grpc_server");
                sockaddr_storage bound{};
                socklen_t bound_len = sizeof(bound);
                ::getsockname(fds.front(), reinterpret_cast<sockaddr*>(&bound), &bound_len);
                port = ntohs(bound.ss_family == AF_INET6
                                 ? reinterpret_cast<const sockaddr_in6&>(bound).sin6_port
                                 : reinterpret_cast<const sockaddr_in&>(bound).sin_port);
                for (int fd : fds) ::close(fd);
            }
        } catch (const std::exception& e) {
            throw grpc_transport_error(grpc::StatusCode::INVALID_ARGUMENT,
                                       std::format("grpc_server: cannot bind {}:{}: {}",
                                                   _bind_address, _bind_port, e.what()));
        }
        std::vector<int> selected_ports(listen_addresses.size(), 0);
        for (std::size_t i = 0; i < listen_addresses.size(); ++i) {
            builder.AddListeningPort(std::format("{}:{}", listen_addresses[i], port),
                                     _server_credentials, &selected_ports[i]);
        }
        builder.SetMaxSendMessageSize(static_cast<int>(_config.max_send_message_size));
        builder.SetMaxReceiveMessageSize(static_cast<int>(_config.max_receive_message_size));
        builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS,
                                   static_cast<int>(_config.max_concurrent_rpcs));
        builder.AddChannelArgument(
            GRPC_ARG_KEEPALIVE_TIME_MS,
            static_cast<int>(std::chrono::milliseconds(_config.keepalive_time).count()));
        builder.AddChannelArgument(
            GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
            static_cast<int>(std::chrono::milliseconds(_config.keepalive_timeout).count()));
        if (_config.sync_server_thread_count > 0) {
            builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS,
                                        static_cast<int>(_config.sync_server_thread_count));
        }

        // Always register the base RaftService; register each optional service
        // only if a handler was configured (Requirement 6.3 / Property 5).
        builder.RegisterService(static_cast<raft::v1::RaftService::CallbackService*>(this));
        if (_request_pre_vote_handler || _timeout_now_handler) {
            builder.RegisterService(
                static_cast<raft::v1::RaftElectionExtensionService::CallbackService*>(this));
        }
        if (_cluster_join_handler || _cluster_leave_handler) {
            builder.RegisterService(
                static_cast<raft::v1::RaftBootstrapService::CallbackService*>(this));
        }
        if (_fetch_log_entries_handler) {
            builder.RegisterService(
                static_cast<raft::v1::RaftPeerReplicationService::CallbackService*>(this));
        }

        _server = builder.BuildAndStart();
        bool every_port_bound = !selected_ports.empty() &&
                                std::ranges::none_of(selected_ports, [](int p) { return p == 0; });
        if (!_server || !every_port_bound) {
            _server.reset();
            throw grpc_transport_error(
                grpc::StatusCode::INTERNAL,
                std::format("grpc_server: failed to bind {}:{}", _bind_address, _bind_port));
        }
        _bound_port.store(static_cast<std::uint16_t>(selected_ports.front()));
        _running.store(true);
        emit_lifecycle_metric("grpc.server.started");
        if (!_config.enable_tls && _config.allow_plaintext) {
            auto metric = _metrics;
            metric.set_metric_name("grpc.server.plaintext.enabled");
            metric.add_dimension("bind_address", _bind_address);
            metric.add_dimension(
                "loopback_only",
                kythira::net_bind::is_loopback_bind_address(_bind_address) ? "true" : "false");
            metric.add_one();
            metric.emit();
        }
    }

    auto stop() -> void {
        disable_auto_reload();  // Requirement 3.3.
        std::unique_ptr<grpc::Server> server;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running.load()) {
                return;
            }
            _running.store(false);
            server = std::move(_server);
        }
        if (server) {
            // Graceful drain: refuse new calls, let in-flight RPCs finish up to
            // the grace deadline, then hard-cancel the rest (Requirement 7.2).
            server->Shutdown(std::chrono::system_clock::now() + _config.shutdown_grace_period);
            server->Wait();
        }
        emit_lifecycle_metric("grpc.server.stopped");
    }

    [[nodiscard]] auto is_running() const -> bool { return _running.load(); }

    /// @brief The port this server is actually listening on, or 0 before the
    /// first successful `start()`.
    ///
    /// Equal to the constructor's `bind_port` whenever that was non-zero. Pass 0
    /// instead to have the kernel allocate a free port, then read it back here
    /// to address the server — the only collision-free way to pick a port, since
    /// any port chosen in advance can be taken by another process in between.
    [[nodiscard]] auto bound_port() const -> std::uint16_t { return _bound_port.load(); }

    // ── CallbackService overrides ────────────────────────────────────────────

    grpc::ServerUnaryReactor* RequestVote(grpc::CallbackServerContext* context,
                                          const raft::v1::RequestVoteRequest* request,
                                          raft::v1::RequestVoteResponse* response) override {
        return handle_unary<request_vote_request<>, request_vote_response<>>(
            "request_vote", context, request, response, get_handler(_request_vote_handler));
    }

    grpc::ServerUnaryReactor* AppendEntries(grpc::CallbackServerContext* context,
                                            const raft::v1::AppendEntriesRequest* request,
                                            raft::v1::AppendEntriesResponse* response) override {
        return handle_unary<append_entries_request<>, append_entries_response<>>(
            "append_entries", context, request, response, get_handler(_append_entries_handler));
    }

    grpc::ServerUnaryReactor* InstallSnapshot(
        grpc::CallbackServerContext* context, const raft::v1::InstallSnapshotRequest* request,
        raft::v1::InstallSnapshotResponse* response) override {
        return handle_unary<install_snapshot_request<>, install_snapshot_response<>>(
            "install_snapshot", context, request, response, get_handler(_install_snapshot_handler));
    }

    grpc::ServerUnaryReactor* RequestPreVote(grpc::CallbackServerContext* context,
                                             const raft::v1::RequestPreVoteRequest* request,
                                             raft::v1::RequestPreVoteResponse* response) override {
        return handle_unary<request_pre_vote_request<>, request_pre_vote_response<>>(
            "request_pre_vote", context, request, response, get_handler(_request_pre_vote_handler));
    }

    grpc::ServerUnaryReactor* TimeoutNow(grpc::CallbackServerContext* context,
                                         const raft::v1::TimeoutNowRequest* request,
                                         raft::v1::TimeoutNowResponse* response) override {
        return handle_unary<timeout_now_request<>, timeout_now_response<>>(
            "timeout_now", context, request, response, get_handler(_timeout_now_handler));
    }

    grpc::ServerUnaryReactor* ClusterJoin(grpc::CallbackServerContext* context,
                                          const raft::v1::ClusterJoinRequest* request,
                                          raft::v1::ClusterJoinResponse* response) override {
        return handle_unary<cluster_join_request<>, cluster_join_response<>>(
            "cluster_join", context, request, response, get_handler(_cluster_join_handler));
    }

    grpc::ServerUnaryReactor* ClusterLeave(grpc::CallbackServerContext* context,
                                           const raft::v1::ClusterLeaveRequest* request,
                                           raft::v1::ClusterLeaveResponse* response) override {
        return handle_unary<cluster_leave_request<>, cluster_leave_response<>>(
            "cluster_leave", context, request, response, get_handler(_cluster_leave_handler));
    }

    grpc::ServerUnaryReactor* FetchLogEntries(
        grpc::CallbackServerContext* context, const raft::v1::FetchLogEntriesRequest* request,
        raft::v1::FetchLogEntriesResponse* response) override {
        return handle_unary<fetch_log_entries_request<>, fetch_log_entries_response<>>(
            "fetch_log_entries", context, request, response,
            get_handler(_fetch_log_entries_handler));
    }

private:
    template<typename Handler> auto get_handler(const Handler& handler) -> Handler {
        std::lock_guard<std::mutex> lock(_mutex);
        return handler;  // copy under lock; the RPC runs against this snapshot.
    }

    auto build_server_credentials() -> std::shared_ptr<grpc::ServerCredentials> {
        if (!_config.enable_tls) {
            // Plaintext Raft RPC is unauthenticated: off loopback it needs an
            // explicit opt-in (.kiro/specs/grpc-plaintext-opt-in/, Requirement
            // 2). Checked here, at construction, before any socket exists.
            if (!_config.allow_plaintext &&
                !kythira::net_bind::is_loopback_bind_address(_bind_address)) {
                throw grpc_plaintext_refused_error(
                    _bind_address,
                    std::format("grpc_server: refusing plaintext, unauthenticated Raft RPC on '{}' "
                                "(enable TLS with server_cert_pem/server_key_pem, bind a loopback "
                                "address, or set allow_plaintext on a network you trust)",
                                _bind_address));
            }
            return grpc::InsecureServerCredentials();  // Requirement 9.7 (default off).
        }
        // Validated here so bad material fails closed at construction
        // (Requirement 9.6, Property 7). The credentials read material
        // through a certificate provider, which is what lets
        // reload_tls_material() change it under a running server
        // (.kiro/specs/grpc-tls-reload/). Client roots matter only when
        // client certificates are required, as before.
        _tls = std::make_unique<grpc_detail::grpc_tls_reloader>(
            grpc_detail::grpc_tls_bridge::role::server, "grpc_server",
            grpc_detail::grpc_tls_inputs{.cert_pem = _config.server_cert_pem,
                                         .key_pem = _config.server_key_pem,
                                         .ca_pem = _config.ca_cert_pem,
                                         .cert_path = _config.server_cert_path,
                                         .key_path = _config.server_key_path,
                                         .ca_path = _config.ca_cert_path,
                                         .source = _config.material_source,
                                         .use_ca = _config.require_client_cert},
            _config.require_client_cert, _config.tls_refresh_interval,
            [this](std::string_view outcome, std::uint64_t generation) {
                auto metric = _metrics;
                metric.set_metric_name(std::string("grpc.server.tls_reload.") +
                                       std::string(outcome));
                metric.add_dimension("generation", std::to_string(generation));
                metric.add_one();
                metric.emit();
            });
        return _tls->server_credentials();
    }

    // Generic per-RPC handler (Requirement 6.2-6.5, 8.3, 8.4, 11.6). Returns a
    // reactor immediately and defers all real work (conversion + handler
    // invocation) to the executor so the gRPC I/O thread is never blocked.
    template<typename Request, typename Response, typename ProtoRequest, typename ProtoResponse,
             typename Handler>
    auto handle_unary(std::string_view rpc_type, grpc::CallbackServerContext* context,
                      const ProtoRequest* proto_req, ProtoResponse* proto_resp, Handler handler)
        -> grpc::ServerUnaryReactor* {
        auto* reactor = context->DefaultReactor();
        if (!handler) {
            // No handler registered for a service that *was* registered (e.g. a
            // core RPC left unset) — fail explicitly rather than crash/hang.
            reactor->Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                         std::format("no handler registered for {}", rpc_type)));
            return reactor;
        }

        emit_server_metric("grpc.server.call.received", rpc_type);
        emit_server_size_metric("grpc.server.call.request_size", rpc_type,
                                static_cast<double>(proto_req->ByteSizeLong()));

        // gRPC keeps *proto_req / *proto_resp valid until Finish() is called;
        // copying the request keeps the executor task self-contained regardless.
        ProtoRequest request_copy = *proto_req;
        const auto start = std::chrono::steady_clock::now();

        _executor.add([this, rpc_type = std::string(rpc_type),
                       request_copy = std::move(request_copy), proto_resp, reactor,
                       handler = std::move(handler), start]() mutable {
            Request kythira_request;
            try {
                kythira_request = from_proto(request_copy);  // may throw → INVALID_ARGUMENT
            } catch (const std::exception& e) {
                emit_server_error_metric("grpc.server.error", rpc_type,
                                         grpc::StatusCode::INVALID_ARGUMENT);
                reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what()));
                return;
            }

            try {
                Response kythira_response = handler(kythira_request);
                *proto_resp = to_proto(kythira_response);
                const auto latency = std::chrono::steady_clock::now() - start;
                emit_server_latency_metric("grpc.server.call.latency", rpc_type, "ok", latency);
                emit_server_size_metric("grpc.server.call.response_size", rpc_type,
                                        static_cast<double>(proto_resp->ByteSizeLong()));
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                // Handler threw → INTERNAL, never crash the process (Requirement 6.5).
                emit_server_error_metric("grpc.server.error", rpc_type, grpc::StatusCode::INTERNAL);
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, e.what()));
            }
        });

        return reactor;
    }

    // ── metrics helpers ───────────────────────────────────────────────────────

    auto emit_server_metric(std::string_view name, std::string_view rpc_type) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_one();
        metric.emit();
    }

    auto emit_server_size_metric(std::string_view name, std::string_view rpc_type, double size)
        -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_value(size);
        metric.emit();
    }

    auto emit_server_latency_metric(std::string_view name, std::string_view rpc_type,
                                    std::string_view status, std::chrono::nanoseconds latency)
        -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("status", status);
        metric.add_duration(latency);
        metric.emit();
    }

    auto emit_server_error_metric(std::string_view name, std::string_view rpc_type,
                                  grpc::StatusCode code) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("rpc_type", rpc_type);
        metric.add_dimension("status_code", std::to_string(static_cast<int>(code)));
        metric.add_one();
        metric.emit();
    }

    auto emit_lifecycle_metric(std::string_view name) -> void {
        auto metric = _metrics;
        metric.set_metric_name(name);
        metric.add_dimension("bind_address", _bind_address);
        metric.add_one();
        metric.emit();
    }

    std::function<request_vote_response<>(const request_vote_request<>&)> _request_vote_handler;
    std::function<append_entries_response<>(const append_entries_request<>&)>
        _append_entries_handler;
    std::function<install_snapshot_response<>(const install_snapshot_request<>&)>
        _install_snapshot_handler;
    std::function<request_pre_vote_response<>(const request_pre_vote_request<>&)>
        _request_pre_vote_handler;
    std::function<timeout_now_response<>(const timeout_now_request<>&)> _timeout_now_handler;
    std::function<cluster_join_response<>(const cluster_join_request<>&)> _cluster_join_handler;
    std::function<cluster_leave_response<>(const cluster_leave_request<>&)> _cluster_leave_handler;
    std::function<fetch_log_entries_response<>(const fetch_log_entries_request<>&)>
        _fetch_log_entries_handler;

    std::string _bind_address;
    std::uint16_t _bind_port;
    // Resolved by start(); atomic because bound_port() is callable without the
    // mutex, in the same spirit as _running.
    std::atomic<std::uint16_t> _bound_port{0};
    grpc_server_config _config;
    metrics_type _metrics;
    executor_type& _executor;
    std::shared_ptr<grpc::ServerCredentials> _server_credentials;
    std::unique_ptr<grpc::Server> _server;
    std::atomic<bool> _running{false};
    mutable std::mutex _mutex;
    // Null when TLS is off; reset first in the destructor because its
    // callbacks use _metrics.
    std::unique_ptr<grpc_detail::grpc_tls_reloader> _tls;
};

// ── concept conformance (Requirements 1.1, 1.2, 15-17; Tasks 4.6/5.7/7.3/8.4/9.4) ──

static_assert(kythira::network_client<grpc_client<grpc_kythira_transport_types>>,
              "grpc_client must satisfy network_client");
static_assert(kythira::network_client_with_pre_vote<grpc_client<grpc_kythira_transport_types>>,
              "grpc_client must satisfy network_client_with_pre_vote");
static_assert(kythira::network_client_with_cluster_join<grpc_client<grpc_kythira_transport_types>>,
              "grpc_client must satisfy network_client_with_cluster_join");
static_assert(kythira::network_client_with_cluster_leave<grpc_client<grpc_kythira_transport_types>>,
              "grpc_client must satisfy network_client_with_cluster_leave");
static_assert(kythira::network_client_with_log_fetch<grpc_client<grpc_kythira_transport_types>>,
              "grpc_client must satisfy network_client_with_log_fetch");

static_assert(kythira::network_server<grpc_server<grpc_kythira_transport_types>>,
              "grpc_server must satisfy network_server");
static_assert(kythira::network_server_with_pre_vote<grpc_server<grpc_kythira_transport_types>>,
              "grpc_server must satisfy network_server_with_pre_vote");

// The optional leadership-transfer extension (Ongaro's dissertation §3.10).
static_assert(kythira::network_client_with_timeout_now<grpc_client<grpc_kythira_transport_types>>,
              "grpc_client must satisfy network_client_with_timeout_now");
static_assert(kythira::network_server_with_timeout_now<grpc_server<grpc_kythira_transport_types>>,
              "grpc_server must satisfy network_server_with_timeout_now");
static_assert(kythira::network_server_with_cluster_join<grpc_server<grpc_kythira_transport_types>>,
              "grpc_server must satisfy network_server_with_cluster_join");
static_assert(kythira::network_server_with_cluster_leave<grpc_server<grpc_kythira_transport_types>>,
              "grpc_server must satisfy network_server_with_cluster_leave");
static_assert(kythira::network_server_with_log_fetch<grpc_server<grpc_kythira_transport_types>>,
              "grpc_server must satisfy network_server_with_log_fetch");

}  // namespace kythira
