#pragma once

#include <raft/rfc6763_peer_discovery.hpp>

#ifdef KYTHIRA_HAS_LDNS

#include <raft/fault_injection.hpp>
#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ldns/ldns.h>

namespace kythira {

// Full peer_discovery: registers four DNS records per node (PTR, instance SRV,
// cluster-level SRV, domain-level SRV) via RFC 2136 dynamic update, and
// delegates find_peers to an embedded rfc6763_peer_discovery, filtering out
// this node's own address.
class rfc6763_ldns_peer_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    struct config {
        rfc6763_peer_discovery::config query;  // DNS server, port, and cluster-level service_name
        std::string zone;                      // cluster zone, e.g. "cluster.example.com."
        std::string domain_service_name;  // domain-level service, e.g. "_raft._tcp.example.com."
        std::string domain_zone;          // domain zone, e.g. "example.com."
        uint16_t srv_priority{10};
        uint16_t srv_weight{0};
        uint32_t ttl{120};
        std::string tsig_key_name;
        std::string tsig_algorithm{"hmac-sha256."};
        std::string tsig_key_base64;
    };

    explicit rfc6763_ldns_peer_discovery(config cfg) : _cfg(std::move(cfg)), _rfc6763{_cfg.query} {}

    ~rfc6763_ldns_peer_discovery() {
        try {
            deregister_self();
        } catch (...) {
        }
    }

    rfc6763_ldns_peer_discovery(const rfc6763_ldns_peer_discovery&) = delete;
    rfc6763_ldns_peer_discovery& operator=(const rfc6763_ldns_peer_discovery&) = delete;
    rfc6763_ldns_peer_discovery(rfc6763_ldns_peer_discovery&&) = delete;
    rfc6763_ldns_peer_discovery& operator=(rfc6763_ldns_peer_discovery&&) = delete;

    auto register_node(std::string self_id, std::string self_address) -> kythira::Future<void> {
        auto [host, port] = parse_self_address(self_address);
        const std::string instance_name = self_id + "." + _cfg.query.service_name;

        send_pkt(build_cluster_update(instance_name, host, port, /*add=*/true));
        send_pkt(build_domain_update(host, port, /*add=*/true));

        // Registration state (used both for find_peers() self-filtering and to
        // decide whether the destructor's deregister_self() has anything to do)
        // is only committed once both UPDATEs succeed. Committing eagerly would
        // leave the destructor attempting a real network DELETE — with no
        // configurable timeout — after a registration that never fully
        // succeeded, mirroring the same invariant in rfc2136_ldns_discovery.
        _self_id = std::move(self_id);
        _instance_name = instance_name;
        _self_host = std::move(host);
        _self_port = port;
        _self_address = std::move(self_address);

        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
        auto all = _rfc6763.find_peers(timeout).get();
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

    using ResPtr = std::unique_ptr<ldns_resolver, ResolverDeleter>;
    using PktPtr = std::unique_ptr<ldns_pkt, PktDeleter>;
    using RdfPtr = std::unique_ptr<ldns_rdf, RdfDeleter>;
    using RrListPtr = std::unique_ptr<ldns_rr_list, RrListDeleter>;
    using RrPtr = std::unique_ptr<ldns_rr, RrDeleter>;

    static std::pair<std::string, uint16_t> parse_self_address(const std::string& addr) {
        auto colon = addr.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon == addr.size() - 1) {
            throw std::invalid_argument("rfc6763_ldns_peer_discovery: malformed self_address: " +
                                        addr);
        }
        std::string host = addr.substr(0, colon);
        std::string port_str = addr.substr(colon + 1);
        int port = 0;
        try {
            std::size_t idx = 0;
            port = std::stoi(port_str, &idx);
            if (idx != port_str.size()) {
                throw std::invalid_argument("");
            }
        } catch (...) {
            throw std::invalid_argument(
                "rfc6763_ldns_peer_discovery: invalid port in self_address: " + addr);
        }
        if (port < 1 || port > 65535) {
            throw std::invalid_argument(
                "rfc6763_ldns_peer_discovery: port out of range in self_address: " + addr);
        }
        return {std::move(host), static_cast<uint16_t>(port)};
    }

    // Builds a DELETE-specific-RR-compatible RR: owner/type/rdata are always
    // set so that deletion removes only this node's entry from a shared RRset
    // (RFC 2136 §2.5.4), never the whole RRset.
    [[nodiscard]] RrPtr make_rr(const std::string& owner, const std::string& type_str,
                                const std::string& rdata, bool add) const {
        const uint32_t ttl = add ? _cfg.ttl : 0;
        const std::string rr_text =
            owner + " " + std::to_string(ttl) + " IN " + type_str + " " + rdata;
        ldns_rr* raw = nullptr;
        ldns_rr_new_frm_str(&raw, rr_text.c_str(), ttl, nullptr, nullptr);
        return RrPtr{raw};
    }

    [[nodiscard]] RrPtr make_ptr(const std::string& instance_name, bool add) const {
        return make_rr(_cfg.query.service_name, "PTR", instance_name, add);
    }

    [[nodiscard]] RrPtr make_srv(const std::string& owner, const std::string& host, uint16_t port,
                                 bool add) const {
        const std::string rdata = std::to_string(_cfg.srv_priority) + " " +
                                  std::to_string(_cfg.srv_weight) + " " + std::to_string(port) +
                                  " " + host;
        return make_rr(owner, "SRV", rdata, add);
    }

    // Builds the cluster-zone UPDATE packet (PTR + instance SRV + cluster-level SRV).
    // Takes the instance/host/port explicitly rather than reading member state so
    // it can be called from register_node() before registration state is committed.
    [[nodiscard]] PktPtr build_cluster_update(const std::string& instance_name,
                                              const std::string& host, uint16_t port,
                                              bool add) const {
        std::vector<RrPtr> rrs;
        rrs.push_back(make_ptr(instance_name, add));
        rrs.push_back(make_srv(instance_name, host, port, add));
        rrs.push_back(make_srv(_cfg.query.service_name, host, port, add));
        return build_update(_cfg.zone, std::move(rrs));
    }

    // Builds the domain-zone UPDATE packet (domain-level SRV only).
    [[nodiscard]] PktPtr build_domain_update(const std::string& host, uint16_t port,
                                             bool add) const {
        std::vector<RrPtr> rrs;
        rrs.push_back(make_srv(_cfg.domain_service_name, host, port, add));
        return build_update(_cfg.domain_zone, std::move(rrs));
    }

    [[nodiscard]] PktPtr build_update(const std::string& zone, std::vector<RrPtr> rrs) const {
        RdfPtr zone_rdf{ldns_dname_new_frm_str(zone.c_str())};
        if (!zone_rdf) {
            throw std::runtime_error("rfc6763_ldns_peer_discovery: invalid zone name");
        }

        RrListPtr update_list{ldns_rr_list_new()};
        RrListPtr prereq_list{ldns_rr_list_new()};
        RrListPtr additional_list{ldns_rr_list_new()};
        if (!update_list || !prereq_list || !additional_list) {
            throw std::runtime_error("rfc6763_ldns_peer_discovery: failed to allocate RR lists");
        }

        for (auto& rr : rrs) {
            if (!rr) {
                throw std::runtime_error("rfc6763_ldns_peer_discovery: failed to build RR");
            }
            ldns_rr_list_push_rr(update_list.get(), rr.get());
            rr.release();  // ownership transferred to update_list
        }

        ldns_pkt* raw_pkt =
            ldns_update_pkt_new(ldns_rdf_clone(zone_rdf.get()), LDNS_RR_CLASS_IN, prereq_list.get(),
                                update_list.get(), additional_list.get());
        prereq_list.release();
        update_list.release();
        additional_list.release();

        if (raw_pkt == nullptr) {
            throw std::runtime_error("rfc6763_ldns_peer_discovery: failed to build UPDATE packet");
        }
        PktPtr pkt{raw_pkt};
        ldns_pkt_set_opcode(pkt.get(), LDNS_PACKET_UPDATE);
        ldns_pkt_set_random_id(pkt.get());
        return pkt;
    }

    [[nodiscard]] ResPtr make_resolver() const {
        ResPtr res{ldns_resolver_new()};
        if (!res) {
            return nullptr;
        }
        ldns_resolver_set_port(res.get(), _cfg.query.port);

        ldns_rdf* ns_rdf = nullptr;
        if (ldns_str2rdf_a(&ns_rdf, _cfg.query.server.c_str()) != LDNS_STATUS_OK) {
            ldns_str2rdf_aaaa(&ns_rdf, _cfg.query.server.c_str());
        }
        if (ns_rdf != nullptr) {
            ldns_resolver_push_nameserver(res.get(), ns_rdf);
            ldns_rdf_deep_free(ns_rdf);
        }

        if (!_cfg.tsig_key_name.empty()) {
            ldns_resolver_set_tsig_keyname(res.get(), _cfg.tsig_key_name.c_str());
            ldns_resolver_set_tsig_algorithm(res.get(), _cfg.tsig_algorithm.c_str());
            ldns_resolver_set_tsig_keydata(res.get(), _cfg.tsig_key_base64.c_str());
        }

        return res;
    }

    void send_pkt(PktPtr pkt) const {
        // Chaos: simulate DNS UPDATE failure (throws) or skip silently (noop).
        fiu_do_on("raft/dns/rfc6763_ldns/send_update",
                  throw std::runtime_error("chaos: rfc6763_ldns send_update"););
        fiu_do_on("raft/dns/rfc6763_ldns/send_update/noop", return;);

        auto res = make_resolver();
        if (!res) {
            throw std::runtime_error("rfc6763_ldns_peer_discovery: failed to create resolver");
        }

        if (!_cfg.tsig_key_name.empty()) {
            ldns_update_pkt_tsig_add(pkt.get(), res.get());
        }

        ldns_pkt* raw_answer = nullptr;
        ldns_status st = ldns_resolver_send_pkt(&raw_answer, res.get(), pkt.get());
        PktPtr answer{raw_answer};

        if (st != LDNS_STATUS_OK || (raw_answer == nullptr)) {
            throw std::runtime_error(std::string("rfc6763_ldns_peer_discovery: send failed: ") +
                                     ldns_get_errorstr_by_id(st));
        }
        if (ldns_pkt_get_rcode(answer.get()) != LDNS_RCODE_NOERROR) {
            throw std::runtime_error(
                "rfc6763_ldns_peer_discovery: DNS UPDATE returned non-NOERROR rcode");
        }
    }

    void deregister_self() {
        if (_self_id.empty()) {
            return;
        }
        send_pkt(build_cluster_update(_instance_name, _self_host, _self_port, /*add=*/false));
        send_pkt(build_domain_update(_self_host, _self_port, /*add=*/false));
    }

    config _cfg;
    rfc6763_peer_discovery _rfc6763;
    std::string _self_id;        // instance label (e.g. "node1")
    std::string _instance_name;  // self_id + "." + service_name
    std::string _self_host;      // hostname parsed from self_address
    uint16_t _self_port{0};      // port parsed from self_address
    std::string _self_address;   // stored for find_peers() filtering
};

static_assert(peer_discovery<rfc6763_ldns_peer_discovery, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_LDNS
