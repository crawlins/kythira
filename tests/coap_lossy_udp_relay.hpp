// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A loopback UDP forwarder that sits between one CoAP client and one server
// and loses the datagrams a test tells it to. It is how the duplicate-replay
// tests (.kiro/specs/coap-cantcoap-duplicate-replay/ Requirement 5) lose a
// reply for real rather than asserting on a request that was never repeated.
//
// The client is pointed at port(); everything it sends goes to the server
// from one upstream socket, and everything the server sends back goes to the
// client that spoke last. Each datagram is offered to the drop rule before it
// is forwarded, and every datagram is recorded, dropped or not, so a test can
// compare a lost reply with the copy that replaced it.
//
// Free of any CoAP library header, like coap_wire_probe.hpp, so the libcoap
// and libnyoci suites can include it too.

#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::testing {

enum class relay_direction {
    to_server,
    to_client
};

class lossy_udp_relay {
public:
    /// Decides whether to drop a datagram: its direction, how many datagrams
    /// went that way before it, and its bytes.
    using drop_rule = std::function<bool(relay_direction, std::size_t index,
                                         const std::vector<std::uint8_t>& datagram)>;

    struct record {
        relay_direction direction;
        std::vector<std::uint8_t> datagram;
        bool dropped;
    };

    lossy_udp_relay(std::uint16_t server_port, drop_rule rule)
        : _rule{std::move(rule)},
          _downstream{open_loopback()},
          _upstream{open_loopback()},
          _server{loopback(server_port)} {
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        BOOST_REQUIRE(::getsockname(_downstream, reinterpret_cast<sockaddr*>(&bound), &length) ==
                      0);
        _port = ntohs(bound.sin_port);
        _thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    lossy_udp_relay(const lossy_udp_relay&) = delete;
    auto operator=(const lossy_udp_relay&) -> lossy_udp_relay& = delete;

    ~lossy_udp_relay() {
        _thread.request_stop();
        _thread.join();
        ::close(_downstream);
        ::close(_upstream);
    }

    /// The port the client sends to instead of the server's.
    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

    [[nodiscard]] auto records() const -> std::vector<record> {
        const std::lock_guard lock(_mutex);
        return _records;
    }

    /// The datagrams that went one way, dropped ones included, in order.
    [[nodiscard]] auto sent(relay_direction direction) const -> std::vector<record> {
        std::vector<record> out;
        for (auto& entry : records()) {
            if (entry.direction == direction) {
                out.push_back(std::move(entry));
            }
        }
        return out;
    }

    [[nodiscard]] auto dropped() const -> std::size_t {
        std::size_t count = 0;
        for (const auto& entry : records()) {
            count += entry.dropped ? 1U : 0U;
        }
        return count;
    }

    /// Drops the index-th datagram (0-based) going one way, and nothing else.
    [[nodiscard]] static auto drop_nth(relay_direction direction, std::size_t index) -> drop_rule {
        return
            [direction, index](relay_direction d, std::size_t i, const std::vector<std::uint8_t>&) {
                return d == direction && i == index;
            };
    }

private:
    [[nodiscard]] static auto loopback(std::uint16_t port) -> sockaddr_in {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return addr;
    }

    [[nodiscard]] static auto open_loopback() -> int {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        BOOST_REQUIRE(fd >= 0);
        auto addr = loopback(0);
        BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        return fd;
    }

    auto run(std::stop_token stop) -> void {
        std::vector<std::uint8_t> buffer(65536);
        std::optional<sockaddr_in> client;
        std::size_t to_server = 0;
        std::size_t to_client = 0;
        while (!stop.stop_requested()) {
            std::array<pollfd, 2> fds{pollfd{_downstream, POLLIN, 0}, pollfd{_upstream, POLLIN, 0}};
            if (::poll(fds.data(), fds.size(), 20) <= 0) {
                continue;
            }
            if ((fds[0].revents & POLLIN) != 0) {
                sockaddr_in from{};
                socklen_t length = sizeof(from);
                const auto n = ::recvfrom(_downstream, buffer.data(), buffer.size(), 0,
                                          reinterpret_cast<sockaddr*>(&from), &length);
                if (n > 0) {
                    client = from;
                    std::vector<std::uint8_t> datagram(buffer.begin(), buffer.begin() + n);
                    if (!offer(relay_direction::to_server, to_server++, datagram)) {
                        (void)::sendto(_upstream, datagram.data(), datagram.size(), 0,
                                       reinterpret_cast<const sockaddr*>(&_server),
                                       sizeof(_server));
                    }
                }
            }
            if ((fds[1].revents & POLLIN) != 0) {
                const auto n = ::recv(_upstream, buffer.data(), buffer.size(), 0);
                if (n > 0 && client) {
                    std::vector<std::uint8_t> datagram(buffer.begin(), buffer.begin() + n);
                    if (!offer(relay_direction::to_client, to_client++, datagram)) {
                        (void)::sendto(_downstream, datagram.data(), datagram.size(), 0,
                                       reinterpret_cast<const sockaddr*>(&*client),
                                       sizeof(*client));
                    }
                }
            }
        }
    }

    /// Records a datagram and returns whether to drop it.
    auto offer(relay_direction direction, std::size_t index,
               const std::vector<std::uint8_t>& datagram) -> bool {
        const bool drop = _rule && _rule(direction, index, datagram);
        const std::lock_guard lock(_mutex);
        _records.push_back({direction, datagram, drop});
        return drop;
    }

    drop_rule _rule;
    int _downstream;
    int _upstream;
    sockaddr_in _server;
    std::uint16_t _port{0};
    mutable std::mutex _mutex;
    std::vector<record> _records;
    std::jthread _thread;
};

}  // namespace kythira::testing
