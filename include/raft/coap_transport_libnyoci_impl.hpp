#pragma once
//
// SKELETON alternate CoAP backend: libnyoci (darconeous/libnyoci).
// ---------------------------------------------------------------------------
// This header sketches how a libnyoci-backed transport would satisfy the SAME
// kythira::network_client / kythira::network_server concepts (include/raft/
// network.hpp) that the libcoap-backed coap_client/coap_server in
// include/raft/coap_transport.hpp already satisfy. It is a design skeleton for
// comparison against coap_transport_cantcoap_impl.hpp — bodies are TODO.
//
// libnyoci is a FULL CoAP stack: it owns the UDP socket, the message/token
// layer, retransmission of confirmable messages, async responses and
// block-wise transfer. So this adapter's job is mostly *bridging*, not
// *building*: translate kythira RPCs into libnyoci transactions and route
// libnyoci's async callbacks back into the future_template<...> promises. That
// is the key contrast with cantcoap, where the adapter must implement all of
// the above by hand.
//
// Gating mirrors LIBCOAP_AVAILABLE exactly: this file compiles to a stub unless
// LIBNYOCI_AVAILABLE is defined by the build (see doc/coap_library_alternatives.md
// for the root CMakeLists.txt probe that sets it).
//
#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/coap_transport.hpp>  // reuse pending_message, block_transfer_state, configs
#include <raft/coap_security.hpp>
#include <raft/future_default.hpp>
#include <concepts/future.hpp>

#include <string>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <stop_token>

#ifdef LIBNYOCI_AVAILABLE
// libnyoci is plain C; include under extern "C" if its headers are not already
// guarded. Exact header path to be confirmed once the overlay port is pinned.
extern "C" {
#include <libnyoci/libnyoci.h>
}
#endif

namespace kythira {

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
// Templated on Types alone and constrained by transport_types<Types>, exactly
// like the libcoap coap_client — so callers get the same folly::Future /
// SimpleFuture backend and the same metrics_type regardless of CoAP library.
template<typename Types>
requires kythira::transport_types<Types>
class coap_libnyoci_client {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    using metrics_type = typename Types::metrics_type;

    coap_libnyoci_client(std::unordered_map<std::uint64_t, std::string> endpoints,
                         coap_client_config config, metrics_type metrics);
    ~coap_libnyoci_client();

    // --- network_client concept surface (must match network.hpp exactly) ---
    auto send_request_vote(std::uint64_t target, const request_vote_request<>& req,
                           std::chrono::milliseconds timeout)
        -> future_template<request_vote_response<>>;
    auto send_append_entries(std::uint64_t target, const append_entries_request<>& req,
                             std::chrono::milliseconds timeout)
        -> future_template<append_entries_response<>>;
    auto send_install_snapshot(std::uint64_t target, const install_snapshot_request<>& req,
                               std::chrono::milliseconds timeout)
        -> future_template<install_snapshot_response<>>;

private:
    // Generic RPC path: serialize -> POST to the target's resource -> resolve
    // the promise from libnyoci's async transaction callback.
    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, const std::string& resource_path, const Request& request,
                  std::chrono::milliseconds timeout) -> future_template<Response>;

#ifdef LIBNYOCI_AVAILABLE
    // TODO: one nyoci_t interface per client, driven on a background thread that
    // calls nyoci_process()/nyoci_plat_update_fdset() in a loop until stop.
    //   nyoci_t interface_{nullptr};
    //
    // TODO: libnyoci transactions are started with nyoci_transaction_begin() and
    // deliver their result through a C callback (nyoci_response_handler_func).
    // Bridge pattern: heap-allocate a small context holding the promise's
    // resolve/reject callbacks (reuse pending_message from coap_transport.hpp),
    // pass it as the callback's void* context, and fulfil the promise there.
    // static nyoci_status_t on_response(int statuscode, void* context);
    //
    // TODO: DTLS/OSCORE — libnyoci layers security via an ssl plugin, but
    // kythira already owns OSCORE/EDHOC through coap_security/coap_edhoc. Decide
    // whether to drive libnyoci's plugin or keep the existing security layer
    // above a plain-CoAP libnyoci core (recommended: reuse coap_security so the
    // OSCORE/EDHOC story does not fork per backend).
#endif

    std::unordered_map<std::uint64_t, std::string> endpoints_;
    coap_client_config config_;
    metrics_type metrics_;
    std::jthread event_thread_;
    std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
template<typename Types>
requires kythira::transport_types<Types>
class coap_libnyoci_server {
public:
    using metrics_type = typename Types::metrics_type;

    coap_libnyoci_server(std::string bind_address, std::uint16_t port, coap_server_config config,
                         metrics_type metrics);
    ~coap_libnyoci_server();

    // --- network_server concept surface ---
    void register_request_vote_handler(
        std::function<request_vote_response<>(const request_vote_request<>&)> h);
    void register_append_entries_handler(
        std::function<append_entries_response<>(const append_entries_request<>&)> h);
    void register_install_snapshot_handler(
        std::function<install_snapshot_response<>(const install_snapshot_request<>&)> h);

    void start();
    void stop();
    bool is_running() const { return running_.load(); }

private:
#ifdef LIBNYOCI_AVAILABLE
    // TODO: register one nyoci node/resource per RPC path via the libnyoci
    // request handler (nyoci_inbound_*), deserialize the payload, invoke the
    // stored std::function handler, and emit the response with
    // nyoci_outbound_begin_response()/nyoci_outbound_send(). libnyoci runs the
    // handler on its own process loop, so guard handler storage with mutex_.
#endif
    std::function<request_vote_response<>(const request_vote_request<>&)> rv_handler_;
    std::function<append_entries_response<>(const append_entries_request<>&)> ae_handler_;
    std::function<install_snapshot_response<>(const install_snapshot_request<>&)> is_handler_;

    std::string bind_address_;
    std::uint16_t port_;
    coap_server_config config_;
    metrics_type metrics_;
    std::jthread event_thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
};

// Concept conformance is the whole point of the skeleton — uncomment once the
// method bodies exist (std_coap_transport_types is the concrete Types bundle
// the libcoap tests use):
// static_assert(kythira::network_client<coap_libnyoci_client<std_coap_transport_types>>);
// static_assert(kythira::network_server<coap_libnyoci_server<std_coap_transport_types>>);

}  // namespace kythira
