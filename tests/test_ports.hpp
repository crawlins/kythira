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
//
// Checking free is not enough on its own. A binary picks its ports when it
// starts and binds them case by case for minutes afterwards, and until a
// case binds, nothing stops another binary's check from finding the same
// port free and taking it. That happened about once in 26 CI runs after the
// fixed ports were retired (proxygen_transport_test lost two ports of one
// block to another binary in a single run, 2026-11-06). So every port handed
// out is also claimed: a one-byte fcntl write lock at offset `port` in one
// shared lock file, held until the process exits, which releases it even on
// a crash. A port another test process has claimed is never handed out
// again while that process lives. The file defaults to
// $TMPDIR/kythira-test-ports.lock (or /tmp/...), and
// $KYTHIRA_TEST_PORT_LOCK_FILE overrides it. If it cannot be opened, ports
// are still checked free, just not claimed.

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
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

/// This process's port bookkeeping. Never destroyed: the lock file must stay
/// open until exit, because closing any descriptor of it drops every fcntl
/// lock this process holds on it.
struct port_registry {
    std::mutex mu;
    std::mt19937 rng{std::random_device{}()};
    std::set<std::uint16_t> claimed;  // fcntl locks never conflict within one process
    int lock_fd = -1;

    port_registry() {
        std::string path;
        if (const char* env = std::getenv("KYTHIRA_TEST_PORT_LOCK_FILE");
            env != nullptr && *env != '\0') {
            path = env;
        } else {
            const char* tmp = std::getenv("TMPDIR");
            path = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") +
                   "/kythira-test-ports.lock";
        }
        lock_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    }
};

inline auto registry() -> port_registry& {
    static auto* r = new port_registry();  // intentionally leaked, see above
    return *r;
}

/// Takes the cross-process claim on `port`. False if another process holds
/// it. Always true when there is no lock file.
inline auto claim_port(port_registry& r, std::uint16_t port) -> bool {
    if (r.lock_fd < 0) {
        return true;
    }
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = port;
    fl.l_len = 1;
    return ::fcntl(r.lock_fd, F_SETLK, &fl) == 0;
}

inline auto unclaim_port(port_registry& r, std::uint16_t port) -> void {
    if (r.lock_fd < 0) {
        return;
    }
    struct flock fl{};
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = port;
    fl.l_len = 1;
    ::fcntl(r.lock_fd, F_SETLK, &fl);
}

}  // namespace detail

/// The first of `count` consecutive loopback ports that are all free now and
/// claimed by no other test process, chosen at random below the ephemeral
/// range. The claim lasts until this process exits. For tests that address
/// their servers as `base + k`.
inline auto free_port_block(std::uint16_t count) -> std::uint16_t {
    auto& r = detail::registry();
    const int ceiling = ephemeral_port_floor();
    const int floor = ceiling > 12000 ? 10000 : 1025;
    if (count == 0 || ceiling - floor <= count) {
        throw std::invalid_argument("free_port_block: bad block size " + std::to_string(count));
    }
    std::uniform_int_distribution<int> pick(floor, ceiling - count);
    const std::lock_guard lock(r.mu);
    for (int attempt = 0; attempt < 500; ++attempt) {
        const int base = pick(r.rng);
        int taken = 0;
        bool ok = true;
        for (; taken < count; ++taken) {
            const auto port = static_cast<std::uint16_t>(base + taken);
            if (r.claimed.contains(port) || !detail::claim_port(r, port)) {
                ok = false;
                break;
            }
            if (!detail::loopback_port_is_free(port)) {
                detail::unclaim_port(r, port);
                ok = false;
                break;
            }
        }
        if (ok) {
            for (int k = 0; k < count; ++k) {
                r.claimed.insert(static_cast<std::uint16_t>(base + k));
            }
            return static_cast<std::uint16_t>(base);
        }
        for (int k = 0; k < taken; ++k) {
            detail::unclaim_port(r, static_cast<std::uint16_t>(base + k));
        }
    }
    throw std::runtime_error("free_port_block: no free block of " + std::to_string(count) +
                             " ports below the ephemeral range");
}

/// One loopback port that is free now and claimed by no other test process,
/// chosen at random below the ephemeral range.
inline auto free_port() -> std::uint16_t {
    return free_port_block(1);
}

}  // namespace kythira::testing
