#pragma once
//
// SKELETON alternate CoAP backend: cantcoap (staropram/cantcoap).
// ---------------------------------------------------------------------------
// This header sketches a cantcoap-backed transport satisfying the SAME
// kythira::network_client / kythira::network_server concepts (include/raft/
// network.hpp) as the libcoap-backed coap_client/coap_server. It is a design
// skeleton for comparison against coap_transport_libnyoci_impl.hpp.
//
// THE CENTRAL DIFFERENCE: cantcoap is ONLY a PDU codec (CoapPDU: build/parse a
// single CoAP message). It provides NO socket, NO retransmission, NO duplicate
// detection, NO block-wise reassembly and NO DTLS. This adapter must therefore
// implement the entire transport around the codec — which is why it is far
// larger than the libnyoci sketch. The upside: kythira keeps full control of
// the wire behaviour and reuses machinery it already has:
//   - retransmission / dedup    -> pending_message, received_message_info
//                                  (already defined in coap_transport.hpp)
//   - block-wise transfer       -> block_transfer_state + coap_block_option.hpp
//   - DTLS / OSCORE / EDHOC      -> coap_security.hpp / coap_edhoc.hpp
// so the "own the stack" cost is mostly the UDP socket + timer loop, not the
// security or block-wise logic.
//
// Gating mirrors LIBCOAP_AVAILABLE: stub unless CANTCOAP_AVAILABLE is defined.
//
#include <raft/types.hpp>
#include <raft/network.hpp>
#include <raft/coap_transport.hpp>  // pending_message, received_message_info, block_transfer_state, configs
#include <raft/coap_block_option.hpp>
#include <raft/coap_security.hpp>
#include <raft/future_default.hpp>
#include <concepts/future.hpp>

#include <string>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <stop_token>

#ifdef CANTCOAP_AVAILABLE
#include <cantcoap.h>  // CoapPDU
#endif

namespace kythira {

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
template<typename Types>
requires kythira::transport_types<Types>
class coap_cantcoap_client {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    using metrics_type = typename Types::metrics_type;

    coap_cantcoap_client(std::unordered_map<std::uint64_t, std::string> endpoints,
                         coap_client_config config, metrics_type metrics);
    ~coap_cantcoap_client();

    // --- network_client concept surface ---
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
    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, const std::string& resource_path, const Request& request,
                  std::chrono::milliseconds timeout) -> future_template<Response>;

#ifdef CANTCOAP_AVAILABLE
    // TODO: build the request PDU with cantcoap. Roughly:
    //   CoapPDU pdu;
    //   pdu.setVersion(1);
    //   pdu.setType(config_.confirmable ? CoapPDU::COAP_CONFIRMABLE
    //                                   : CoapPDU::COAP_NON_CONFIRMABLE);
    //   pdu.setCode(CoapPDU::COAP_POST);
    //   pdu.setToken(token.data(), token.size());
    //   pdu.setMessageID(next_message_id());
    //   pdu.setURI(const_cast<char*>(resource_path.c_str()), resource_path.size());
    //   pdu.setPayload(payload.data(), payload.size());
    //   ::sendto(fd_, pdu.getPDUPointer(), pdu.getPDULength(), 0, ...);
    // cantcoap does the CoAP encoding; EVERYTHING below is on us:

    // TODO: UDP socket lifecycle (one bound fd per client) — cantcoap has none.
    int fd_{-1};

    // TODO: RX + timer loop on event_thread_:
    //   * recvfrom() -> CoapPDU pdu(buf, n); pdu.validate();
    //   * match pdu.getTokenPointer()/getToken to a pending_message and resolve
    //     its promise (reuse pending_message from coap_transport.hpp).
    //   * duplicate suppression via received_message_info keyed on message id.
    //   * retransmit confirmable pending_messages whose timeout elapsed, with
    //     CoAP's exponential backoff (ACK_TIMEOUT * ACK_RANDOM_FACTOR ...),
    //     giving up after MAX_RETRANSMIT and rejecting the promise.
    std::unordered_map<std::string, std::shared_ptr<pending_message>> pending_;
    std::unordered_map<std::uint16_t, received_message_info> seen_;

    // TODO: block-wise transfer. cantcoap can read/write Block1/Block2 option
    // bytes, but the reassembly state machine is ours — drive block_transfer_state
    // with the Block option helpers in coap_block_option.hpp.
    std::unordered_map<std::string, block_transfer_state> block_transfers_;

    // TODO: DTLS/OSCORE. cantcoap is cleartext-only. Wrap the socket in the
    // existing coap_security provider (coap_security.hpp) — feed decrypted
    // datagrams into CoapPDU on RX and encrypt CoapPDU::getPDUPointer() on TX —
    // and keep OSCORE/EDHOC in coap_edhoc.hpp exactly as the libcoap path does.
    std::unique_ptr<coap_security_provider> security_;

    std::uint16_t next_message_id();
    std::atomic<std::uint16_t> message_id_counter_{0};
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
class coap_cantcoap_server {
public:
    using metrics_type = typename Types::metrics_type;

    coap_cantcoap_server(std::string bind_address, std::uint16_t port, coap_server_config config,
                         metrics_type metrics);
    ~coap_cantcoap_server();

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
#ifdef CANTCOAP_AVAILABLE
    // TODO: bound UDP fd + RX loop. For each datagram:
    //   CoapPDU in(buf, n);
    //   if (in.validate() != 1) continue;                    // malformed -> drop
    //   route on in.getURI(...) to the matching handler;
    //   deserialize payload, invoke the std::function, serialize the response;
    //   build an ACK/response CoapPDU echoing in.getToken()/getMessageID();
    //   sendto() back to the peer's address from recvfrom().
    // Confirmable-message ACKs, dedup and block-wise responses are all manual
    // here too (same helpers as the client).
    int fd_{-1};
    std::unordered_map<std::uint16_t, received_message_info> seen_;
    std::unique_ptr<coap_security_provider> security_;
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

// static_assert(kythira::network_client<coap_cantcoap_client<std_coap_transport_types>>);
// static_assert(kythira::network_server<coap_cantcoap_server<std_coap_transport_types>>);

}  // namespace kythira
