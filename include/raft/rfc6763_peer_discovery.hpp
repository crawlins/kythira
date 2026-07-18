#pragma once

#include <raft/fault_injection.hpp>
#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef KYTHIRA_HAS_LDNS

#include <ldns/ldns.h>

namespace kythira {

// Partial peer_discovery: find_peers only, via a single RFC 6763 SRV query at
// the cluster-level service name. Does NOT implement register_node and is not
// verified against the peer_discovery concept — it is a building block for
// rfc6763_ldns_peer_discovery.
class rfc6763_peer_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    struct config {
        std::string server;
        uint16_t port{53};
        std::string service_name;  // cluster-level, e.g. "_raft._tcp.cluster.example.com."
    };

    explicit rfc6763_peer_discovery(config cfg) : _cfg(std::move(cfg)) {}

    auto register_node(std::string /*self_id*/, std::string /*self_address*/)
        -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
        std::vector<peer_info<std::string, std::string>> results;

        struct ResolverDeleter {
            void operator()(ldns_resolver* r) const noexcept { ldns_resolver_free(r); }
        };
        struct PktDeleter {
            void operator()(ldns_pkt* p) const noexcept { ldns_pkt_free(p); }
        };
        struct RdfDeleter {
            void operator()(ldns_rdf* r) const noexcept { ldns_rdf_deep_free(r); }
        };

        std::unique_ptr<ldns_resolver, ResolverDeleter> res{ldns_resolver_new()};
        if (!res) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        ldns_resolver_set_port(res.get(), _cfg.port);

        {
            ldns_rdf* ns_rdf = nullptr;
            // Try IPv4 first, then IPv6
            if (ldns_str2rdf_a(&ns_rdf, _cfg.server.c_str()) != LDNS_STATUS_OK) {
                ldns_str2rdf_aaaa(&ns_rdf, _cfg.server.c_str());
            }
            if (ns_rdf != nullptr) {
                ldns_resolver_push_nameserver(res.get(), ns_rdf);
                ldns_rdf_deep_free(ns_rdf);
            }
        }

        struct timeval tv{};
        auto ms = timeout.count();
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ldns_resolver_set_timeout(res.get(), tv);

        std::unique_ptr<ldns_rdf, RdfDeleter> qname{
            ldns_dname_new_frm_str(_cfg.service_name.c_str())};
        if (!qname) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        // Chaos: simulate DNS failure (returns empty) or inject synthetic peer lists.
        fiu_do_on("raft/dns/rfc6763/find_peers/fail",
                  return kythira::FutureFactory::makeFuture(
                      std::vector<peer_info<std::string, std::string>>{}););
        fiu_do_on("raft/dns/rfc6763/find_peers/inject",
                  return kythira::FutureFactory::makeFuture(
                      std::vector<peer_info<std::string, std::string>>{
                          {"node1.cluster.example.com.", "node1.cluster.example.com.:4001"},
                          {"node2.cluster.example.com.", "node2.cluster.example.com.:4002"}}););

        std::unique_ptr<ldns_pkt, PktDeleter> pkt{ldns_resolver_query(
            res.get(), qname.get(), LDNS_RR_TYPE_SRV, LDNS_RR_CLASS_IN, LDNS_RD)};
        if (!pkt) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        ldns_rr_list* answer = ldns_pkt_answer(pkt.get());
        if (answer == nullptr) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        std::size_t count = ldns_rr_list_rr_count(answer);
        for (std::size_t i = 0; i < count; ++i) {
            ldns_rr* rr = ldns_rr_list_rr(answer, i);
            if ((rr == nullptr) || ldns_rr_get_type(rr) != LDNS_RR_TYPE_SRV) {
                continue;
            }
            // SRV rdata layout: priority(0), weight(1), port(2), target(3).
            ldns_rdf* port_rdf = ldns_rr_rdf(rr, 2);
            ldns_rdf* target_rdf = ldns_rr_rdf(rr, 3);
            if ((port_rdf == nullptr) || (target_rdf == nullptr)) {
                continue;
            }
            uint16_t port = ldns_rdf2native_int16(port_rdf);
            char* target_cstr = ldns_rdf2str(target_rdf);
            if (target_cstr == nullptr) {
                continue;
            }
            std::string target{target_cstr};
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            free(target_cstr);
            std::string address = target + ":" + std::to_string(port);
            results.push_back(peer_info<std::string, std::string>{target, address});
        }

        return kythira::FutureFactory::makeFuture(std::move(results));
    }

private:
    config _cfg;
};

}  // namespace kythira

#endif  // KYTHIRA_HAS_LDNS
