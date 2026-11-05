// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Loopback ports for tests that listen on TCP.
//
// Tests used to hardcode ports in 182xx-185xx, each file picking "its own"
// range. The ranges drifted into each other: by November 2026 eighteen ports
// were claimed by two or more test binaries (18200, 18220, 18230, 18250,
// 18290, 18295, 18320, ...), and ctest runs those binaries in parallel, so
// one of them failed "bind() on port N" whenever the schedules lined up
// (beast_server_test on 18332, proxygen_transport_test on 18288, 18345 and
// 18423). These helpers pick ports at run time instead.
//
// Ports come from BELOW the kernel's ephemeral range. A port from bind(0)
// is an ephemeral port, and between the check here and the server's own
// bind any outgoing connection on the machine can be handed it as its local
// port. A port below the range is never given to an outgoing connection, so
// the only thing that can take it in that window is another process binding
// an explicit port. The choice is random so two binaries rarely try the
// same one, and every port is checked free before it is returned.

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>

namespace kythira::testing {

/// The low end of the kernel's ephemeral port range, 32768 if it cannot be
/// read.
inline auto ephemeral_port_floor() -> std::uint16_t {
    std::ifstream in("/proc/sys/net/ipv4/ip_local_port_range");
    unsigned low = 0;
    if (in >> low && low > 1024U && low <= 65535U) {
        return static_cast<std::uint16_t>(low);
    }
    return 32768;
}

namespace detail {

/// True when nothing on this machine is bound to `port` on 127.0.0.1 or the
/// IPv4 wildcard. SO_REUSEADDR is left off so a listener on the wildcard
/// address also counts as taken.
inline auto loopback_port_is_free(std::uint16_t port) -> bool {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("test_ports: socket() failed");
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return bound;
}

}  // namespace detail

/// The first of `count` consecutive loopback ports that are all free now,
/// chosen at random below the ephemeral range. For tests that address their
/// servers as `base + k`.
inline auto free_port_block(std::uint16_t count) -> std::uint16_t {
    static std::mutex mu;
    static std::mt19937 rng{std::random_device{}()};
    const int ceiling = ephemeral_port_floor();
    const int floor = ceiling > 12000 ? 10000 : 1025;
    if (count == 0 || ceiling - floor <= count) {
        throw std::invalid_argument("free_port_block: bad block size " + std::to_string(count));
    }
    std::uniform_int_distribution<int> pick(floor, ceiling - count);
    for (int attempt = 0; attempt < 500; ++attempt) {
        int base = 0;
        {
            std::lock_guard lock(mu);
            base = pick(rng);
        }
        bool all_free = true;
        for (int k = 0; k < count && all_free; ++k) {
            all_free = detail::loopback_port_is_free(static_cast<std::uint16_t>(base + k));
        }
        if (all_free) {
            return static_cast<std::uint16_t>(base);
        }
    }
    throw std::runtime_error("free_port_block: no free block of " + std::to_string(count) +
                             " ports below the ephemeral range");
}

/// One loopback port that is free now, chosen at random below the ephemeral
/// range.
inline auto free_port() -> std::uint16_t {
    return free_port_block(1);
}

}  // namespace kythira::testing
