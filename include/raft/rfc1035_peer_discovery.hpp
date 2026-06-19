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

class rfc1035_peer_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    struct config {
        std::string server;
        uint16_t port{53};
        std::string shared_name;
    };

    explicit rfc1035_peer_discovery(config cfg) : _cfg(std::move(cfg)) {}

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
            if (ns_rdf) {
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
            ldns_dname_new_frm_str(_cfg.shared_name.c_str())};
        if (!qname) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        // Chaos: simulate DNS failure (returns empty) or inject synthetic peer lists.
        fiu_do_on("raft/dns/rfc1035/find_peers/fail",
                  return kythira::FutureFactory::makeFuture(
                      std::vector<peer_info<std::string, std::string>>{}););
        fiu_do_on("raft/dns/rfc1035/find_peers/inject_ipv4",
                  return kythira::FutureFactory::makeFuture(
                      std::vector<peer_info<std::string, std::string>>{{"10.0.0.1", "10.0.0.1"},
                                                                       {"10.0.0.2", "10.0.0.2"}}););
        fiu_do_on("raft/dns/rfc1035/find_peers/inject_mixed",
                  return kythira::FutureFactory::makeFuture(
                      std::vector<peer_info<std::string, std::string>>{{"10.0.0.1", "10.0.0.1"},
                                                                       {"::2", "::2"}}););

        for (ldns_rr_type qtype : {LDNS_RR_TYPE_A, LDNS_RR_TYPE_AAAA}) {
            std::unique_ptr<ldns_pkt, PktDeleter> pkt{
                ldns_resolver_query(res.get(), qname.get(), qtype, LDNS_RR_CLASS_IN, LDNS_RD)};
            if (!pkt) {
                continue;
            }

            ldns_rr_list* answer = ldns_pkt_answer(pkt.get());
            if (!answer) {
                continue;
            }

            std::size_t count = ldns_rr_list_rr_count(answer);
            for (std::size_t i = 0; i < count; ++i) {
                ldns_rr* rr = ldns_rr_list_rr(answer, i);
                if (!rr) {
                    continue;
                }
                ldns_rdf* rdata = ldns_rr_rdf(rr, 0);
                if (!rdata) {
                    continue;
                }
                char* ip_cstr = ldns_rdf2str(rdata);
                if (!ip_cstr) {
                    continue;
                }
                std::string ip{ip_cstr};
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
                free(ip_cstr);
                results.push_back(peer_info<std::string, std::string>{ip, ip});
            }
        }

        return kythira::FutureFactory::makeFuture(std::move(results));
    }

private:
    config _cfg;
};

}  // namespace kythira

#endif  // KYTHIRA_HAS_LDNS
