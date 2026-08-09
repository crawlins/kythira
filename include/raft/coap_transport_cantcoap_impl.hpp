#pragma once
//
// Alternate CoAP backend: cantcoap (staropram/cantcoap).
// ---------------------------------------------------------------------------
// coap_cantcoap_client<Types> / coap_cantcoap_server<Types> are a *third*
// implementation of the same kythira::network_client / kythira::network_server
// concepts (include/raft/network.hpp) that the libcoap-backed
// coap_client/coap_server and the libnyoci-backed pair already satisfy. See
// .kiro/specs/coap-transport-cantcoap/ for the spec and
// doc/coap_library_alternatives.md for the three-way comparison.
//
// BUILD, DON'T BRIDGE
// -------------------
// This is the opposite end of the cost curve from the libnyoci backend.
// cantcoap is a CoAP *message codec* and nothing else: `CoapPDU` builds and
// parses one RFC 7252 message. There is no socket, no message layer, no
// retransmission, no duplicate detection, no block-wise state machine and no
// DTLS. All of that lives here.
//
// What keeps that honest is how much of it kythira already had. The socket and
// timer loop are genuinely new; everything above them is existing scaffolding
// wired together:
//
//   PDU encode/parse   cantcoap's CoapPDU
//   UDP socket + loop  new here (socket/recvfrom/sendto + a timer tick)
//   retransmit/dedup   pending_message, received_message_info
//   block-wise         block_option (coap_block_option.hpp) + our own sequencing
//   OSCORE             oscore::security_context (raft/oscore.hpp)
//
// So "own the stack" really means "own the socket and the timer, and wire up
// the pieces" -- not "reimplement CoAP".
//
// WHY THIS FILE DOES NOT INCLUDE raft/coap_transport.hpp
// ------------------------------------------------------
// The same reason the libnyoci backend cannot: libcoap spells the CoAP option
// numbers as object-like macros (`#define COAP_OPTION_IF_MATCH 1`) and cantcoap
// as enumerators (`COAP_OPTION_IF_MATCH=1,` inside class CoapPDU). Being
// class-scoped does not help -- macros substitute anywhere -- so including
// libcoap first rewrites the middle of cantcoap's enum into `1=1,`. Verified,
// not assumed. The shared, library-neutral declarations therefore come from
// raft/coap_transport_config.hpp.
//
// THREADING MODEL
// ---------------
// One UDP socket and one std::jthread per client and per server. The thread
// polls the socket with a bounded timeout, so the same loop that receives
// datagrams also drives retransmission and expiry -- no second timer thread and
// no lock ordering between them. Public methods only touch state under
// `_mutex`; everything else happens on the loop.
//
// Gating mirrors LIBCOAP_AVAILABLE: this file compiles to a stub unless
// CANTCOAP_AVAILABLE is defined, keeping its full concept surface either way so
// the conformance static_asserts hold in a build without cantcoap.
//
#include <raft/types.hpp>
#include <raft/network.hpp>
// coap_client_config / coap_server_config / pending_message /
// received_message_info / translate_legacy_fields(). NOT
// raft/coap_transport.hpp -- see above.
#include <raft/coap_transport_config.hpp>
#include <raft/coap_block_option.hpp>
#include <raft/coap_exceptions.hpp>
#include <raft/coap_security.hpp>
#include <raft/coap_utils.hpp>
#include <raft/oscore.hpp>
#include <raft/peer_capability_cache.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/future_default.hpp>
#include <concepts/future.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// The POSIX socket headers are included unconditionally, unlike <cantcoap.h>.
// This adapter supplies its own UDP socket, so `sockaddr_in6` and friends name
// the transport's own state rather than anything cantcoap declares — and
// `pending_exchange` below stores a `sockaddr_in6` whether or not the codec was
// found. Guarding these caused a build that has no cantcoap to compile only by
// accident, via a transitive include from folly; the stdexec future backend
// pulls in no folly and failed outright.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef CANTCOAP_AVAILABLE
#include <cantcoap.h>
#endif

namespace kythira {

// The CoAP resource each RPC is POSTed to. Identical to the paths the libcoap
// and libnyoci backends use, so all three interoperate on the wire.
inline constexpr const char* cantcoap_request_vote_path = "/raft/request_vote";
inline constexpr const char* cantcoap_append_entries_path = "/raft/append_entries";
inline constexpr const char* cantcoap_install_snapshot_path = "/raft/install_snapshot";

/// How long the loop blocks in poll() before servicing timers. Bounds both
/// retransmission granularity and how quickly stop() is noticed.
inline constexpr int cantcoap_poll_interval_ms = 20;

/// Largest datagram this backend will read. CoAP over UDP is expected to fit a
/// single unfragmented datagram; anything larger is what block-wise is for.
inline constexpr std::size_t cantcoap_max_datagram = 1500;

namespace cantcoap_detail {

/// Decide what to do about `config`'s security (Requirement 6).
///
/// The same finding as the libnyoci backend, reached independently:
/// `coap_security_provider`'s whole interface is expressed in libcoap types, so
/// it cannot be reused from a backend that cannot include a libcoap header.
///
/// What *is* reusable is `raft/oscore.hpp` -- object security implemented
/// against CoAP message bytes -- which is exactly the shape this backend needs,
/// since it already owns the bytes on both sides of the socket. That was
/// written for the libnyoci backend and is inherited here for free, which is
/// the payoff of having made it transport-neutral.
///
/// DTLS is refused. cantcoap is cleartext-only and, unlike libnyoci, there is
/// no plugin to fall back on: providing it would mean driving an OpenSSL DTLS
/// BIO over this backend's own socket, including the handshake, retransmission
/// and cookie exchange. That is a transport in its own right and is out of
/// scope for this spec; refusing beats a half-implementation that looks
/// encrypted.
enum class channel {
    plain,
    oscore
};

template<typename Config>
[[nodiscard]] inline auto plan_security(const Config& config, const char* role)
    -> std::pair<channel, coap_security_config> {
    coap_security_config effective = kythira::translate_legacy_fields(config);
    switch (effective.mode) {
        case coap_auth_mode::none:
            return {channel::plain, std::move(effective)};
        case coap_auth_mode::oscore:
            if (std::holds_alternative<oscore_credentials>(effective.credentials) &&
                std::get<oscore_credentials>(effective.credentials).bootstrap_method ==
                    oscore_bootstrap::edhoc) {
                throw coap_security_error(
                    std::string("the cantcoap CoAP backend does not implement the EDHOC "
                                "bootstrap for this ") +
                    role +
                    " yet; supply static OSCORE credentials, or use the libnyoci backend, which "
                    "serves /.well-known/edhoc.");
            }
            return {channel::oscore, std::move(effective)};
        case coap_auth_mode::dtls_psk:
        case coap_auth_mode::dtls_pki:
        case coap_auth_mode::dtls_rpk:
            throw coap_security_error(
                std::string("the cantcoap CoAP backend cannot provide DTLS for this ") + role +
                ". cantcoap is a message codec with no transport, and unlike libnyoci there is no "
                "DTLS plugin to drive -- supplying it would mean implementing the DTLS handshake "
                "and record layer over this backend's own socket. Use OSCORE here (object "
                "security, which this backend does provide), or the libcoap or libnyoci backend "
                "for DTLS. See .kiro/specs/coap-transport-cantcoap/ Requirement 6.");
    }
    throw coap_security_config_error("unknown coap_auth_mode");
}

#ifdef CANTCOAP_AVAILABLE

/// An owned UDP socket. Exists so the destructor cannot forget the close(),
/// which matters here because this backend opens the socket itself rather than
/// handing that job to a library.
class udp_socket {
public:
    udp_socket() = default;
    udp_socket(const udp_socket&) = delete;
    auto operator=(const udp_socket&) -> udp_socket& = delete;
    udp_socket(udp_socket&& other) noexcept : _fd(other._fd) { other._fd = -1; }
    auto operator=(udp_socket&& other) noexcept -> udp_socket& {
        if (this != &other) {
            close();
            _fd = other._fd;
            other._fd = -1;
        }
        return *this;
    }
    ~udp_socket() { close(); }

    /// Binds an AF_INET6 socket with IPV6_V6ONLY off, so one socket serves both
    /// families and a v4 peer arrives as a v4-mapped address.
    auto open(std::uint16_t port) -> void {
        _fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (_fd < 0) {
            throw coap_network_error("failed to create a UDP socket for the cantcoap backend");
        }
        int off = 0;
        ::setsockopt(_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        int reuse = 1;
        ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);
        if (::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            throw coap_network_error("failed to bind a UDP socket to port " + std::to_string(port));
        }
    }

    [[nodiscard]] auto bound_port() const -> std::uint16_t {
        sockaddr_in6 addr{};
        socklen_t length = sizeof(addr);
        if (_fd < 0 || ::getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
            return 0;
        }
        return ntohs(addr.sin6_port);
    }

    [[nodiscard]] auto fd() const -> int { return _fd; }
    [[nodiscard]] auto valid() const -> bool { return _fd >= 0; }

    auto close() -> void {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

private:
    int _fd{-1};
};

/// Resolve "coap://host:port" (or "host:port") to a sockaddr this backend can
/// sendto(). v4 results are mapped into the v6 socket's address family.
[[nodiscard]] inline auto resolve_endpoint(const std::string& endpoint) -> sockaddr_in6 {
    std::string rest = endpoint;
    for (const auto* scheme : {"coaps://", "coap://"}) {
        if (rest.rfind(scheme, 0) == 0) {
            rest.erase(0, std::strlen(scheme));
            break;
        }
    }
    while (!rest.empty() && rest.back() == '/') {
        rest.pop_back();
    }

    std::string host = rest;
    std::string port = "5683";
    if (!rest.empty() && rest.front() == '[') {
        // Bracketed IPv6 literal: [::1]:5683
        const auto end = rest.find(']');
        if (end == std::string::npos) {
            throw coap_network_error("malformed IPv6 endpoint: " + endpoint);
        }
        host = rest.substr(1, end - 1);
        if (end + 1 < rest.size() && rest[end + 1] == ':') {
            port = rest.substr(end + 2);
        }
    } else if (const auto colon = rest.rfind(':');
               colon != std::string::npos && rest.find(':') == colon) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    }

    addrinfo hints{};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    // V4MAPPED|ALL so a v4-only host still resolves into the v6 socket's family.
    hints.ai_flags = AI_V4MAPPED | AI_ALL;

    addrinfo* results = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
        throw coap_network_error("failed to resolve CoAP endpoint: " + endpoint);
    }
    sockaddr_in6 out{};
    std::memcpy(&out, results->ai_addr, std::min<std::size_t>(sizeof(out), results->ai_addrlen));
    ::freeaddrinfo(results);
    return out;
}

/// Add one CoAP option holding a minimum-length big-endian unsigned integer,
/// which is how RFC 7252 encodes Content-Format, Accept and the block options.
inline auto add_uint_option(CoapPDU& pdu, std::uint16_t number, std::uint32_t value) -> void {
    std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(value & 0xFFU),
    };
    std::size_t first = 0;
    while (first < 4 && bytes[first] == 0) {
        ++first;
    }
    pdu.addOption(number, static_cast<std::uint16_t>(4 - first), bytes.data() + first);
}

[[nodiscard]] inline auto read_uint_option(const CoapPDU::CoapOption& option) -> std::uint32_t {
    std::uint32_t value = 0;
    for (std::uint16_t i = 0; i < option.optionValueLength; ++i) {
        value = (value << 8U) | static_cast<std::uint32_t>(option.optionValuePointer[i]);
    }
    return value;
}

/// Everything this backend needs out of an inbound PDU, read once so the rest
/// of the code never touches cantcoap's raw option list.
struct parsed_options {
    std::optional<std::uint16_t> content_format;
    std::vector<std::uint16_t> accepted_formats;
    std::optional<block_option> block1;
    std::optional<block_option> block2;
};

[[nodiscard]] inline auto scan_options(CoapPDU& pdu) -> parsed_options {
    parsed_options out;
    CoapPDU::CoapOption* options = pdu.getOptions();
    if (options == nullptr) {
        return out;
    }
    const int count = pdu.getNumOptions();
    for (int i = 0; i < count; ++i) {
        const auto& option = options[i];
        switch (option.optionNumber) {
            case CoapPDU::COAP_OPTION_CONTENT_FORMAT:
                out.content_format = static_cast<std::uint16_t>(read_uint_option(option));
                break;
            case CoapPDU::COAP_OPTION_ACCEPT:
                out.accepted_formats.push_back(
                    static_cast<std::uint16_t>(read_uint_option(option)));
                break;
            case CoapPDU::COAP_OPTION_BLOCK1:
                out.block1 = block_option::parse(read_uint_option(option));
                break;
            case CoapPDU::COAP_OPTION_BLOCK2:
                out.block2 = block_option::parse(read_uint_option(option));
                break;
            default:
                break;
        }
    }
    // getOptions() hands back a malloc'd array that the caller owns.
    std::free(options);
    return out;
}

/// Add the Uri-Path options for "/raft/append_entries" one segment at a time.
///
/// Not CoapPDU::setURI(): that also emits Uri-Host and Uri-Port, which this
/// backend has no use for (the peer address is already known from the socket)
/// and which would only add bytes to every datagram.
inline auto add_uri_path(CoapPDU& pdu, const std::string& path) -> void {
    std::size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') {
            ++start;
            continue;
        }
        const auto end = path.find('/', start);
        const auto segment =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        pdu.addOption(CoapPDU::COAP_OPTION_URI_PATH, static_cast<std::uint16_t>(segment.size()),
                      reinterpret_cast<std::uint8_t*>(const_cast<char*>(segment.data())));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

/// Reassemble "/a/b" from an inbound PDU's Uri-Path options.
[[nodiscard]] inline auto read_uri_path(CoapPDU& pdu) -> std::string {
    std::string path;
    CoapPDU::CoapOption* options = pdu.getOptions();
    if (options == nullptr) {
        return path;
    }
    const int count = pdu.getNumOptions();
    for (int i = 0; i < count; ++i) {
        if (options[i].optionNumber == CoapPDU::COAP_OPTION_URI_PATH) {
            path.push_back('/');
            path.append(reinterpret_cast<const char*>(options[i].optionValuePointer),
                        options[i].optionValueLength);
        }
    }
    std::free(options);
    return path;
}

#endif  // CANTCOAP_AVAILABLE

}  // namespace cantcoap_detail

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
template<typename Types>
requires kythira::transport_types<Types>
class coap_cantcoap_client {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    template<typename T> using promise_template = typename Types::template promise_template<T>;
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;

    coap_cantcoap_client(std::unordered_map<std::uint64_t, std::string> node_id_to_endpoint_map,
                         kythira::coap_client_config config, metrics_type metrics)
        : _registry{},
          _node_id_to_endpoint{std::move(node_id_to_endpoint_map)},
          _config{std::move(config)},
          _metrics{std::move(metrics)} {
        kythira::coap_utils::validate_registry_content_formats(_registry);
        const auto [selected, security] = cantcoap_detail::plan_security(_config, "client");
        if (selected == cantcoap_detail::channel::oscore) {
            _oscore = std::make_shared<oscore::security_context>(
                std::get<oscore_credentials>(security.credentials));
        }
#ifdef CANTCOAP_AVAILABLE
        // Port 0: the client only originates requests, so any source port will
        // do. Opening here rather than lazily means a bind failure surfaces at
        // construction instead of on the first RPC.
        _socket.open(0);
        _thread = std::jthread([this](std::stop_token stop) { run_loop(stop); });
#endif
    }

    ~coap_cantcoap_client() {
#ifdef CANTCOAP_AVAILABLE
        if (_thread.joinable()) {
            _thread.request_stop();
            _thread.join();
        }
        _socket.close();
#endif
        // Whatever the loop did not reject on its way out, reject here: no
        // future may outlive the client unresolved (Requirement 7.3).
        reject_all("cantcoap CoAP client destroyed with the request in flight");
    }

    coap_cantcoap_client(const coap_cantcoap_client&) = delete;
    auto operator=(const coap_cantcoap_client&) -> coap_cantcoap_client& = delete;
    coap_cantcoap_client(coap_cantcoap_client&&) = delete;
    auto operator=(coap_cantcoap_client&&) -> coap_cantcoap_client& = delete;

    // --- network_client concept surface ---

    auto send_request_vote(std::uint64_t target, const kythira::request_vote_request<>& request,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        -> future_template<kythira::request_vote_response<>> {
        return send_rpc<kythira::request_vote_request<>, kythira::request_vote_response<>>(
            target, cantcoap_request_vote_path, request, timeout);
    }

    auto send_append_entries(std::uint64_t target, const kythira::append_entries_request<>& request,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{5000})
        -> future_template<kythira::append_entries_response<>> {
        return send_rpc<kythira::append_entries_request<>, kythira::append_entries_response<>>(
            target, cantcoap_append_entries_path, request, timeout);
    }

    auto send_install_snapshot(std::uint64_t target,
                               const kythira::install_snapshot_request<>& request,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds{30000})
        -> future_template<kythira::install_snapshot_response<>> {
        return send_rpc<kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
            target, cantcoap_install_snapshot_path, request, timeout);
    }

    [[nodiscard]] auto bound_port() const -> std::uint16_t {
#ifdef CANTCOAP_AVAILABLE
        return _socket.bound_port();
#else
        return 0;
#endif
    }

    [[nodiscard]] static constexpr auto backend_available() -> bool {
#ifdef CANTCOAP_AVAILABLE
        return true;
#else
        return false;
#endif
    }

private:
    /// One in-flight exchange. Holds the reused `pending_message` (Requirement
    /// 4.1) plus the transport state cantcoap cannot supply: the exact datagram
    /// to retransmit, the peer address, the retransmission deadline, and the
    /// block-wise cursor.
    struct pending_exchange {
        std::unique_ptr<pending_message> message;
        std::vector<std::byte> datagram;  //!< exactly what was sent, resent verbatim
        sockaddr_in6 peer{};
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds backoff{0};
        std::size_t retransmissions{0};
        std::chrono::steady_clock::time_point expiry;

        // Block-wise cursors and the reassembly buffer.
        std::string resource_path;
        std::string request_media_type;
        std::vector<std::uint16_t> accept_formats;
        std::vector<std::byte> full_request;
        std::uint32_t request_block{0};
        std::uint32_t response_block{0};
        std::uint32_t block_size{0};
        std::vector<std::byte> accumulated;
        std::optional<std::uint16_t> response_format;
        bool settled{false};
        /// What verifying this exchange's response needs, when OSCORE is on.
        oscore::request_binding binding;
    };

    template<typename Request, typename Response>
    auto send_rpc(std::uint64_t target, const std::string& resource_path, const Request& request,
                  std::chrono::milliseconds timeout) -> future_template<Response> {
        auto promise = std::make_shared<promise_template<Response>>();
        auto future = promise->getFuture();

#ifdef CANTCOAP_AVAILABLE
        try {
            const auto endpoint = _node_id_to_endpoint.find(target);
            if (endpoint == _node_id_to_endpoint.end()) {
                throw coap_network_error("no CoAP endpoint configured for node " +
                                         std::to_string(target));
            }
            const std::string media_type =
                kythira::select_request_media_type(_registry, _capability_cache, target);
            const auto format = kythira::coap_utils::media_type_to_coap_content_format(media_type);
            if (!format) {
                throw coap_unsupported_content_format_error(media_type);
            }

            auto exchange = std::make_unique<pending_exchange>();
            exchange->peer = cantcoap_detail::resolve_endpoint(endpoint->second);
            exchange->resource_path = resource_path;
            exchange->request_media_type = media_type;
            exchange->full_request = _registry.encode_with(media_type, request);
            exchange->block_size = static_cast<std::uint32_t>(
                _config.enable_block_transfer &&
                        kythira::coap_utils::is_valid_block_size(_config.max_block_size)
                    ? _config.max_block_size
                    : 1024);
            for (const auto& accepted : _registry.preferred_media_types()) {
                if (const auto accepted_format =
                        kythira::coap_utils::media_type_to_coap_content_format(accepted)) {
                    exchange->accept_formats.push_back(
                        static_cast<std::uint16_t>(*accepted_format));
                }
            }

            const auto token = next_token();
            exchange->message = std::make_unique<pending_message>(
                token, 0, timeout,
                [promise, this, target](std::vector<std::byte> body,
                                        const std::string& response_media_type) {
                    try {
                        Response response =
                            _registry.template decode_with<Response>(response_media_type, body);
                        _capability_cache.record(target, response_media_type);
                        promise->setValue(std::move(response));
                    } catch (const std::exception& e) {
                        promise->setException(std::make_exception_ptr(coap_transport_error(
                            "failed to deserialize CoAP response: " + std::string(e.what()))));
                    }
                },
                [promise](std::exception_ptr error) { promise->setException(error); },
                exchange->full_request, endpoint->second, resource_path,
                _config.use_confirmable_messages);
            exchange->expiry = std::chrono::steady_clock::now() + timeout;

            {
                const std::lock_guard lock(_mutex);
                if (_shutting_down) {
                    throw coap_transport_error("cantcoap CoAP client is shutting down");
                }
                _pending.emplace(token, std::move(exchange));
            }
            // Building and sending happen on the loop thread so that every
            // socket write, and every mutation of the pending map, has a single
            // owner. Requirement 7.4 without a second lock.
            {
                const std::lock_guard lock(_mutex);
                _to_start.push_back(token);
            }

            auto request_metric = _metrics;
            request_metric.set_metric_name("coap.client.request.sent");
            request_metric.add_dimension("resource_path", resource_path);
            request_metric.add_dimension("target_node_id", std::to_string(target));
            request_metric.add_dimension("media_type", media_type);
            request_metric.add_one();
            request_metric.emit();
        } catch (...) {
            promise->setException(std::current_exception());
        }
#else
        (void)target;
        (void)resource_path;
        (void)request;
        (void)timeout;
        promise->setException(std::make_exception_ptr(coap_transport_error(
            "cantcoap CoAP backend unavailable: rebuild with the vcpkg 'coap-cantcoap' feature so "
            "CANTCOAP_AVAILABLE is defined")));
#endif
        return std::move(future);
    }

    [[nodiscard]] auto next_token() -> std::string {
        const auto value = _token_counter.fetch_add(1);
        std::string token(8, '\0');
        for (int i = 0; i < 8; ++i) {
            token[static_cast<std::size_t>(i)] =
                static_cast<char>((value >> (8 * (7 - i))) & 0xFFU);
        }
        return token;
    }

    auto reject_all(const std::string& reason) -> void {
        std::unordered_map<std::string, std::unique_ptr<pending_exchange>> pending;
        {
            const std::lock_guard lock(_mutex);
            _shutting_down = true;
            pending.swap(_pending);
        }
        for (auto& [token, exchange] : pending) {
            if (exchange && !exchange->settled && exchange->message) {
                exchange->settled = true;
                exchange->message->reject_callback(
                    std::make_exception_ptr(coap_transport_error(reason)));
            }
        }
    }

#ifdef CANTCOAP_AVAILABLE
    // ---- the one thread that touches the socket ----

    auto run_loop(std::stop_token stop) -> void {
        std::vector<std::uint8_t> buffer(cantcoap_max_datagram);
        while (!stop.stop_requested()) {
            start_queued();

            pollfd fds{};
            fds.fd = _socket.fd();
            fds.events = POLLIN;
            const int ready = ::poll(&fds, 1, cantcoap_poll_interval_ms);
            if (ready > 0 && (fds.revents & POLLIN) != 0) {
                sockaddr_in6 from{};
                socklen_t from_length = sizeof(from);
                const auto received = ::recvfrom(_socket.fd(), buffer.data(), buffer.size(), 0,
                                                 reinterpret_cast<sockaddr*>(&from), &from_length);
                if (received > 0) {
                    handle_datagram(buffer.data(), static_cast<int>(received));
                }
            }
            service_timers();
        }
        reject_all("cantcoap CoAP client stopped with the request in flight");
    }

    auto start_queued() -> void {
        std::vector<std::string> tokens;
        {
            const std::lock_guard lock(_mutex);
            tokens.swap(_to_start);
        }
        for (const auto& token : tokens) {
            const std::lock_guard lock(_mutex);
            const auto it = _pending.find(token);
            if (it == _pending.end()) {
                continue;
            }
            transmit_locked(*it->second, token);
        }
    }

    /// Build the current block's PDU, remember it verbatim for retransmission,
    /// and send it. Called with `_mutex` held.
    auto transmit_locked(pending_exchange& exchange, const std::string& token) -> void {
        try {
            exchange.datagram = build_request(exchange, token);
        } catch (...) {
            if (!exchange.settled) {
                exchange.settled = true;
                exchange.message->reject_callback(std::current_exception());
            }
            return;
        }
        send_datagram(exchange.datagram, exchange.peer);
        exchange.retransmissions = 0;
        // RFC 7252 Section 4.2: the first retransmission waits ACK_TIMEOUT
        // scaled by a random factor in [1, ACK_RANDOM_FACTOR), and each
        // subsequent one doubles it.
        exchange.backoff = initial_backoff();
        exchange.deadline = std::chrono::steady_clock::now() + exchange.backoff;
    }

    [[nodiscard]] auto initial_backoff() -> std::chrono::milliseconds {
        const auto base = _config.ack_timeout.count() > 0 ? _config.ack_timeout.count() : 2000;
        const auto jitter = _config.ack_random_factor_ms.count();
        if (jitter <= 0) {
            return std::chrono::milliseconds{base};
        }
        std::uniform_int_distribution<long long> distribution(0, jitter);
        return std::chrono::milliseconds{base + distribution(_rng)};
    }

    [[nodiscard]] auto build_request(pending_exchange& exchange, const std::string& token)
        -> std::vector<std::byte> {
        CoapPDU pdu;
        pdu.setVersion(1);
        pdu.setType(exchange.message->is_confirmable ? CoapPDU::COAP_CONFIRMABLE
                                                     : CoapPDU::COAP_NON_CONFIRMABLE);
        pdu.setCode(CoapPDU::COAP_POST);
        pdu.setMessageID(next_message_id());
        exchange.message->message_id = pdu.getMessageID();
        pdu.setToken(reinterpret_cast<std::uint8_t*>(const_cast<char*>(token.data())),
                     static_cast<std::uint8_t>(token.size()));

        // Options must be added in ascending number order; cantcoap does not
        // sort them for us.
        cantcoap_detail::add_uri_path(pdu, exchange.resource_path);  // 11
        const auto format =
            *kythira::coap_utils::media_type_to_coap_content_format(exchange.request_media_type);
        cantcoap_detail::add_uint_option(pdu, CoapPDU::COAP_OPTION_CONTENT_FORMAT,
                                         static_cast<std::uint32_t>(format));  // 12
        for (const auto accepted : exchange.accept_formats) {
            cantcoap_detail::add_uint_option(pdu, CoapPDU::COAP_OPTION_ACCEPT, accepted);  // 17
        }
        if (exchange.response_block > 0) {
            block_option block;
            block.block_number = exchange.response_block;
            block.more_blocks = false;
            block.block_size = exchange.block_size;
            cantcoap_detail::add_uint_option(pdu, CoapPDU::COAP_OPTION_BLOCK2,
                                             block.encode());  // 23
        }

        // Block1: split the body when it exceeds one block (Requirement 5.1).
        const std::size_t offset =
            static_cast<std::size_t>(exchange.request_block) * exchange.block_size;
        const std::size_t remaining =
            offset < exchange.full_request.size() ? exchange.full_request.size() - offset : 0;
        const std::size_t chunk = std::min<std::size_t>(exchange.block_size, remaining);
        const bool more = (offset + chunk) < exchange.full_request.size();
        if (more || exchange.request_block > 0) {
            block_option block;
            block.block_number = exchange.request_block;
            block.more_blocks = more;
            block.block_size = exchange.block_size;
            cantcoap_detail::add_uint_option(pdu, CoapPDU::COAP_OPTION_BLOCK1,
                                             block.encode());  // 27
        }
        if (chunk > 0) {
            pdu.setPayload(reinterpret_cast<std::uint8_t*>(exchange.full_request.data() + offset),
                           static_cast<int>(chunk));
        }

        auto bytes = to_bytes(pdu);
        if (_oscore) {
            bytes = protect(bytes, exchange);
        }
        return bytes;
    }

    /// OSCORE-protect an outbound request. The PDU cantcoap just built *is* the
    /// inner message, so it only has to be re-read through the neutral codec --
    /// which is the whole reason raft/oscore.hpp works on message bytes.
    [[nodiscard]] auto protect(const std::vector<std::byte>& plain, pending_exchange& exchange)
        -> std::vector<std::byte> {
        const auto inner = oscore::parse_message(plain);
        const auto outer = _oscore->protect_request(inner, exchange.binding);
        return oscore::serialize_message(outer);
    }

    [[nodiscard]] static auto to_bytes(CoapPDU& pdu) -> std::vector<std::byte> {
        const auto* start = reinterpret_cast<const std::byte*>(pdu.getPDUPointer());
        return {start, start + pdu.getPDULength()};
    }

    auto send_datagram(const std::vector<std::byte>& bytes, const sockaddr_in6& peer) -> void {
        ::sendto(_socket.fd(), bytes.data(), bytes.size(), 0,
                 reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    }

    [[nodiscard]] auto next_message_id() -> std::uint16_t {
        return static_cast<std::uint16_t>(_message_id_counter.fetch_add(1));
    }

    auto handle_datagram(std::uint8_t* data, int length) -> void {
        std::vector<std::byte> bytes(reinterpret_cast<std::byte*>(data),
                                     reinterpret_cast<std::byte*>(data) + length);
        std::optional<oscore::request_binding> binding_for_response;

        // Parse first to recover the token, then verify: the binding needed to
        // verify is per-exchange, and the token is what identifies it.
        CoapPDU probe(data, length);
        if (probe.validate() != 1) {
            return;  // Malformed: drop, stay alive (Requirement 6.4).
        }
        std::string token(reinterpret_cast<const char*>(probe.getTokenPointer()),
                          static_cast<std::size_t>(probe.getTokenLength()));

        const std::lock_guard lock(_mutex);
        const auto it = _pending.find(token);
        if (it == _pending.end()) {
            return;  // No such exchange, or already settled: nothing to do.
        }
        auto& exchange = *it->second;

        std::vector<std::byte> plain = bytes;
        if (_oscore) {
            try {
                const auto outer = oscore::parse_message(bytes);
                const auto inner = _oscore->unprotect_response(outer, exchange.binding);
                plain = oscore::serialize_message(inner);
            } catch (const std::exception&) {
                return;  // Undecryptable: drop (Requirement 6.4).
            }
        }
        CoapPDU pdu(reinterpret_cast<std::uint8_t*>(plain.data()), static_cast<int>(plain.size()));
        if (pdu.validate() != 1) {
            return;
        }
        if (is_duplicate(pdu.getMessageID())) {
            return;  // Requirement 4.4.
        }
        handle_response_locked(exchange, token, pdu);
    }

    auto handle_response_locked(pending_exchange& exchange, const std::string& token, CoapPDU& pdu)
        -> void {
        const auto options = cantcoap_detail::scan_options(pdu);
        const auto code = pdu.getCode();

        // 2.31 Continue: the server took this request block and wants the next.
        if (static_cast<int>(code) == 0x5F && options.block1 && options.block1->more_blocks) {
            ++exchange.request_block;
            transmit_locked(exchange, token);
            return;
        }
        if (code >= CoapPDU::COAP_BAD_REQUEST) {
            settle_reject(exchange, exception_for_code(code, exchange.message->target_endpoint));
            _pending.erase(token);
            return;
        }

        if (!exchange.response_format && options.content_format) {
            exchange.response_format = options.content_format;
        }
        const auto* payload = pdu.getPayloadPointer();
        const auto payload_length = pdu.getPayloadLength();
        if (payload != nullptr && payload_length > 0) {
            const auto* start = reinterpret_cast<const std::byte*>(payload);
            exchange.accumulated.insert(exchange.accumulated.end(), start, start + payload_length);
        }

        // Block2: a response arriving in slices (Requirement 5.1).
        if (options.block2 && options.block2->more_blocks) {
            exchange.response_block = options.block2->block_number + 1;
            exchange.block_size = options.block2->block_size;
            // The request body is already delivered; the continuation only asks.
            exchange.full_request.clear();
            exchange.request_block = 0;
            transmit_locked(exchange, token);
            return;
        }

        std::string media_type = exchange.request_media_type;
        if (exchange.response_format) {
            const auto resolved = kythira::coap_utils::registry_media_type_for_content_format(
                _registry, kythira::coap_utils::parse_content_format(*exchange.response_format));
            if (!resolved) {
                settle_reject(
                    exchange,
                    std::make_exception_ptr(coap_unsupported_content_format_error(
                        "CoAP Content-Format " + std::to_string(*exchange.response_format))));
                _pending.erase(token);
                return;
            }
            media_type = *resolved;
        }
        if (!exchange.settled) {
            exchange.settled = true;
            exchange.message->resolve_callback(std::move(exchange.accumulated), media_type);
        }
        _pending.erase(token);
    }

    static auto settle_reject(pending_exchange& exchange, std::exception_ptr error) -> void {
        if (!exchange.settled) {
            exchange.settled = true;
            exchange.message->reject_callback(error);
        }
    }

    [[nodiscard]] static auto exception_for_code(CoapPDU::Code code, const std::string& endpoint)
        -> std::exception_ptr {
        const auto value = static_cast<std::uint8_t>(code);
        const std::string context = "CoAP request to " + endpoint;
        if (value >= CoapPDU::COAP_INTERNAL_SERVER_ERROR) {
            return std::make_exception_ptr(coap_server_error(value, context + ": server error"));
        }
        return std::make_exception_ptr(coap_client_error(value, context + ": client error"));
    }

    /// Requirement 4.4: suppress a Message ID seen recently.
    [[nodiscard]] auto is_duplicate(std::uint16_t message_id) -> bool {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(_seen, [now](const auto& entry) {
            return now - entry.second.received_time > std::chrono::seconds{60};
        });
        if (_seen.contains(message_id)) {
            return true;
        }
        _seen.emplace(message_id, received_message_info{message_id});
        return false;
    }

    /// Retransmission with exponential backoff, and expiry (Requirements 4.1,
    /// 4.2). Runs on every loop tick, so it needs no timer thread.
    auto service_timers() -> void {
        std::vector<std::pair<std::string, std::exception_ptr>> to_reject;
        {
            const std::lock_guard lock(_mutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [token, exchange] : _pending) {
                if (!exchange || exchange->settled) {
                    continue;
                }
                if (now >= exchange->expiry) {
                    to_reject.emplace_back(
                        token, std::make_exception_ptr(coap_timeout_error(
                                   "CoAP request to " + exchange->message->target_endpoint +
                                   " timed out")));
                    continue;
                }
                if (!exchange->message->is_confirmable || now < exchange->deadline) {
                    continue;
                }
                if (exchange->retransmissions >= _config.max_retransmit) {
                    to_reject.emplace_back(
                        token, std::make_exception_ptr(coap_timeout_error(
                                   "CoAP request to " + exchange->message->target_endpoint +
                                   " exhausted " + std::to_string(_config.max_retransmit) +
                                   " retransmissions")));
                    continue;
                }
                ++exchange->retransmissions;
                exchange->message->retransmission_count = exchange->retransmissions;
                exchange->backoff *= 2;
                exchange->deadline = now + exchange->backoff;
                send_datagram(exchange->datagram, exchange->peer);
            }
            for (auto& [token, error] : to_reject) {
                if (const auto it = _pending.find(token); it != _pending.end()) {
                    settle_reject(*it->second, error);
                    _pending.erase(it);
                }
            }
        }
    }
#endif  // CANTCOAP_AVAILABLE

    serializer_type _serializer;
    serializer_registry_type _registry;
    peer_capability_cache<std::uint64_t> _capability_cache;
    std::unordered_map<std::uint64_t, std::string> _node_id_to_endpoint;
    kythira::coap_client_config _config;
    metrics_type _metrics;
    std::shared_ptr<oscore::security_context> _oscore;

    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::unique_ptr<pending_exchange>> _pending;
    std::vector<std::string> _to_start;
    std::unordered_map<std::uint16_t, received_message_info> _seen;
    bool _shutting_down{false};
    std::atomic<std::uint64_t> _token_counter{1};
    std::atomic<std::uint16_t> _message_id_counter{1};
    std::mt19937 _rng{std::random_device{}()};

#ifdef CANTCOAP_AVAILABLE
    cantcoap_detail::udp_socket _socket;
    std::jthread _thread;
#endif
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
template<typename Types>
requires kythira::transport_types<Types>
class coap_cantcoap_server {
public:
    template<typename T> using future_template = typename Types::template future_template<T>;
    using serializer_type = typename Types::serializer_type;
    using serializer_registry_type = typename Types::serializer_registry_type;
    using metrics_type = typename Types::metrics_type;
    using executor_type = typename Types::executor_type;
    using address_type = std::string;
    using port_type = std::uint16_t;

    coap_cantcoap_server(std::string bind_address, std::uint16_t bind_port,
                         kythira::coap_server_config config, metrics_type metrics)
        : _registry{},
          _bind_address{std::move(bind_address)},
          _bind_port{bind_port},
          _actual_bound_port{bind_port},
          _config{std::move(config)},
          _metrics{std::move(metrics)} {
        kythira::coap_utils::validate_registry_content_formats(_registry);
        auto [selected, security] = cantcoap_detail::plan_security(_config, "server");
        _secure = selected == cantcoap_detail::channel::oscore;
        _security = std::move(security);
    }

    ~coap_cantcoap_server() { stop(); }

    coap_cantcoap_server(const coap_cantcoap_server&) = delete;
    auto operator=(const coap_cantcoap_server&) -> coap_cantcoap_server& = delete;
    coap_cantcoap_server(coap_cantcoap_server&&) = delete;
    auto operator=(coap_cantcoap_server&&) -> coap_cantcoap_server& = delete;

    // --- network_server concept surface ---

    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
            handler) -> void {
        const std::lock_guard lock(_mutex);
        _request_vote_handler = std::move(handler);
    }

    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
            handler) -> void {
        const std::lock_guard lock(_mutex);
        _append_entries_handler = std::move(handler);
    }

    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<>&)>
                                               handler) -> void {
        const std::lock_guard lock(_mutex);
        _install_snapshot_handler = std::move(handler);
    }

    auto start() -> void {
        if (_running.load()) {
            return;
        }
#ifdef CANTCOAP_AVAILABLE
        if (_secure) {
            _oscore = std::make_shared<oscore::security_context>(
                std::get<oscore_credentials>(_security.credentials));
        }
        // One AF_INET6 socket with IPV6_V6ONLY off, so v4 peers arrive
        // v4-mapped. _bind_address is recorded for parity with the other
        // backends and for diagnostics; this binds the wildcard, as they do.
        _socket.open(_bind_port);
        _actual_bound_port = _socket.bound_port();
        _running.store(true);
        _thread = std::jthread([this](std::stop_token stop) { run_loop(stop); });
#else
        throw coap_transport_error(
            "cantcoap CoAP backend unavailable: rebuild with the vcpkg 'coap-cantcoap' feature so "
            "CANTCOAP_AVAILABLE is defined");
#endif
    }

    auto stop() -> void {
#ifdef CANTCOAP_AVAILABLE
        if (_thread.joinable()) {
            _thread.request_stop();
            _thread.join();
        }
        _socket.close();
        _oscore.reset();
        {
            const std::lock_guard lock(_mutex);
            _block1_assembly.clear();
            _seen.clear();
        }
#endif
        _running.store(false);
    }

    [[nodiscard]] auto is_running() const -> bool { return _running.load(); }

    /// The UDP port actually bound by start(). Differs from the constructor's
    /// port when that was 0 (ephemeral); before start() it reports the
    /// requested port unchanged, matching the other backends.
    [[nodiscard]] auto bound_port() const -> port_type { return _actual_bound_port; }

    [[nodiscard]] static constexpr auto backend_available() -> bool {
#ifdef CANTCOAP_AVAILABLE
        return true;
#else
        return false;
#endif
    }

private:
#ifdef CANTCOAP_AVAILABLE
    auto run_loop(std::stop_token stop) -> void {
        std::vector<std::uint8_t> buffer(cantcoap_max_datagram);
        while (!stop.stop_requested()) {
            pollfd fds{};
            fds.fd = _socket.fd();
            fds.events = POLLIN;
            const int ready = ::poll(&fds, 1, cantcoap_poll_interval_ms);
            if (ready <= 0 || (fds.revents & POLLIN) == 0) {
                continue;
            }
            sockaddr_in6 from{};
            socklen_t from_length = sizeof(from);
            const auto received = ::recvfrom(_socket.fd(), buffer.data(), buffer.size(), 0,
                                             reinterpret_cast<sockaddr*>(&from), &from_length);
            if (received <= 0) {
                continue;
            }
            // Every failure below is a drop, never a throw out of the loop: a
            // malformed or undecryptable datagram must not take the server down
            // (Requirement 6.4, and the robustness test that checks it).
            try {
                handle_datagram(buffer.data(), static_cast<int>(received), from);
            } catch (const std::exception&) {
                // Swallowed deliberately; the peer sees a timeout, we stay up.
            }
        }
    }

    auto handle_datagram(std::uint8_t* data, int length, const sockaddr_in6& from) -> void {
        std::vector<std::byte> plain(reinterpret_cast<std::byte*>(data),
                                     reinterpret_cast<std::byte*>(data) + length);
        oscore::request_binding binding;

        if (_oscore) {
            try {
                const auto outer = oscore::parse_message(plain);
                const auto inner = _oscore->unprotect_request(outer, binding);
                plain = oscore::serialize_message(inner);
            } catch (const std::exception&) {
                return;  // Unverifiable: drop without a handler ever seeing it.
            }
        }

        CoapPDU pdu(reinterpret_cast<std::uint8_t*>(plain.data()), static_cast<int>(plain.size()));
        if (pdu.validate() != 1) {
            return;  // Malformed.
        }
        if (pdu.getCode() != CoapPDU::COAP_POST) {
            send_error(pdu, from, binding, CoapPDU::COAP_METHOD_NOT_ALLOWED);
            return;
        }

        const std::lock_guard lock(_mutex);
        if (is_duplicate(pdu.getMessageID())) {
            return;  // Requirement 4.4.
        }

        const auto path = cantcoap_detail::read_uri_path(pdu);
        const auto options = cantcoap_detail::scan_options(pdu);

        std::vector<std::byte> body;
        const auto* payload = pdu.getPayloadPointer();
        const auto payload_length = pdu.getPayloadLength();
        if (payload != nullptr && payload_length > 0) {
            const auto* start = reinterpret_cast<const std::byte*>(payload);
            body.assign(start, start + payload_length);
        }

        // Block1 reassembly (Requirement 5.1). Keyed by path: this backend
        // holds one exchange per peer at a time, which is what the Raft
        // transport actually does.
        if (options.block1) {
            const auto expected =
                static_cast<std::size_t>(options.block1->block_number) * options.block1->block_size;
            if (options.block1->block_number == 0) {
                _block1_assembly.clear();
            }
            if (expected != _block1_assembly.size()) {
                _block1_assembly.clear();
                send_error(pdu, from, binding, CoapPDU::COAP_REQUEST_ENTITY_TOO_LARGE);
                return;
            }
            if (_block1_assembly.size() + body.size() > _config.max_request_size) {
                _block1_assembly.clear();
                send_error(pdu, from, binding, CoapPDU::COAP_REQUEST_ENTITY_TOO_LARGE);
                return;
            }
            _block1_assembly.insert(_block1_assembly.end(), body.begin(), body.end());
            if (options.block1->more_blocks) {
                send_continue(pdu, from, binding, *options.block1);
                return;
            }
            body = std::move(_block1_assembly);
            _block1_assembly.clear();
        }

        // Content negotiation, exactly as the other two backends do it.
        std::string request_media_type = _registry.default_media_type();
        if (options.content_format) {
            const auto resolved = kythira::coap_utils::registry_media_type_for_content_format(
                _registry, kythira::coap_utils::parse_content_format(*options.content_format));
            if (!resolved) {
                send_error(pdu, from, binding, CoapPDU::COAP_UNSUPPORTED_CONTENT_FORMAT);
                return;
            }
            request_media_type = *resolved;
        }
        std::string response_media_type = request_media_type;
        if (!options.accepted_formats.empty()) {
            std::vector<std::string> accepted;
            for (const auto format : options.accepted_formats) {
                if (const auto resolved =
                        kythira::coap_utils::registry_media_type_for_content_format(
                            _registry, kythira::coap_utils::parse_content_format(format))) {
                    accepted.push_back(*resolved);
                }
            }
            const auto selected = _registry.select_output_media_type(accepted);
            if (!selected) {
                send_error(pdu, from, binding, CoapPDU::COAP_NOT_ACCEPTABLE);
                return;
            }
            response_media_type = *selected;
        }

        std::vector<std::byte> encoded;
        try {
            if (path == cantcoap_request_vote_path) {
                if (!_request_vote_handler) {
                    send_error(pdu, from, binding, CoapPDU::COAP_NOT_IMPLEMENTED);
                    return;
                }
                encoded = _registry.encode_with(
                    response_media_type,
                    _request_vote_handler(
                        _registry.template decode_with<kythira::request_vote_request<>>(
                            request_media_type, body)));
            } else if (path == cantcoap_append_entries_path) {
                if (!_append_entries_handler) {
                    send_error(pdu, from, binding, CoapPDU::COAP_NOT_IMPLEMENTED);
                    return;
                }
                encoded = _registry.encode_with(
                    response_media_type,
                    _append_entries_handler(
                        _registry.template decode_with<kythira::append_entries_request<>>(
                            request_media_type, body)));
            } else if (path == cantcoap_install_snapshot_path) {
                if (!_install_snapshot_handler) {
                    send_error(pdu, from, binding, CoapPDU::COAP_NOT_IMPLEMENTED);
                    return;
                }
                encoded = _registry.encode_with(
                    response_media_type,
                    _install_snapshot_handler(
                        _registry.template decode_with<kythira::install_snapshot_request<>>(
                            request_media_type, body)));
            } else {
                send_error(pdu, from, binding, CoapPDU::COAP_NOT_FOUND);
                return;
            }
        } catch (const std::exception&) {
            send_error(pdu, from, binding, CoapPDU::COAP_INTERNAL_SERVER_ERROR);
            return;
        }

        send_content(pdu, from, binding, encoded, response_media_type,
                     options.block2 ? options.block2->block_number : 0);
    }

    /// Build a reply that echoes the request's token and Message ID, as a
    /// piggy-backed ACK when the request was confirmable.
    [[nodiscard]] auto begin_reply(CoapPDU& request, CoapPDU::Code code)
        -> std::unique_ptr<CoapPDU> {
        auto reply = std::make_unique<CoapPDU>();
        reply->setVersion(1);
        reply->setType(request.getType() == CoapPDU::COAP_CONFIRMABLE
                           ? CoapPDU::COAP_ACKNOWLEDGEMENT
                           : CoapPDU::COAP_NON_CONFIRMABLE);
        reply->setCode(code);
        reply->setMessageID(request.getMessageID());
        reply->setToken(request.getTokenPointer(),
                        static_cast<std::uint8_t>(request.getTokenLength()));
        return reply;
    }

    auto finish_reply(CoapPDU& reply, const sockaddr_in6& to,
                      const oscore::request_binding& binding) -> void {
        const auto* start = reinterpret_cast<const std::byte*>(reply.getPDUPointer());
        std::vector<std::byte> bytes(start, start + reply.getPDULength());
        if (_oscore) {
            const auto inner = oscore::parse_message(bytes);
            bytes = oscore::serialize_message(_oscore->protect_response(inner, binding));
        }
        ::sendto(_socket.fd(), bytes.data(), bytes.size(), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    auto send_error(CoapPDU& request, const sockaddr_in6& to,
                    const oscore::request_binding& binding, CoapPDU::Code code) -> void {
        auto reply = begin_reply(request, code);
        finish_reply(*reply, to, binding);
    }

    /// 2.31 Continue, echoing the Block1 option so the peer knows which block
    /// landed (RFC 7959 Section 2.5).
    auto send_continue(CoapPDU& request, const sockaddr_in6& to,
                       const oscore::request_binding& binding, const block_option& block) -> void {
        auto reply = begin_reply(request, static_cast<CoapPDU::Code>(0x5F));
        block_option echo = block;
        echo.more_blocks = true;
        cantcoap_detail::add_uint_option(*reply, CoapPDU::COAP_OPTION_BLOCK1, echo.encode());
        finish_reply(*reply, to, binding);
    }

    /// 2.05 Content, sliced into Block2 when the body exceeds one block.
    /// Stateless: the block number comes from the request, so nothing has to be
    /// remembered between datagrams.
    auto send_content(CoapPDU& request, const sockaddr_in6& to,
                      const oscore::request_binding& binding, const std::vector<std::byte>& body,
                      const std::string& media_type, std::uint32_t block_number) -> void {
        const auto format = kythira::coap_utils::media_type_to_coap_content_format(media_type);
        if (!format) {
            send_error(request, to, binding, CoapPDU::COAP_UNSUPPORTED_CONTENT_FORMAT);
            return;
        }
        auto reply = begin_reply(request, CoapPDU::COAP_CONTENT);
        cantcoap_detail::add_uint_option(*reply, CoapPDU::COAP_OPTION_CONTENT_FORMAT,
                                         static_cast<std::uint32_t>(*format));

        const std::size_t block_size =
            _config.enable_block_transfer &&
                    kythira::coap_utils::is_valid_block_size(_config.max_block_size)
                ? _config.max_block_size
                : 1024;
        if (body.size() > block_size) {
            const std::size_t offset = static_cast<std::size_t>(block_number) * block_size;
            if (offset >= body.size()) {
                send_error(request, to, binding, CoapPDU::COAP_BAD_OPTION);
                return;
            }
            const std::size_t chunk = std::min(block_size, body.size() - offset);
            block_option block;
            block.block_number = block_number;
            block.more_blocks = (offset + chunk) < body.size();
            block.block_size = static_cast<std::uint32_t>(block_size);
            cantcoap_detail::add_uint_option(*reply, CoapPDU::COAP_OPTION_BLOCK2, block.encode());
            reply->setPayload(
                reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(body.data()) + offset),
                static_cast<int>(chunk));
        } else if (!body.empty()) {
            reply->setPayload(reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(body.data())),
                              static_cast<int>(body.size()));
        }
        finish_reply(*reply, to, binding);
    }

    [[nodiscard]] auto is_duplicate(std::uint16_t message_id) -> bool {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(_seen, [now](const auto& entry) {
            return now - entry.second.received_time > std::chrono::seconds{60};
        });
        if (_seen.contains(message_id)) {
            return true;
        }
        _seen.emplace(message_id, received_message_info{message_id});
        return false;
    }
#endif  // CANTCOAP_AVAILABLE

    serializer_type _serializer;
    serializer_registry_type _registry;
    address_type _bind_address;
    port_type _bind_port;
    port_type _actual_bound_port;
    kythira::coap_server_config _config;
    metrics_type _metrics;
    coap_security_config _security{};
    bool _secure{false};
    std::shared_ptr<oscore::security_context> _oscore;

    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)>
        _request_vote_handler;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
        _append_entries_handler;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        _install_snapshot_handler;

    mutable std::mutex _mutex;
    std::atomic<bool> _running{false};
    std::unordered_map<std::uint16_t, received_message_info> _seen;
    std::vector<std::byte> _block1_assembly;

#ifdef CANTCOAP_AVAILABLE
    cantcoap_detail::udp_socket _socket;
    std::jthread _thread;
#endif
};

}  // namespace kythira
