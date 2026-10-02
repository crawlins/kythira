// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// docker_quorum_manager against a real container daemon
// (.kiro/specs/quorum-management/ Req 18 AC 21).
//
// Skipped unless KYTHIRA_DOCKER_INTEGRATION_TESTS=1.  Talks to the daemon only
// through its REST API, the same way the manager does, so it runs unchanged
// against rootful Docker and rootless Podman:
//
//   KYTHIRA_DOCKER_HOST          daemon endpoint (default
//                                unix:///var/run/docker.sock); for rootless
//                                Podman use unix://$XDG_RUNTIME_DIR/podman/podman.sock
//   KYTHIRA_DOCKER_TEST_IMAGE    image the provisioned containers run
//                                (default busybox:1.36, pulled if missing)

#define BOOST_TEST_MODULE docker_quorum_manager_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/docker_quorum_manager.hpp>

#include <boost/json.hpp>
#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace kythira;
using namespace std::chrono_literals;

namespace {

auto env_or(const char* name, const char* fallback) -> std::string {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : fallback;
}

auto integration_enabled() -> bool {
    return env_or("KYTHIRA_DOCKER_INTEGRATION_TESTS", "0") == "1";
}

// An httplib client for the daemon, built the same way the manager builds
// its own.
auto daemon_client(const std::string& url) -> std::unique_ptr<httplib::Client> {
    std::unique_ptr<httplib::Client> cli;
    if (url.starts_with("unix://")) {
        cli = std::make_unique<httplib::Client>(url.substr(7));
        cli->set_address_family(AF_UNIX);
        cli->set_default_headers({{"Host", "localhost"}});
    } else {
        cli = std::make_unique<httplib::Client>(url);
    }
    cli->set_connection_timeout(10, 0);
    cli->set_read_timeout(120, 0);  // an image pull can be slow
    return cli;
}

auto url_encode_filter(const std::string& label) -> std::string {
    std::string json = R"({"label":[")" + label + R"("]})";
    std::string out;
    for (char c : json) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

struct DaemonFixture {
    std::string daemon_url = env_or("KYTHIRA_DOCKER_HOST", "unix:///var/run/docker.sock");
    std::string image = env_or("KYTHIRA_DOCKER_TEST_IMAGE", "busybox:1.36");
    // Unique per run so concurrent or interrupted runs never share containers.
    std::string cluster = "dqm-it-" + std::to_string(::getpid());
    std::string network = cluster + "-net";
    std::unique_ptr<httplib::Client> cli = daemon_client(daemon_url);

    DaemonFixture() {
        auto colon = image.rfind(':');
        std::string repo = colon == std::string::npos ? image : image.substr(0, colon);
        std::string tag = colon == std::string::npos ? "latest" : image.substr(colon + 1);
        auto pull =
            cli->Post("/images/create?fromImage=" + repo + "&tag=" + tag, "", "application/json");
        BOOST_REQUIRE_MESSAGE(pull && pull->status == 200,
                              "cannot pull " << image << " via " << daemon_url << ": "
                                             << (pull ? "HTTP " + std::to_string(pull->status)
                                                      : httplib::to_string(pull.error())));

        boost::json::object net{{"Name", network}};
        auto created =
            cli->Post("/networks/create", boost::json::serialize(net), "application/json");
        BOOST_REQUIRE_MESSAGE(created && created->status >= 200 && created->status < 300,
                              "cannot create network " << network);
    }

    ~DaemonFixture() {
        // Req 19 AC 12 style cleanup: remove every container this run labelled.
        auto list = cli->Get("/containers/json?all=true&filters=" +
                             url_encode_filter("kythira.cluster=" + cluster));
        if (list && list->status == 200) {
            for (const auto& ct : boost::json::parse(list->body).as_array()) {
                auto id = std::string(ct.as_object().at("Id").as_string());
                cli->Delete("/containers/" + id + "?force=true");
            }
        }
        cli->Delete("/networks/" + network);
    }

    auto manager_config() const -> docker_quorum_manager_config {
        docker_quorum_manager_config cfg;
        cfg.daemon_url = daemon_url;
        cfg.image = image;
        cfg.cluster_name = cluster;
        cfg.network_name = network;
        cfg.target_count = 3;
        cfg.api_timeout = 10s;
        cfg.extra_args = {"sleep", "3600"};  // keep the container running
        return cfg;
    }

    auto container_name(std::uint64_t id) const -> std::string {
        return "kythira-" + cluster + "-" + std::to_string(id);
    }

    // State.Status, or "" when the container does not exist.
    auto container_status(std::uint64_t id) const -> std::string {
        auto res = cli->Get("/containers/" + container_name(id) + "/json");
        if (!res || res->status != 200) {
            return "";
        }
        return std::string(
            boost::json::parse(res->body).at("State").as_object().at("Status").as_string());
    }

    auto container_labels(std::uint64_t id) const -> boost::json::object {
        auto res = cli->Get("/containers/" + container_name(id) + "/json");
        BOOST_REQUIRE(res && res->status == 200);
        return boost::json::parse(res->body).at("Config").as_object().at("Labels").as_object();
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(provision_assess_decommission_round_trip, *boost::unit_test::timeout(300)) {
    if (!integration_enabled()) {
        BOOST_TEST_MESSAGE("skipped: set KYTHIRA_DOCKER_INTEGRATION_TESTS=1 to run");
        return;
    }
    DaemonFixture f;
    docker_quorum_manager<> mgr(f.manager_config());

    // (b) three containers with the right names and labels
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(mgr.provision_node("default", std::nullopt).get().node_id);
    }
    BOOST_CHECK(ids == (std::vector<std::uint64_t>{1, 2, 3}));
    std::vector<node_placement<std::uint64_t, std::string>> cluster;
    for (auto id : ids) {
        BOOST_CHECK_EQUAL(f.container_status(id), "running");
        auto labels = f.container_labels(id);
        BOOST_CHECK_EQUAL(std::string(labels.at("kythira.cluster").as_string()), f.cluster);
        BOOST_CHECK_EQUAL(std::string(labels.at("kythira.node_id").as_string()),
                          std::to_string(id));
        cluster.push_back({id, "default"});
    }

    auto healthy = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(healthy.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(healthy.live_node_count, 3u);

    // (c) stop one container: one unreachable node.  AC 21c says "degraded",
    // but with 2 of 3 live one more failure loses quorum, which Req 4.4
    // defines as critical; the manager follows Req 4.4.
    auto stopped =
        f.cli->Post("/containers/" + f.container_name(2) + "/stop?t=0", "", "application/json");
    BOOST_REQUIRE(stopped && (stopped->status == 204 || stopped->status == 304));
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.status, quorum_status::critical);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes[0], 2u);

    // A further provision skips every ID still owned by a container,
    // including the stopped one.
    auto fourth = mgr.provision_node("default", std::optional<std::uint64_t>{2}).get();
    BOOST_CHECK_EQUAL(fourth.node_id, 4u);
    ids.push_back(fourth.node_id);

    // (d) decommission removes every container
    for (auto id : ids) {
        BOOST_CHECK_NO_THROW(mgr.decommission_node(id).get());
        BOOST_CHECK_EQUAL(f.container_status(id), "");
    }

    // (e) idempotent on a container that is already gone
    BOOST_CHECK_NO_THROW(mgr.decommission_node(ids.front()).get());
}
