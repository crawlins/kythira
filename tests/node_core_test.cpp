// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The node core (.kiro/specs/service-colocation/ task 2): KYTHIRA_NODE_*
// settings and legacy aliases, the runner's lifecycle order (drain before
// stop, both before the host stops; a module that fails to start stops
// the ones before it), health and metrics, the shared endpoint, the
// per-service log prefix, and the stop signal.

#define BOOST_TEST_MODULE node_core_test
#include <boost/test/unit_test.hpp>

#include <raft/node_core.hpp>
#include <raft/node_endpoint.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <pthread.h>

namespace node = kythira::service_node;
namespace http = boost::beast::http;

namespace {

struct fake_host {
    std::vector<std::uint64_t> groups;
};

struct journal {
    std::vector<std::string> events;
};

/// A module that records each call, and can be told to fail.
class fake_module final : public node::service_module<fake_host> {
public:
    struct options {
        std::string name;
        std::uint8_t number = 1;
        std::vector<std::string> after;
        bool fail_start = false;
        bool fail_stop = false;
        bool healthy = true;
    };

    fake_module(journal& j, options o) : _j(j), _o(std::move(o)) {}

    [[nodiscard]] auto name() const -> std::string_view override { return _o.name; }
    [[nodiscard]] auto number() const -> std::uint8_t override { return _o.number; }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KYTHIRA_X_"; }
    [[nodiscard]] auto starts_after() const -> std::vector<std::string> override {
        return _o.after;
    }
    auto configure(const node::env_lookup& env) -> std::optional<std::string> override {
        if (auto v = env("KYTHIRA_" + _o.name + "_BAD")) {
            return "KYTHIRA_" + _o.name + "_BAD is set";
        }
        return std::nullopt;
    }
    void register_groups(fake_host& host, const node::group_space& space) override {
        host.groups.push_back(space.to_host(1));
        _j.events.push_back(_o.name + " register");
    }
    void publish(node::module_registry& /*registry*/) override {
        _j.events.push_back(_o.name + " publish");
    }
    void start(node::module_registry& /*registry*/) override {
        if (_o.fail_start) {
            throw std::runtime_error("port in use");
        }
        _j.events.push_back(_o.name + " start");
    }
    void drain() override { _j.events.push_back(_o.name + " drain"); }
    void stop() override {
        _j.events.push_back(_o.name + " stop");
        if (_o.fail_stop) {
            throw std::runtime_error("flush failed");
        }
    }
    [[nodiscard]] auto health() const -> node::module_health override {
        return _o.healthy ? node::module_health{true, {}}
                          : node::module_health{false, "no leader \"yet\""};
    }
    [[nodiscard]] auto metrics() const -> std::vector<node::module_metric> override {
        return {{"requests_total", 3, {{"op", "get"}}}, {"tables", 2, {}}};
    }

private:
    journal& _j;
    options _o;
};

auto env_of(std::map<std::string, std::string> vars) -> node::env_lookup {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        if (it == vars.end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    };
}

auto minimal() -> std::map<std::string, std::string> {
    return {{"KYTHIRA_NODE_ID", "1"},
            {"KYTHIRA_NODE_DATA_DIR", "/var/lib/kythira"},
            {"KYTHIRA_NODE_INSECURE_PLAINTEXT", "1"}};
}

auto config_error(const std::map<std::string, std::string>& vars) -> std::string {
    auto c = node::node_config_from_env(env_of(vars));
    const auto* e = std::get_if<std::string>(&c);
    return e == nullptr ? std::string("ok") : *e;
}

/// A plan of fakes and a runner over it, with a temporary data directory.
struct fixture {
    journal j;
    node::module_catalog<fake_host> catalog;
    std::filesystem::path dir;
    std::vector<std::string> log;

    fixture() {
        dir = std::filesystem::temp_directory_path() /
              ("node-core-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
    }
    fixture(const fixture&) = delete;
    auto operator=(const fixture&) -> fixture& = delete;
    fixture(fixture&&) = delete;
    auto operator=(fixture&&) -> fixture& = delete;
    ~fixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void add(fake_module::options o) {
        node::module_registrar<fake_host>(
            o.name, [this, o] { return std::make_unique<fake_module>(j, o); }, catalog);
    }

    auto runner(const std::vector<std::string>& services, std::uint16_t metrics_port = 0)
        -> std::unique_ptr<node::node_runner<fake_host>> {
        auto plan = node::plan_modules(catalog, services, true);
        const auto* e = std::get_if<std::string>(&plan);
        BOOST_REQUIRE_MESSAGE(e == nullptr, (e != nullptr ? *e : std::string()));
        node::node_config c;
        c.node_id = "1";
        c.data_dir = dir;
        c.insecure_plaintext = true;
        c.metrics_port = metrics_port;
        return std::make_unique<node::node_runner<fake_host>>(
            std::move(std::get<node::module_plan<fake_host>>(plan)), c,
            [this](bool error, std::string_view line) {
                log.push_back((error ? "E " : "I ") + std::string(line));
            });
    }
};

auto get(std::uint16_t port, std::string_view target, http::verb verb = http::verb::get)
    -> http::response<http::string_body> {
    boost::asio::io_context ioc;
    boost::beast::tcp_stream s(ioc);
    s.expires_after(std::chrono::seconds(10));
    s.connect({boost::asio::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> req{verb, target, 11};
    req.set(http::field::host, "127.0.0.1");
    req.prepare_payload();
    http::write(s, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(s, buf, res);
    return res;
}

}  // namespace

BOOST_AUTO_TEST_CASE(node_settings_are_read_and_checked) {
    auto vars = minimal();
    vars["KYTHIRA_NODE_PEERS"] = "1=10.0.0.1:7400, 2=node-2:7401,3=[fd00::3]:7402";
    vars["KYTHIRA_NODE_RAFT_PORT"] = "7500";
    vars["KYTHIRA_NODE_METRICS_PORT"] = "0";
    auto c = std::get<node::node_config>(node::node_config_from_env(env_of(vars)));
    BOOST_TEST(c.node_id == "1");
    BOOST_TEST(c.data_dir == std::filesystem::path("/var/lib/kythira"));
    BOOST_REQUIRE_EQUAL(c.peers.size(), 3U);
    BOOST_TEST(c.peers[1].host == "node-2");
    BOOST_TEST(c.peers[1].port == 7401U);
    BOOST_TEST(c.peers[2].host == "fd00::3");
    BOOST_TEST(c.raft_port == 7500U);
    BOOST_TEST(c.metrics_port == 0U);
    BOOST_TEST(c.metrics_bind == "127.0.0.1");
    BOOST_TEST(c.drain_seconds == 30U);

    BOOST_TEST(config_error({}) == "KYTHIRA_NODE_ID is required");
    vars = minimal();
    vars.erase("KYTHIRA_NODE_DATA_DIR");
    BOOST_TEST(config_error(vars) == "KYTHIRA_NODE_DATA_DIR is required");
    vars = minimal();
    vars["KYTHIRA_NODE_PEERS"] = "2=a:1,3=b:2";
    BOOST_TEST(config_error(vars) == "KYTHIRA_NODE_PEERS does not list this node (1)");
    vars["KYTHIRA_NODE_PEERS"] = "1=a:1,1=b:2";
    BOOST_TEST(config_error(vars) == "KYTHIRA_NODE_PEERS: node 1 is listed twice");
    vars["KYTHIRA_NODE_PEERS"] = "1=a";
    BOOST_TEST(config_error(vars) == "KYTHIRA_NODE_PEERS: expected id=host:port, got '1=a'");
    vars = minimal();
    vars["KYTHIRA_NODE_RAFT_PORT"] = "0";
    BOOST_TEST(config_error(vars) == "KYTHIRA_NODE_RAFT_PORT: not a port: '0'");
    vars = minimal();
    vars["KYTHIRA_NODE_DRAIN_SECS"] = "0";
    BOOST_TEST(config_error(vars) == "KYTHIRA_NODE_DRAIN_SECS: expected 1 to 3600, got '0'");
    vars = minimal();
    vars.erase("KYTHIRA_NODE_INSECURE_PLAINTEXT");
    BOOST_TEST(config_error(vars).starts_with("KYTHIRA_NODE_TLS_CERT, _TLS_KEY and _TLS_CA"));
    BOOST_TEST(node::node_config_usage().find("KYTHIRA_NODE_PEERS") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_standalone_daemons_legacy_names_still_work) {
    const auto env = node::with_aliases(env_of({{"KYTHIRA_NODE_ID", "1"},
                                                {"KYTHIRA_DYNAMODB_DATA_DIR", "/old"},
                                                {"KYTHIRA_NODE_RAFT_PORT", "7600"},
                                                {"KYTHIRA_DYNAMODB_RAFT_PORT", "7000"},
                                                {"KYTHIRA_NODE_INSECURE_PLAINTEXT", "1"}}),
                                        {{"KYTHIRA_NODE_DATA_DIR", "KYTHIRA_DYNAMODB_DATA_DIR"},
                                         {"KYTHIRA_NODE_RAFT_PORT", "KYTHIRA_DYNAMODB_RAFT_PORT"}});
    auto c = std::get<node::node_config>(node::node_config_from_env(env));
    BOOST_TEST(c.data_dir == std::filesystem::path("/old"));
    // The new name wins when both are set.
    BOOST_TEST(c.raft_port == 7600U);
}

BOOST_AUTO_TEST_CASE(modules_start_in_order_and_drain_before_the_host_stops) {
    fixture f;
    f.add({.name = "iam", .number = 5});
    f.add({.name = "dynamodb", .number = 2, .after = {"iam"}});
    auto r = f.runner({"dynamodb", "iam"});
    fake_host host;
    r->start(host, env_of({}));
    BOOST_TEST(std::filesystem::is_directory(f.dir / "iam"));
    BOOST_TEST(std::filesystem::is_directory(f.dir / "dynamodb"));
    BOOST_TEST(host.groups.size() == 2U);

    bool host_stopped = false;
    r->stop([&] {
        f.j.events.emplace_back("host stop");
        host_stopped = true;
    });
    BOOST_TEST(host_stopped);
    const std::vector<std::string> want{"iam register",     "dynamodb register", "iam publish",
                                        "dynamodb publish", "iam start",         "dynamodb start",
                                        "dynamodb drain",   "iam drain",         "dynamodb stop",
                                        "iam stop",         "host stop"};
    BOOST_TEST(f.j.events == want, boost::test_tools::per_element());
    // Stopping again does nothing more.
    r->stop([&] { f.j.events.emplace_back("host stop again"); });
    BOOST_TEST(f.j.events.size() == want.size());
    BOOST_TEST(f.log.front() == "I service=iam started");
}

BOOST_AUTO_TEST_CASE(every_bad_setting_is_reported_before_the_host_is_touched) {
    fixture f;
    f.add({.name = "iam", .number = 5});
    f.add({.name = "kinesis", .number = 3});
    auto r = f.runner({"iam", "kinesis"});
    fake_host host;
    try {
        r->start(host, env_of({{"KYTHIRA_iam_BAD", "1"}, {"KYTHIRA_kinesis_BAD", "1"}}));
        BOOST_FAIL("start should have thrown");
    } catch (const std::invalid_argument& e) {
        BOOST_TEST(std::string(e.what()) ==
                   "iam: KYTHIRA_iam_BAD is set; kinesis: KYTHIRA_kinesis_BAD is set");
    }
    BOOST_TEST(host.groups.empty());
    BOOST_TEST(f.j.events.empty());
}

BOOST_AUTO_TEST_CASE(a_module_that_fails_to_start_stops_the_ones_before_it) {
    fixture f;
    f.add({.name = "iam", .number = 5, .fail_stop = true});
    f.add({.name = "kinesis", .number = 3, .after = {"iam"}});
    f.add({.name = "dynamodb", .number = 2, .after = {"kinesis"}, .fail_start = true});
    auto r = f.runner({"iam", "kinesis", "dynamodb"});
    fake_host host;
    BOOST_CHECK_EXCEPTION(
        r->start(host, env_of({})), std::runtime_error, [](const std::runtime_error& e) {
            return std::string(e.what()) == "dynamodb failed to start: port in use";
        });
    const std::vector<std::string> tail{"kinesis drain", "iam drain", "kinesis stop", "iam stop"};
    BOOST_TEST(std::vector<std::string>(f.j.events.end() - 4, f.j.events.end()) == tail,
               boost::test_tools::per_element());
    // IAM's failed stop was logged, and did not keep Kinesis from stopping.
    BOOST_TEST(std::ranges::count(f.log, std::string("E service=iam stop failed: flush failed")) ==
               1);
    BOOST_TEST(std::ranges::count(f.log, std::string("E service=dynamodb failed to start: port "
                                                     "in use")) == 1);
}

BOOST_AUTO_TEST_CASE(health_and_metrics_cover_every_module) {
    fixture f;
    f.add({.name = "iam", .number = 5});
    f.add({.name = "kinesis", .number = 3, .healthy = false});
    auto r = f.runner({"iam", "kinesis"});
    // Before start, nothing is healthy.
    BOOST_TEST(r->health_report().first == 503U);
    fake_host host;
    r->start(host, env_of({}));
    const auto [status, body] = r->health_report();
    BOOST_TEST(status == 503U);
    BOOST_TEST(body ==
               R"({"node_id":"1","services":{"iam":{"healthy":true},)"
               R"("kinesis":{"healthy":false,"detail":"no leader \"yet\""}},"healthy":false})");
    const auto text = r->metrics_text();
    BOOST_TEST(text.find("kythira_node_service_up{service=\"iam\"} 1\n") != std::string::npos);
    BOOST_TEST(text.find("kythira_node_service_up{service=\"kinesis\"} 0\n") != std::string::npos);
    BOOST_TEST(text.find("kythira_iam_requests_total{op=\"get\"} 3\n") != std::string::npos);
    BOOST_TEST(text.find("kythira_kinesis_tables 2\n") != std::string::npos);
    r->stop();
}

BOOST_AUTO_TEST_CASE(the_endpoint_serves_health_and_metrics) {
    fixture f;
    f.add({.name = "iam", .number = 5});
    auto r = f.runner({"iam"});
    fake_host host;
    r->start(host, env_of({}));
    node::node_endpoint<fake_host> ep(*r);
    ep.start();
    BOOST_REQUIRE(ep.port() != 0);
    auto res = get(ep.port(), "/healthz");
    BOOST_TEST(res.result_int() == 200U);
    BOOST_TEST(res.body().find(R"("healthy":true})") != std::string::npos);
    res = get(ep.port(), "/metrics");
    BOOST_TEST(res.result_int() == 200U);
    BOOST_TEST(res.body().find("kythira_iam_tables 2") != std::string::npos);
    BOOST_TEST(get(ep.port(), "/other").result_int() == 404U);
    BOOST_TEST(get(ep.port(), "/healthz", http::verb::post).result_int() == 405U);
    ep.stop();
    r->stop();
}

BOOST_AUTO_TEST_CASE(each_line_names_its_service) {
    std::vector<std::string> lines;
    const auto sink = node::module_log(
        [&](bool error, std::string_view line) {
            lines.push_back((error ? "E " : "I ") + std::string(line));
        },
        "kinesis");
    sink(false, "listening on 4567");
    sink(true, "shard 3 lost its leader");
    BOOST_TEST(lines == (std::vector<std::string>{"I service=kinesis listening on 4567",
                                                  "E service=kinesis shard 3 lost its leader"}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_stop_signal_is_received) {
    node::block_stop_signals();
    // Blocked, so it stays pending for sigwait rather than ending the process.
    BOOST_REQUIRE(std::raise(SIGTERM) == 0);
    BOOST_TEST(node::wait_for_stop_signal() == SIGTERM);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
}
