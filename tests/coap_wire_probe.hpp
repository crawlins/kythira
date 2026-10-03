// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A bound loopback UDP socket that stands in for a CoAP server when only the
// bytes a client puts on the wire matter, or when the test needs a peer that
// answers with one fixed error code, plus the one header field the
// TimeoutNow tests read from them. Shared by every backend's TimeoutNow test
// (.kiro/specs/coap-transport-multi-raft/ tasks 6 and 7) so "always
// confirmable" is checked the same way on libcoap, libnyoci and cantcoap.
//
// Free of any CoAP library header on purpose: <coap3/coap.h> and
// <libnyoci/libnyoci.h> cannot share a translation unit, and this has to be
// includable beside either.

#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kythira::testing {

class udp_probe {
public:
    udp_probe() : _fd{::socket(AF_INET, SOCK_DGRAM, 0)} {
        BOOST_REQUIRE(_fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_REQUIRE(::bind(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        BOOST_REQUIRE(::getsockname(_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        _port = ntohs(addr.sin_port);
    }
    udp_probe(const udp_probe&) = delete;
    auto operator=(const udp_probe&) -> udp_probe& = delete;
    ~udp_probe() { close(); }

    auto close() -> void {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

    [[nodiscard]] auto endpoint() const -> std::string {
        return "coap://127.0.0.1:" + std::to_string(_port);
    }

    /// The first datagram to arrive, or nullopt if none does in time.
    auto receive() -> std::optional<std::vector<std::uint8_t>> {
        std::vector<std::uint8_t> buffer(1500);
        const auto deadline = std::chrono::steady_clock::now() + scaled_deadline(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd fds{_fd, POLLIN, 0};
            if (::poll(&fds, 1, 50) > 0) {
                const auto n = ::recv(_fd, buffer.data(), buffer.size(), 0);
                if (n > 0) {
                    buffer.resize(static_cast<std::size_t>(n));
                    return buffer;
                }
            }
        }
        return std::nullopt;
    }

    /// Receive one request and answer it with `code` (class << 5 | detail)
    /// and no payload, the way a peer with no such resource (4.04) or no
    /// handler (5.01) does. Stands in for a peer on an older build without
    /// pulling a second CoAP library into the test. Returns the request, or
    /// nullopt if none arrived in time.
    auto answer(std::uint8_t code) -> std::optional<std::vector<std::uint8_t>> {
        std::vector<std::uint8_t> buffer(1500);
        const auto deadline = std::chrono::steady_clock::now() + scaled_deadline(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd fds{_fd, POLLIN, 0};
            if (::poll(&fds, 1, 50) <= 0) {
                continue;
            }
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const auto n = ::recvfrom(_fd, buffer.data(), buffer.size(), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 4) {
                continue;
            }
            buffer.resize(static_cast<std::size_t>(n));
            // RFC 7252 Section 3: a CON request gets a piggy-backed ACK with
            // its Message ID; a NON one a NON reply. Either echoes the token.
            const auto token_length = static_cast<std::size_t>(buffer[0] & 0x0F);
            const auto type = coap_type_of(buffer[0]);
            std::vector<std::uint8_t> reply;
            reply.push_back(static_cast<std::uint8_t>(0x40 | ((type == 0 ? 2 : 1) << 4) |
                                                      static_cast<int>(token_length)));
            reply.push_back(code);
            reply.push_back(buffer[2]);
            reply.push_back(buffer[3]);
            reply.insert(reply.end(), buffer.begin() + 4,
                         buffer.begin() + 4 + static_cast<std::ptrdiff_t>(token_length));
            if (::sendto(_fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from),
                         from_len) < 0) {
                return std::nullopt;
            }
            return buffer;
        }
        return std::nullopt;
    }

private:
    static auto coap_type_of(std::uint8_t first_byte) -> std::uint8_t {
        return static_cast<std::uint8_t>((first_byte >> 4) & 0x03);
    }

    int _fd;
    std::uint16_t _port{0};
};

// RFC 7252 Section 3: the message type is bits 4-5 of the first byte.
inline constexpr std::uint8_t coap_type_con = 0;
inline constexpr std::uint8_t coap_type_non = 1;

[[nodiscard]] inline auto coap_message_type(const std::vector<std::uint8_t>& datagram)
    -> std::uint8_t {
    return static_cast<std::uint8_t>((datagram.at(0) >> 4) & 0x03);
}

}  // namespace kythira::testing
