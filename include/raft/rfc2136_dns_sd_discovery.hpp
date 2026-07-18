#pragma once

#include <raft/fault_injection.hpp>
#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#ifdef KYTHIRA_HAS_LDNS

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <ldns/ldns.h>

namespace kythira {

// DNS-SD peer discovery over unicast DNS using RFC 2136 dynamic updates.
//
// Registration publishes PTR, SRV, and TXT records for the service instance.
// The TXT record carries a "fresh_until=<epoch_seconds>" field; a background
// thread renews it every freshness_interval/2 so that stale entries from
// crashed nodes (whose destructor never runs) are filtered out by find_peers.
class rfc2136_dns_sd_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;  // "hostname:port"

    struct config {
        std::string server;  // DNS server IP
        uint16_t port{53};
        std::string zone;                           // e.g. "example.local."
        std::string service_domain;                 // e.g. "cluster.example.local."
        std::string service_type{"_kythira._tcp"};  // e.g. "_kythira-test._tcp"
        uint32_t ttl{30};
        std::chrono::seconds freshness_interval{60};
        std::string tsig_key_name;
        std::string tsig_algorithm{"hmac-sha256."};
        std::string tsig_key_base64;
    };

    explicit rfc2136_dns_sd_discovery(config cfg) : _cfg(std::move(cfg)) {}

    ~rfc2136_dns_sd_discovery() {
        stop_fresher();
        try {
            send_deregister();
        } catch (...) {
        }
    }

    rfc2136_dns_sd_discovery(const rfc2136_dns_sd_discovery&) = delete;
    rfc2136_dns_sd_discovery& operator=(const rfc2136_dns_sd_discovery&) = delete;
    rfc2136_dns_sd_discovery(rfc2136_dns_sd_discovery&&) = delete;
    rfc2136_dns_sd_discovery& operator=(rfc2136_dns_sd_discovery&&) = delete;

    auto register_node(std::string node_id, std::string self_addr) -> kythira::Future<void> {
        _self_node_id = std::move(node_id);
        _self_addr = std::move(self_addr);
        send_register(fresh_until_epoch());
        start_fresher();
        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
        std::vector<peer_info<std::string, std::string>> results;

        auto res = make_resolver(timeout);
        if (!res) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        // Step 1: browse PTR records at _service_type._service_domain
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        auto browse_rdf = rdf_from_name(browse);
        if (!browse_rdf) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        auto ptr_pkt = pkt_query(res.get(), browse_rdf.get(), LDNS_RR_TYPE_PTR);
        if (!ptr_pkt) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        ldns_rr_list* ptr_answer = ldns_pkt_answer(ptr_pkt.get());
        if (ptr_answer == nullptr) {
            return kythira::FutureFactory::makeFuture(std::move(results));
        }

        const int64_t now = epoch_seconds_now();
        const std::size_t n_ptr = ldns_rr_list_rr_count(ptr_answer);

        for (std::size_t i = 0; i < n_ptr; ++i) {
            ldns_rr* ptr_rr = ldns_rr_list_rr(ptr_answer, i);
            if ((ptr_rr == nullptr) || ldns_rr_get_type(ptr_rr) != LDNS_RR_TYPE_PTR) {
                continue;
            }
            ldns_rdf* inst_rdf = ldns_rr_rdf(ptr_rr, 0);
            if (inst_rdf == nullptr) {
                continue;
            }

            // Extract node_id = label before first dot in instance name
            char* inst_cstr = ldns_rdf2str(inst_rdf);
            if (inst_cstr == nullptr) {
                continue;
            }
            const std::string instance{inst_cstr};
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            free(inst_cstr);

            const auto dot = instance.find('.');
            const std::string found_id =
                (dot != std::string::npos) ? instance.substr(0, dot) : instance;

            if (found_id == _self_node_id) {
                continue;  // skip self
            }

            // Step 2: check TXT freshness at instance name
            auto inst_name_rdf = rdf_from_name(instance);
            if (!inst_name_rdf) {
                continue;
            }

            auto txt_pkt = pkt_query(res.get(), inst_name_rdf.get(), LDNS_RR_TYPE_TXT);
            int64_t fresh_until = 0;
            if (txt_pkt) {
                fresh_until = parse_fresh_until(ldns_pkt_answer(txt_pkt.get()));
            }
            if (fresh_until <= now) {
                continue;  // stale or no TXT
            }

            // Step 3: resolve SRV at instance name → (port, target)
            auto srv_pkt = pkt_query(res.get(), inst_name_rdf.get(), LDNS_RR_TYPE_SRV);
            if (!srv_pkt) {
                continue;
            }
            ldns_rr_list* srv_answer = ldns_pkt_answer(srv_pkt.get());
            if (srv_answer == nullptr) {
                continue;
            }
            const std::size_t n_srv = ldns_rr_list_rr_count(srv_answer);
            for (std::size_t j = 0; j < n_srv; ++j) {
                ldns_rr* srv_rr = ldns_rr_list_rr(srv_answer, j);
                if ((srv_rr == nullptr) || ldns_rr_get_type(srv_rr) != LDNS_RR_TYPE_SRV) {
                    continue;
                }
                ldns_rdf* port_rdf = ldns_rr_rdf(srv_rr, 2);
                ldns_rdf* target_rdf = ldns_rr_rdf(srv_rr, 3);
                if ((port_rdf == nullptr) || (target_rdf == nullptr)) {
                    continue;
                }
                const uint16_t srv_port = ldns_rdf2native_int16(port_rdf);
                char* target_cstr = ldns_rdf2str(target_rdf);
                if (target_cstr == nullptr) {
                    continue;
                }
                std::string target{target_cstr};
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
                free(target_cstr);
                // Strip trailing dot
                if (!target.empty() && target.back() == '.') {
                    target.pop_back();
                }
                const std::string addr = target + ":" + std::to_string(srv_port);
                results.push_back({found_id, addr});
            }
        }

        return kythira::FutureFactory::makeFuture(std::move(results));
    }

private:
    // ── RAII wrappers ─────────────────────────────────────────────────────────

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

    // ── Helpers ───────────────────────────────────────────────────────────────

    static int64_t epoch_seconds_now() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] int64_t fresh_until_epoch() const {
        return epoch_seconds_now() + _cfg.freshness_interval.count();
    }

    static RdfPtr rdf_from_name(const std::string& name) {
        return RdfPtr{ldns_dname_new_frm_str(name.c_str())};
    }

    [[nodiscard]] ResPtr make_resolver(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) const {
        ResPtr res{ldns_resolver_new()};
        if (!res) {
            return nullptr;
        }
        ldns_resolver_set_port(res.get(), _cfg.port);

        ldns_rdf* ns_rdf = nullptr;
        if (ldns_str2rdf_a(&ns_rdf, _cfg.server.c_str()) != LDNS_STATUS_OK) {
            ldns_str2rdf_aaaa(&ns_rdf, _cfg.server.c_str());
        }
        if (ns_rdf != nullptr) {
            ldns_resolver_push_nameserver(res.get(), ns_rdf);
            ldns_rdf_deep_free(ns_rdf);
        }

        struct timeval tv{};
        tv.tv_sec = timeout.count() / 1000;
        tv.tv_usec = static_cast<int>((timeout.count() % 1000) * 1000);
        ldns_resolver_set_timeout(res.get(), tv);

        if (!_cfg.tsig_key_name.empty()) {
            ldns_resolver_set_tsig_keyname(res.get(), _cfg.tsig_key_name.c_str());
            ldns_resolver_set_tsig_algorithm(res.get(), _cfg.tsig_algorithm.c_str());
            ldns_resolver_set_tsig_keydata(res.get(), _cfg.tsig_key_base64.c_str());
        }

        return res;
    }

    PktPtr pkt_query(ldns_resolver* res, ldns_rdf* qname, ldns_rr_type qtype) const {
        return PktPtr{ldns_resolver_query(res, qname, qtype, LDNS_RR_CLASS_IN, LDNS_RD)};
    }

    // Parses "\"fresh_until=<epoch>\"" from a TXT RR list; returns 0 if absent.
    static int64_t parse_fresh_until(ldns_rr_list* list) {
        if (list == nullptr) {
            return 0;
        }
        const std::size_t n = ldns_rr_list_rr_count(list);
        for (std::size_t i = 0; i < n; ++i) {
            ldns_rr* rr = ldns_rr_list_rr(list, i);
            if ((rr == nullptr) || ldns_rr_get_type(rr) != LDNS_RR_TYPE_TXT) {
                continue;
            }
            ldns_rdf* rdata = ldns_rr_rdf(rr, 0);
            if (rdata == nullptr) {
                continue;
            }
            char* s = ldns_rdf2str(rdata);
            if (s == nullptr) {
                continue;
            }
            std::string txt{s};
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            free(s);
            // Strip surrounding quotes from presentation format
            if (txt.size() >= 2 && txt.front() == '"' && txt.back() == '"') {
                txt = txt.substr(1, txt.size() - 2);
            }
            const auto kw = txt.find("fresh_until=");
            if (kw != std::string::npos) {
                try {
                    return std::stoll(txt.substr(kw + 12));
                } catch (...) {
                }
            }
        }
        return 0;
    }

    // ── DNS UPDATE helpers ────────────────────────────────────────────────────

    // Sends a single-record RFC 2136 UPDATE (add or delete).
    // For delete-rrset (is_delete=true, rr_rdata=""), sets class=ANY.
    void send_update_rr(const RrPtr& rr) const {
        fiu_do_on("raft/dns/rfc2136_dns_sd/send_update_rr",
                  throw std::runtime_error("chaos: rfc2136_dns_sd send_update_rr"););
        fiu_do_on("raft/dns/rfc2136_dns_sd/send_update_rr/noop", return;);
        auto res = make_resolver();
        if (!res) {
            throw std::runtime_error("rfc2136_dns_sd_discovery: failed to create resolver");
        }

        auto zone_rdf = rdf_from_name(_cfg.zone);
        if (!zone_rdf) {
            throw std::runtime_error("rfc2136_dns_sd_discovery: invalid zone name");
        }

        RrListPtr update_list{ldns_rr_list_new()};
        RrListPtr prereq_list{ldns_rr_list_new()};
        RrListPtr additional_list{ldns_rr_list_new()};
        if (!update_list || !prereq_list || !additional_list) {
            throw std::runtime_error("rfc2136_dns_sd_discovery: failed to allocate RR lists");
        }

        // ldns_update_pkt_new takes ownership of the three lists; clone rr so
        // the caller keeps its RrPtr valid.
        ldns_rr_list_push_rr(update_list.get(), ldns_rr_clone(rr.get()));

        ldns_pkt* raw_pkt =
            ldns_update_pkt_new(ldns_rdf_clone(zone_rdf.get()), LDNS_RR_CLASS_IN, prereq_list.get(),
                                update_list.get(), additional_list.get());
        prereq_list.release();
        update_list.release();
        additional_list.release();

        if (raw_pkt == nullptr) {
            throw std::runtime_error("rfc2136_dns_sd_discovery: failed to build UPDATE packet");
        }
        PktPtr pkt{raw_pkt};
        ldns_pkt_set_opcode(pkt.get(), LDNS_PACKET_UPDATE);
        ldns_pkt_set_random_id(pkt.get());

        if (!_cfg.tsig_key_name.empty()) {
            ldns_update_pkt_tsig_add(pkt.get(), res.get());
        }

        ldns_pkt* raw_answer = nullptr;
        const ldns_status st = ldns_resolver_send_pkt(&raw_answer, res.get(), pkt.get());
        PktPtr answer{raw_answer};

        if (st != LDNS_STATUS_OK || (raw_answer == nullptr)) {
            throw std::runtime_error(std::string("rfc2136_dns_sd_discovery: send failed: ") +
                                     ldns_get_errorstr_by_id(st));
        }
        if (ldns_pkt_get_rcode(answer.get()) != LDNS_RCODE_NOERROR) {
            throw std::runtime_error(
                "rfc2136_dns_sd_discovery: DNS UPDATE returned non-NOERROR rcode");
        }
    }

    // Builds a PTR add RR: browse_name → instance_name
    [[nodiscard]] RrPtr make_ptr_add() const {
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        const std::string instance = _self_node_id + "." + browse;
        const std::string rr_text = browse + " " + std::to_string(_cfg.ttl) + " IN PTR " + instance;
        RrPtr rr{nullptr};
        ldns_rr* raw = nullptr;
        ldns_rr_new_frm_str(&raw, rr_text.c_str(), _cfg.ttl, nullptr, nullptr);
        rr.reset(raw);
        return rr;
    }

    // Builds a PTR delete RR (specific rdata delete): browse_name → instance_name
    [[nodiscard]] RrPtr make_ptr_del() const {
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        const std::string instance = _self_node_id + "." + browse;
        const std::string rr_text = browse + " 0 IN PTR " + instance;
        RrPtr rr{nullptr};
        ldns_rr* raw = nullptr;
        ldns_rr_new_frm_str(&raw, rr_text.c_str(), 0, nullptr, nullptr);
        rr.reset(raw);
        return rr;
    }

    // Builds an SRV add RR: instance_name → target:port
    [[nodiscard]] RrPtr make_srv_add() const {
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        const std::string instance = _self_node_id + "." + browse;
        // Parse "hostname:port" from self_addr
        const auto colon = _self_addr.rfind(':');
        const std::string hostname = _self_addr.substr(0, colon);
        const std::string port_str = _self_addr.substr(colon + 1);
        const std::string target = hostname + "." + _cfg.service_domain;
        const std::string rr_text =
            instance + " " + std::to_string(_cfg.ttl) + " IN SRV 0 0 " + port_str + " " + target;
        RrPtr rr{nullptr};
        ldns_rr* raw = nullptr;
        ldns_rr_new_frm_str(&raw, rr_text.c_str(), _cfg.ttl, nullptr, nullptr);
        rr.reset(raw);
        return rr;
    }

    // Builds an SRV rrset-delete RR (class=ANY, TTL=0, no rdata)
    [[nodiscard]] RrPtr make_srv_del() const {
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        const std::string instance = _self_node_id + "." + browse;
        RrPtr rr{ldns_rr_new()};
        ldns_rr_set_owner(rr.get(), ldns_dname_new_frm_str(instance.c_str()));
        ldns_rr_set_type(rr.get(), LDNS_RR_TYPE_SRV);
        ldns_rr_set_class(rr.get(), LDNS_RR_CLASS_ANY);
        ldns_rr_set_ttl(rr.get(), 0);
        return rr;
    }

    // Builds a TXT add RR with freshness: instance_name → "fresh_until=<epoch>"
    [[nodiscard]] RrPtr make_txt_add(int64_t fresh_until) const {
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        const std::string instance = _self_node_id + "." + browse;
        const std::string rr_text = instance + " " + std::to_string(_cfg.ttl) +
                                    " IN TXT \"fresh_until=" + std::to_string(fresh_until) + "\"";
        RrPtr rr{nullptr};
        ldns_rr* raw = nullptr;
        ldns_rr_new_frm_str(&raw, rr_text.c_str(), _cfg.ttl, nullptr, nullptr);
        rr.reset(raw);
        return rr;
    }

    // Builds a TXT rrset-delete RR (class=ANY, TTL=0, no rdata)
    [[nodiscard]] RrPtr make_txt_del() const {
        const std::string browse = _cfg.service_type + "." + _cfg.service_domain;
        const std::string instance = _self_node_id + "." + browse;
        RrPtr rr{ldns_rr_new()};
        ldns_rr_set_owner(rr.get(), ldns_dname_new_frm_str(instance.c_str()));
        ldns_rr_set_type(rr.get(), LDNS_RR_TYPE_TXT);
        ldns_rr_set_class(rr.get(), LDNS_RR_CLASS_ANY);
        ldns_rr_set_ttl(rr.get(), 0);
        return rr;
    }

    // ── Registration / deregistration ─────────────────────────────────────────

    void send_register(int64_t fresh_until) {
        send_update_rr(make_ptr_add());
        send_update_rr(make_srv_add());
        // Always delete-then-add TXT to avoid accumulating stale copies.
        send_update_rr(make_txt_del());
        send_update_rr(make_txt_add(fresh_until));
    }

    void send_deregister() {
        if (_self_node_id.empty()) {
            return;
        }
        send_update_rr(make_ptr_del());
        send_update_rr(make_srv_del());
        send_update_rr(make_txt_del());
    }

    // ── Fresher thread ────────────────────────────────────────────────────────

    void start_fresher() {
        _running = true;
        _fresher = std::thread([this] { fresher_loop(); });
    }

    void stop_fresher() {
        {
            std::lock_guard<std::mutex> lk(_mu);
            _running = false;
        }
        _cv.notify_all();
        if (_fresher.joinable()) {
            _fresher.join();
        }
    }

    void fresher_loop() {
        const auto interval = _cfg.freshness_interval / 2;
        std::unique_lock<std::mutex> lk(_mu);
        while (_running) {
            if (_cv.wait_for(lk, interval, [this] { return !_running; })) {
                break;
            }
            try {
                send_update_rr(make_txt_del());
                send_update_rr(make_txt_add(fresh_until_epoch()));
            } catch (...) {
            }
        }
    }

    config _cfg;
    std::string _self_node_id;
    std::string _self_addr;
    bool _running{false};
    std::mutex _mu;
    std::condition_variable _cv;
    std::thread _fresher;
};

static_assert(peer_discovery<rfc2136_dns_sd_discovery, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_LDNS
