// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Boost.Asio front end for net_bind: one acceptor per address a bind
// address resolves to, so "*" or "localhost" can listen on IPv4 and IPv6.

#include <raft/net_bind.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kythira::net_bind {

// Opens a listening acceptor on `port` for every address `address`
// resolves to (see resolve_bind_addresses() for what it accepts). With
// port 0 every acceptor shares the one ephemeral port the kernel picked for
// the first. Shared pointers so an accept handler can keep its acceptor
// alive past the owner's stop(). Throws std::invalid_argument for an
// unusable address and std::runtime_error when a bind fails.
inline auto open_asio_acceptors(boost::asio::io_context& io, const std::string& address,
                                std::uint16_t port, const char* who)
    -> std::vector<std::shared_ptr<boost::asio::ip::tcp::acceptor>> {
    using tcp = boost::asio::ip::tcp;
    auto endpoints = resolve_bind_addresses(address, who);
    auto fds = open_listeners(endpoints, port, who);
    std::vector<std::shared_ptr<tcp::acceptor>> out;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        sockaddr_storage bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(fds[i], reinterpret_cast<sockaddr*>(&bound), &len);
        auto acceptor = std::make_shared<tcp::acceptor>(io);
        boost::system::error_code ec;
        acceptor->assign(bound.ss_family == AF_INET6 ? tcp::v6() : tcp::v4(), fds[i], ec);
        if (ec) {
            for (std::size_t j = i; j < fds.size(); ++j) ::close(fds[j]);
            throw std::runtime_error(std::string(who) + ": " + ec.message());
        }
        out.push_back(std::move(acceptor));
    }
    return out;
}

}  // namespace kythira::net_bind
