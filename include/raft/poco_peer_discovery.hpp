#pragma once

#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef KYTHIRA_HAS_POCO_DNSSD

#include <Poco/DNSSD/Avahi/Avahi.h>
#include <Poco/DNSSD/DNSSDBrowser.h>
#include <Poco/DNSSD/DNSSDResponder.h>
#include <Poco/DNSSD/Service.h>
#include <Poco/Delegate.h>

namespace kythira {
namespace detail {

// Base class whose constructor calls initializeDNSSD() exactly once before any
// DNSSDResponder is constructed.  Base classes are initialised before member objects,
// guaranteeing the factory is registered by the time _responder is created.
struct PocoDnssdInit {
    PocoDnssdInit() {
        static std::once_flag s_once;
        std::call_once(s_once, [] { Poco::DNSSD::initializeDNSSD(); });
    }
};

}  // namespace detail

// DNS-SD peer discovery with TXT-record freshness.
//
// Each registered node embeds "fresh_until=<epoch_seconds>" in its TXT record.
// A background thread wakes every freshness_interval/2 to re-register the local
// node (renewing the timestamp) and to browse for stale foreign entries.
// Stale entries are excluded from find_peers() results.
//
// Limitation: Poco DNSSD does not allow one node to remove another node's
// registration.  Stale entries are pruned locally; they linger in DNS until
// the mDNS TTL expires or the original publisher re-registers / goes away.
class poco_peer_discovery : private detail::PocoDnssdInit {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    // TXT record key that carries the freshness deadline (decimal epoch seconds).
    static constexpr const char* k_fresh_until_key = "fresh_until";

    struct config {
        std::string service_type{"_raft._tcp"};
        std::string domain{};
        std::chrono::seconds freshness_interval{600};  // 10 minutes
    };

    explicit poco_peer_discovery(config cfg) : _cfg(std::move(cfg)) {}

    ~poco_peer_discovery() {
        // Stop the fresher thread before touching shared Poco state.
        {
            std::lock_guard<std::mutex> lk{_stop_mutex};
            _stop = true;
        }
        _stop_cv.notify_all();
        if (_fresher.joinable()) {
            _fresher.join();
        }
        try {
            if (_registered) {
                _responder.unregisterService(_service_handle);
            }
            if (_started) {
                _responder.stop();
            }
        } catch (...) {
        }
    }

    poco_peer_discovery(const poco_peer_discovery&) = delete;
    poco_peer_discovery& operator=(const poco_peer_discovery&) = delete;
    poco_peer_discovery(poco_peer_discovery&&) = delete;
    poco_peer_discovery& operator=(poco_peer_discovery&&) = delete;

    auto register_node(std::string self_id, std::string self_address) -> kythira::Future<void> {
        auto [host, port] = parse_address(self_address);

        ensure_started();
        _self_id = std::move(self_id);
        _self_address = std::move(self_address);
        _host = std::move(host);
        _port = port;

        do_register();  // throws on failure
        _registered = true;
        start_fresher();
        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
        std::vector<peer_info<std::string, std::string>> results;
        try {
            ensure_started();
            std::lock_guard<std::mutex> browse_lk{_browse_mutex};
            do_browse(timeout, results);
        } catch (...) {
        }
        std::erase_if(results, [this](const auto& p) { return p.address == _self_address; });
        return kythira::FutureFactory::makeFuture(std::move(results));
    }

private:
    using RegistrationArgs = Poco::DNSSD::DNSSDResponder::ServiceEventArgs;
    using RegistrationFailedArgs = Poco::DNSSD::DNSSDResponder::ErrorEventArgs;
    using BrowserArgs = Poco::DNSSD::DNSSDBrowser::ServiceEventArgs;

    // ── Helpers ────────────────────────────────────────────────────────────────

    static int64_t epoch_seconds_now() {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
    }

    static std::pair<std::string, int> parse_address(const std::string& addr) {
        const auto colon = addr.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == addr.size()) {
            throw std::invalid_argument(
                "poco_peer_discovery: self_address must be 'host:port', got: " + addr);
        }
        const std::string host = addr.substr(0, colon);
        const std::string port_str = addr.substr(colon + 1);
        int port_int = 0;
        {
            std::size_t pos = 0;
            bool bad = false;
            try {
                port_int = std::stoi(port_str, &pos);
                bad = (pos != port_str.size());
            } catch (...) {
                bad = true;
            }
            if (bad) {
                throw std::invalid_argument("poco_peer_discovery: invalid port in self_address: " +
                                            addr);
            }
        }
        if (port_int < 1 || port_int > 65535) {
            throw std::invalid_argument("poco_peer_discovery: port out of range [1, 65535] in: " +
                                        addr);
        }
        return {host, port_int};
    }

    void ensure_started() {
        if (!_started) {
            _responder.start();
            _started = true;
        }
    }

    Poco::DNSSD::Service::Properties make_properties() const {
        Poco::DNSSD::Service::Properties props;
        props.add(k_fresh_until_key,
                  std::to_string(epoch_seconds_now() + _cfg.freshness_interval.count()));
        return props;
    }

    // Register (or re-register) the local node with a fresh timestamp.
    // Caller must not hold _reg_mutex.
    void do_register() {
        Poco::DNSSD::Service service{0,
                                     _self_id,
                                     "",
                                     _cfg.service_type,
                                     _cfg.domain,
                                     _host,
                                     static_cast<Poco::UInt16>(_port),
                                     make_properties()};

        std::unique_lock<std::mutex> lock{_reg_mutex};
        _reg_done = false;
        _reg_error.clear();

        _responder.serviceRegistered +=
            Poco::delegate(this, &poco_peer_discovery::on_service_registered);
        _responder.serviceRegistrationFailed +=
            Poco::delegate(this, &poco_peer_discovery::on_service_registration_failed);

        _service_handle = _responder.registerService(service, 0);

        const bool signalled =
            _reg_cv.wait_for(lock, std::chrono::seconds{10}, [this] { return _reg_done; });

        _responder.serviceRegistered -=
            Poco::delegate(this, &poco_peer_discovery::on_service_registered);
        _responder.serviceRegistrationFailed -=
            Poco::delegate(this, &poco_peer_discovery::on_service_registration_failed);

        if (!signalled) {
            throw std::runtime_error("poco_peer_discovery: registration timed out");
        }
        if (!_reg_error.empty()) {
            throw std::runtime_error("poco_peer_discovery: registration failed: " + _reg_error);
        }
    }

    // Browse and resolve services, appending fresh peers to out.
    // Caller must hold _browse_mutex.
    void do_browse(std::chrono::milliseconds timeout,
                   std::vector<peer_info<std::string, std::string>>& out) {
        _browse_now = epoch_seconds_now();
        _browse_out = &out;

        Poco::DNSSD::DNSSDBrowser& browser = _responder.browser();
        browser.serviceFound += Poco::delegate(this, &poco_peer_discovery::on_service_found);
        browser.serviceResolved += Poco::delegate(this, &poco_peer_discovery::on_service_resolved);

        auto browse_handle = browser.browse(_cfg.service_type, _cfg.domain);
        std::this_thread::sleep_for(timeout);
        browser.cancel(browse_handle);

        browser.serviceFound -= Poco::delegate(this, &poco_peer_discovery::on_service_found);
        browser.serviceResolved -= Poco::delegate(this, &poco_peer_discovery::on_service_resolved);

        _browse_out = nullptr;
    }

    void start_fresher() {
        _fresher = std::thread([this] { fresher_loop(); });
    }

    void fresher_loop() {
        const auto half = _cfg.freshness_interval / 2;

        while (true) {
            {
                std::unique_lock<std::mutex> lk{_stop_mutex};
                _stop_cv.wait_for(lk, half, [this] { return _stop; });
                if (_stop) {
                    break;
                }
            }

            // Renew own registration with an updated fresh_until timestamp.
            // Unregister before re-registering so Avahi replaces the TXT record.
            try {
                _responder.unregisterService(_service_handle);
                do_register();
            } catch (...) {
                // Daemon unavailable; will retry next cycle.
            }

            // Browse to find and record stale peers.  Stale entries are excluded
            // from results by on_service_resolved; also collect them in the
            // eviction set so find_peers() double-filters even between browses.
            try {
                std::vector<peer_info<std::string, std::string>> found;
                {
                    std::lock_guard<std::mutex> browse_lk{_browse_mutex};
                    do_browse(std::chrono::seconds{2}, found);
                }
                // Any peer not returned by do_browse (due to stale TXT) but
                // visible on the network goes into the eviction set.  We detect
                // this by comparing a raw browse against do_browse's filtered
                // output — omitted here for simplicity; the TXT-based filter in
                // on_service_resolved is the authoritative freshness gate.
            } catch (...) {
            }
        }
    }

    // ── Event handlers ─────────────────────────────────────────────────────────

    void on_service_registered(const void*, const RegistrationArgs&) {
        std::lock_guard<std::mutex> lock{_reg_mutex};
        _reg_done = true;
        _reg_cv.notify_one();
    }

    void on_service_registration_failed(const void*, const RegistrationFailedArgs& args) {
        std::lock_guard<std::mutex> lock{_reg_mutex};
        _reg_done = true;
        _reg_error = args.error.message();
        _reg_cv.notify_one();
    }

    void on_service_found(const void*, const BrowserArgs& args) {
        _responder.browser().resolve(args.service);
    }

    void on_service_resolved(const void*, const BrowserArgs& args) {
        if (!_browse_out) {
            return;
        }
        const auto& svc = args.service;

        // Freshness gate: check the fresh_until TXT entry.
        const auto& props = svc.properties();
        const auto it = props.find(k_fresh_until_key);
        if (it != props.end()) {
            try {
                const int64_t fresh_until = std::stoll(it->second);
                if (fresh_until < _browse_now) {
                    // Stale: exclude and record in the eviction set.
                    std::lock_guard<std::mutex> lk{_stale_mutex};
                    _stale_addresses.insert(svc.host() + ":" + std::to_string(svc.port()));
                    return;
                }
            } catch (...) {
                // Malformed fresh_until — treat as stale.
                return;
            }
        }
        // No fresh_until key: include (tolerates peers without the feature).

        std::string addr = svc.host() + ":" + std::to_string(svc.port());

        // Belt-and-suspenders: also exclude addresses in the eviction set.
        {
            std::lock_guard<std::mutex> lk{_stale_mutex};
            if (_stale_addresses.count(addr)) {
                return;
            }
        }

        std::lock_guard<std::mutex> lk{_browse_write_mutex};
        _browse_out->push_back({svc.name(), std::move(addr)});
    }

    // ── Members ────────────────────────────────────────────────────────────────

    config _cfg;
    Poco::DNSSD::DNSSDResponder _responder;
    Poco::DNSSD::ServiceHandle _service_handle{};
    std::string _self_id;
    std::string _self_address;
    std::string _host;
    int _port{0};
    bool _started{false};
    bool _registered{false};

    // Registration synchronisation.
    std::mutex _reg_mutex;
    std::condition_variable _reg_cv;
    bool _reg_done{false};
    std::string _reg_error;

    // Browse state — _browse_mutex serialises concurrent browse callers;
    // _browse_write_mutex protects _browse_out writes from Avahi callbacks.
    std::mutex _browse_mutex;
    std::mutex _browse_write_mutex;
    std::vector<peer_info<std::string, std::string>>* _browse_out{nullptr};
    int64_t _browse_now{0};

    // Stale-peer eviction set (populated by fresher browse, consulted by
    // on_service_resolved).  Protected by _stale_mutex.
    std::mutex _stale_mutex;
    std::unordered_set<std::string> _stale_addresses;

    // Freshness background thread.
    std::thread _fresher;
    std::mutex _stop_mutex;
    std::condition_variable _stop_cv;
    bool _stop{false};
};

static_assert(peer_discovery<poco_peer_discovery, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_POCO_DNSSD
