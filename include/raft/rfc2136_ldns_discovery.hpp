#pragma once

#include <raft/rfc1035_peer_discovery.hpp>

#ifdef KYTHIRA_HAS_LDNS

#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ldns/ldns.h>

namespace kythira {

class rfc2136_ldns_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    struct config {
        rfc1035_peer_discovery::config query;
        std::string zone;
        uint32_t ttl{30};
        std::string tsig_key_name;
        std::string tsig_algorithm{"hmac-sha256."};
        std::string tsig_key_base64;
    };

    explicit rfc2136_ldns_discovery(config cfg) : _cfg(std::move(cfg)), _rfc1035{_cfg.query} {}

    ~rfc2136_ldns_discovery() {
        // Deliberately never lets deregistration failure escape a destructor
        // (or abort a normal shutdown), but a real failure here — the DELETE
        // UPDATE that's supposed to remove this node's A/AAAA record —
        // previously vanished with zero trace anywhere, making it
        // indistinguishable from success. Logged to stderr (visible via
        // `docker logs` for the containerized peer-discovery binaries) so a
        // real failure is at least observable, not just silently wrong.
        try {
            deregister_self();
        } catch (const std::exception& e) {
            std::cerr << "rfc2136_ldns_discovery: deregistration failed: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "rfc2136_ldns_discovery: deregistration failed: unknown exception\n";
        }
    }

    rfc2136_ldns_discovery(const rfc2136_ldns_discovery&) = delete;
    rfc2136_ldns_discovery& operator=(const rfc2136_ldns_discovery&) = delete;
    rfc2136_ldns_discovery(rfc2136_ldns_discovery&&) = delete;
    rfc2136_ldns_discovery& operator=(rfc2136_ldns_discovery&&) = delete;

    auto register_node(std::string /*self_id*/, std::string self_address) -> kythira::Future<void> {
        send_update(self_address, /*is_delete=*/false);
        _self_address = self_address;  // only set after successful registration
        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
        auto all = _rfc1035.find_peers(timeout).get();
        std::vector<peer_info<std::string, std::string>> filtered;
        filtered.reserve(all.size());
        for (auto& p : all) {
            if (p.address != _self_address) {
                filtered.push_back(std::move(p));
            }
        }
        return kythira::FutureFactory::makeFuture(std::move(filtered));
    }

private:
    struct ResolverDeleter {
        void operator()(ldns_resolver* r) const noexcept { ldns_resolver_free(r); }
    };
    struct PktDeleter {
        void operator()(ldns_pkt* p) const noexcept { ldns_pkt_free(p); }
    };
    struct RdfDeleter {
        void operator()(ldns_rdf* r) const noexcept { ldns_rdf_deep_free(r); }
    };
    struct RrListDeleter {
        void operator()(ldns_rr_list* l) const noexcept { ldns_rr_list_deep_free(l); }
    };
    struct RrDeleter {
        void operator()(ldns_rr* r) const noexcept { ldns_rr_free(r); }
    };

    // Builds a DNS UPDATE packet that adds (is_delete=false) or deletes (is_delete=true)
    // an A or AAAA record for the given address under _cfg.zone.
    void send_update(const std::string& addr, bool is_delete) {
        if (addr.empty()) {
            return;
        }

        // Chaos: simulate DNS UPDATE failure (throws) or skip silently (noop).
        fiu_do_on("raft/dns/rfc2136/send_update",
                  throw std::runtime_error("chaos: rfc2136 send_update"););
        fiu_do_on("raft/dns/rfc2136/send_update/noop", return;);

        std::unique_ptr<ldns_resolver, ResolverDeleter> res{ldns_resolver_new()};
        if (!res) {
            throw std::runtime_error("rfc2136_ldns_discovery: failed to create resolver");
        }

        ldns_resolver_set_port(res.get(), _cfg.query.port);

        {
            ldns_rdf* ns_rdf = nullptr;
            if (ldns_str2rdf_a(&ns_rdf, _cfg.query.server.c_str()) != LDNS_STATUS_OK) {
                ldns_str2rdf_aaaa(&ns_rdf, _cfg.query.server.c_str());
            }
            if (ns_rdf != nullptr) {
                ldns_resolver_push_nameserver(res.get(), ns_rdf);
                ldns_rdf_deep_free(ns_rdf);
            }
        }

        if (!_cfg.tsig_key_name.empty()) {
            ldns_resolver_set_tsig_keyname(res.get(), _cfg.tsig_key_name.c_str());
            ldns_resolver_set_tsig_algorithm(res.get(), _cfg.tsig_algorithm.c_str());
            ldns_resolver_set_tsig_keydata(res.get(), _cfg.tsig_key_base64.c_str());
        }

        const bool is_ipv4 = (std::strchr(addr.c_str(), '.') != nullptr);
        const ldns_rr_type rr_type = is_ipv4 ? LDNS_RR_TYPE_A : LDNS_RR_TYPE_AAAA;

        std::unique_ptr<ldns_rdf, RdfDeleter> zone_rdf{ldns_dname_new_frm_str(_cfg.zone.c_str())};
        if (!zone_rdf) {
            throw std::runtime_error("rfc2136_ldns_discovery: invalid zone name");
        }

        std::unique_ptr<ldns_rdf, RdfDeleter> owner_rdf{
            ldns_dname_new_frm_str(_cfg.query.shared_name.c_str())};
        if (!owner_rdf) {
            throw std::runtime_error("rfc2136_ldns_discovery: invalid shared_name");
        }

        std::unique_ptr<ldns_rr_list, RrListDeleter> update_list{ldns_rr_list_new()};
        if (!update_list) {
            throw std::runtime_error("rfc2136_ldns_discovery: failed to allocate RR list");
        }

        std::unique_ptr<ldns_rr, RrDeleter> rr{ldns_rr_new_frm_type(rr_type)};
        if (!rr) {
            throw std::runtime_error("rfc2136_ldns_discovery: failed to allocate RR");
        }

        // owner is consumed by ldns_rr_set_owner; release the unique_ptr before handing off
        ldns_rr_set_owner(rr.get(), ldns_rdf_clone(owner_rdf.get()));
        ldns_rr_set_ttl(rr.get(), is_delete ? 0 : _cfg.ttl);
        ldns_rr_set_type(rr.get(), rr_type);
        // RFC 2136 §2.5.4 "Delete An RR From An RRset": CLASS must be NONE,
        // TTL 0, with the RDATA of the exact RR to remove — NOT §2.5.2
        // "Delete An RRset" (CLASS ANY, empty RDATA), which deletes every
        // record at that owner name regardless of value. shared_name is one
        // DNS name shared by every node in the cluster (a round-robin-style
        // RRset with one A/AAAA record per node), so an RRset-wide delete
        // would silently wipe out every *other* node's registration too,
        // not just this node's own record, the first time any single node
        // deregistered — confirmed on real arm64 CI: after this exact bug
        // (CLASS ANY) briefly replaced the original one (missing CLASS
        // entirely, which defaulted to IN and made BIND9 reject the delete
        // outright), the survivors' peer count went from a stuck "2" to an
        // immediate "0", not the correct "1".
        ldns_rr_set_class(rr.get(), is_delete ? LDNS_RR_CLASS_NONE : LDNS_RR_CLASS_IN);

        {
            ldns_rdf* addr_rdf = nullptr;
            ldns_status st = is_ipv4 ? ldns_str2rdf_a(&addr_rdf, addr.c_str())
                                     : ldns_str2rdf_aaaa(&addr_rdf, addr.c_str());
            if (st != LDNS_STATUS_OK || (addr_rdf == nullptr)) {
                throw std::runtime_error("rfc2136_ldns_discovery: invalid address: " + addr);
            }
            ldns_rr_push_rdf(rr.get(), addr_rdf);
        }

        ldns_rr_list_push_rr(update_list.get(), rr.get());
        rr.release();  // ownership transferred to update_list

        std::unique_ptr<ldns_rr_list, RrListDeleter> prereq_list{ldns_rr_list_new()};
        std::unique_ptr<ldns_rr_list, RrListDeleter> additional_list{ldns_rr_list_new()};

        // ldns_update_pkt_new takes ownership of the three rr_list args; release them
        ldns_pkt* raw_pkt =
            ldns_update_pkt_new(ldns_rdf_clone(zone_rdf.get()), LDNS_RR_CLASS_IN, prereq_list.get(),
                                update_list.get(), additional_list.get());
        prereq_list.release();
        update_list.release();
        additional_list.release();

        if (raw_pkt == nullptr) {
            throw std::runtime_error("rfc2136_ldns_discovery: failed to build UPDATE packet");
        }
        std::unique_ptr<ldns_pkt, PktDeleter> pkt{raw_pkt};

        ldns_pkt_set_opcode(pkt.get(), LDNS_PACKET_UPDATE);
        ldns_pkt_set_random_id(pkt.get());

        if (!_cfg.tsig_key_name.empty()) {
            ldns_update_pkt_tsig_add(pkt.get(), res.get());
        }

        std::unique_ptr<ldns_pkt, PktDeleter> answer;
        {
            ldns_pkt* raw_answer = nullptr;
            ldns_status st = ldns_resolver_send_pkt(&raw_answer, res.get(), pkt.get());
            if (st != LDNS_STATUS_OK || (raw_answer == nullptr)) {
                throw std::runtime_error(std::string("rfc2136_ldns_discovery: send failed: ") +
                                         ldns_get_errorstr_by_id(st));
            }
            answer.reset(raw_answer);
        }

        if (ldns_pkt_get_rcode(answer.get()) != LDNS_RCODE_NOERROR) {
            throw std::runtime_error(
                "rfc2136_ldns_discovery: DNS UPDATE returned non-NOERROR rcode");
        }
    }

    void deregister_self() {
        if (!_self_address.empty()) {
            send_update(_self_address, /*is_delete=*/true);
        }
    }

    config _cfg;
    rfc1035_peer_discovery _rfc1035;
    std::string _self_address;
};

static_assert(peer_discovery<rfc2136_ldns_discovery, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_LDNS
