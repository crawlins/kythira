#pragma once

#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef KYTHIRA_HAS_POCO_DNSSD

#include <Poco/DNSSD/DNSSDBrowser.h>
#include <Poco/DNSSD/DNSSDResponder.h>
#include <Poco/DNSSD/ServiceInfo.h>
#include <Poco/Delegate.h>

namespace kythira {

class poco_peer_discovery {
public:
    using node_id_type = std::string;
    using address_type = std::string;

    struct config {
        std::string service_type{"_raft._tcp"};
        std::string domain{};
    };

    explicit poco_peer_discovery(config cfg) : _cfg(std::move(cfg)) {}

    ~poco_peer_discovery() {
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
        const auto colon = self_address.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == self_address.size()) {
            throw std::invalid_argument(
                "poco_peer_discovery: self_address must be 'host:port', got: " + self_address);
        }

        const std::string port_str = self_address.substr(colon + 1);
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
                                            self_address);
            }
        }
        if (port_int < 1 || port_int > 65535) {
            throw std::invalid_argument("poco_peer_discovery: port out of range [1, 65535] in: " +
                                        self_address);
        }

        ensure_started();

        _self_id = std::move(self_id);
        _self_address = std::move(self_address);

        Poco::DNSSD::ServiceInfo info;
        info.setName(_self_id);
        info.setType(_cfg.service_type);
        info.setDomain(_cfg.domain);
        info.setPort(static_cast<Poco::UInt16>(port_int));

        std::unique_lock<std::mutex> lock{_reg_mutex};
        _reg_done = false;
        _reg_error.clear();

        _responder.serviceRegistered +=
            Poco::delegate(this, &poco_peer_discovery::on_service_registered);
        _responder.serviceRegistrationFailed +=
            Poco::delegate(this, &poco_peer_discovery::on_service_registration_failed);

        _responder.registerService(info, _service_handle);

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

        _registered = true;
        return kythira::FutureFactory::makeFuture();
    }

    auto find_peers(std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<peer_info<std::string, std::string>>> {
        std::vector<peer_info<std::string, std::string>> results;

        try {
            ensure_started();

            std::mutex results_mutex;
            Poco::DNSSD::DNSSDBrowser browser{_responder};

            _browse_results = &results;
            _browse_results_mutex = &results_mutex;
            _browse_browser = &browser;

            browser.serviceFound += Poco::delegate(this, &poco_peer_discovery::on_service_found);
            browser.serviceResolved +=
                Poco::delegate(this, &poco_peer_discovery::on_service_resolved);

            auto browse_handle = browser.browse(_cfg.service_type, _cfg.domain);
            std::this_thread::sleep_for(timeout);
            browser.stopBrowse(browse_handle);

            browser.serviceFound -= Poco::delegate(this, &poco_peer_discovery::on_service_found);
            browser.serviceResolved -=
                Poco::delegate(this, &poco_peer_discovery::on_service_resolved);

            _browse_results = nullptr;
            _browse_results_mutex = nullptr;
            _browse_browser = nullptr;
        } catch (...) {
            // Daemon unavailable or browse failed: return whatever was collected.
        }

        std::erase_if(results, [this](const auto& p) { return p.address == _self_address; });

        return kythira::FutureFactory::makeFuture(std::move(results));
    }

private:
    // Poco DNSSD event arg types — verify names against installed Poco headers.
    using RegistrationArgs = Poco::DNSSD::DNSSDResponder::ServiceEventArgs;
    using RegistrationFailedArgs = Poco::DNSSD::DNSSDResponder::ServiceRegistrationFailedEventArgs;
    using BrowserArgs = Poco::DNSSD::DNSSDBrowser::ServiceEventArgs;

    void ensure_started() {
        if (!_started) {
            _responder.start();
            _started = true;
        }
    }

    void on_service_registered(const void*, RegistrationArgs&) {
        std::lock_guard<std::mutex> lock{_reg_mutex};
        _reg_done = true;
        _reg_cv.notify_one();
    }

    void on_service_registration_failed(const void*, RegistrationFailedArgs& args) {
        std::lock_guard<std::mutex> lock{_reg_mutex};
        _reg_done = true;
        _reg_error = args.error;  // verify field name against installed Poco headers
        _reg_cv.notify_one();
    }

    void on_service_found(const void*, BrowserArgs& args) {
        if (_browse_browser) {
            _browse_browser->resolve(args.service);
        }
    }

    void on_service_resolved(const void*, BrowserArgs& args) {
        if (!_browse_results || !_browse_results_mutex) {
            return;
        }
        const auto& info = args.service;
        std::string addr = info.host() + ":" + std::to_string(info.port());
        std::lock_guard<std::mutex> lock{*_browse_results_mutex};
        _browse_results->push_back({info.name(), std::move(addr)});
    }

    config _cfg;
    Poco::DNSSD::DNSSDResponder _responder;
    Poco::DNSSD::DNSSDResponder::ServiceHandle _service_handle{};
    std::string _self_id;
    std::string _self_address;
    bool _started{false};
    bool _registered{false};

    std::mutex _reg_mutex;
    std::condition_variable _reg_cv;
    bool _reg_done{false};
    std::string _reg_error;

    // Browse state — only valid during an active find_peers call.
    std::vector<peer_info<std::string, std::string>>* _browse_results{nullptr};
    std::mutex* _browse_results_mutex{nullptr};
    Poco::DNSSD::DNSSDBrowser* _browse_browser{nullptr};
};

static_assert(peer_discovery<poco_peer_discovery, std::string, std::string>);

}  // namespace kythira

#endif  // KYTHIRA_HAS_POCO_DNSSD
