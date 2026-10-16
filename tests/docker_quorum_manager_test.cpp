// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE docker_quorum_manager_test
#include <boost/test/unit_test.hpp>

#include <raft/docker_quorum_manager.hpp>
#include <raft/generated_node_name.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <concepts>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace kythira;
using namespace std::chrono_literals;

// ── Embedded mock Docker API server ──────────────────────────────────────────
//
// Binds to an ephemeral port before each test; docker_quorum_manager is
// configured to use http://127.0.0.1:<port> so no real Docker daemon is needed.

struct MockDockerServer {
    httplib::Server server;
    int port{0};
    std::thread srv_thread;

    MockDockerServer() {
        port = server.bind_to_any_port("127.0.0.1");
        BOOST_REQUIRE_GT(port, 0);
        srv_thread = std::thread([this] { server.listen_after_bind(); });
        // Wait until the server's accept loop is running
        for (int i = 0; i < 200 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        BOOST_REQUIRE(server.is_running());
    }

    ~MockDockerServer() {
        server.stop();
        if (srv_thread.joinable()) {
            srv_thread.join();
        }
    }

    docker_quorum_manager_config make_cfg() const {
        docker_quorum_manager_config cfg;
        cfg.daemon_url = "http://127.0.0.1:" + std::to_string(port);
        cfg.image = "kythira-test:latest";
        cfg.cluster_name = "test-cluster";
        cfg.network_name = "test-net";
        cfg.target_count = 3;
        cfg.node_port = 7000;
        cfg.api_timeout = 2s;
        return cfg;
    }

    // Convenience: serve /containers/{name}/json with a given status string
    void serve_container_state(const std::string& name, const std::string& status_str) {
        server.Get("/containers/kythira-test-cluster-" + name + "/json",
                   [status_str](const httplib::Request&, httplib::Response& res) {
                       res.set_content(R"({"State":{"Status":")" + status_str + R"("}})",
                                       "application/json");
                   });
    }
};

using Cluster = std::vector<node_placement<uint64_t, std::string>>;

// ── Constructor validation ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(constructor_rejects_empty_image) {
    docker_quorum_manager_config cfg;
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    BOOST_CHECK_THROW((docker_quorum_manager<std::uint64_t>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_empty_cluster_name) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.network_name = "n";
    BOOST_CHECK_THROW((docker_quorum_manager<std::uint64_t>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_empty_network_name) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.cluster_name = "c";
    BOOST_CHECK_THROW((docker_quorum_manager<std::uint64_t>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_zero_target_count) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    cfg.target_count = 0;
    BOOST_CHECK_THROW((docker_quorum_manager<std::uint64_t>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_zero_node_port) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    cfg.node_port = 0;
    BOOST_CHECK_THROW((docker_quorum_manager<std::uint64_t>(cfg)), std::invalid_argument);
}

// ── topology ──────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(topology_returns_configured_group, MockDockerServer) {
    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    auto t = mgr.topology();
    BOOST_REQUIRE_EQUAL(t.groups.size(), 1u);
    BOOST_CHECK_EQUAL(t.groups[0].target_count, 3u);
    BOOST_CHECK_EQUAL(t.groups[0].group_id, "default");
}

// ── assess_quorum ─────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(assess_quorum_all_running_is_healthy, MockDockerServer) {
    serve_container_state("1", "running");
    serve_container_state("2", "running");
    serve_container_state("3", "running");

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    Cluster cluster{{1u, "default"}, {2u, "default"}, {3u, "default"}};
    auto h = mgr.assess_quorum(cluster).get();

    BOOST_CHECK_EQUAL(h.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(h.live_node_count, 3u);
    BOOST_CHECK(h.unreachable_nodes.empty());
}

BOOST_FIXTURE_TEST_CASE(assess_quorum_two_of_three_is_critical, MockDockerServer) {
    serve_container_state("1", "running");
    serve_container_state("2", "exited");
    serve_container_state("3", "running");

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    Cluster cluster{{1u, "default"}, {2u, "default"}, {3u, "default"}};
    auto h = mgr.assess_quorum(cluster).get();

    BOOST_CHECK_EQUAL(h.status, quorum_status::critical);
    BOOST_CHECK_EQUAL(h.live_node_count, 2u);
    BOOST_REQUIRE_EQUAL(h.unreachable_nodes.size(), 1u);
    BOOST_CHECK_EQUAL(h.unreachable_nodes[0], 2u);
}

BOOST_FIXTURE_TEST_CASE(assess_quorum_one_of_three_is_lost, MockDockerServer) {
    serve_container_state("1", "running");
    // Node 2 returns 404 (container gone)
    server.Get("/containers/kythira-test-cluster-2/json",
               [](const httplib::Request&, httplib::Response& res) { res.status = 404; });
    serve_container_state("3", "exited");

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    Cluster cluster{{1u, "default"}, {2u, "default"}, {3u, "default"}};
    auto h = mgr.assess_quorum(cluster).get();

    BOOST_CHECK_EQUAL(h.status, quorum_status::lost);
    BOOST_CHECK_EQUAL(h.live_node_count, 1u);
    BOOST_CHECK_EQUAL(h.unreachable_nodes.size(), 2u);
}

BOOST_FIXTURE_TEST_CASE(assess_quorum_degraded_for_five_node_cluster, MockDockerServer) {
    // 4 of 5 running → degraded (above majority, below total)
    auto cfg = make_cfg();
    cfg.target_count = 5;
    docker_quorum_manager<std::uint64_t> mgr(cfg);

    for (int i = 1; i <= 4; ++i) {
        serve_container_state(std::to_string(i), "running");
    }
    serve_container_state("5", "exited");

    Cluster cluster;
    for (uint64_t i = 1; i <= 5; ++i) {
        cluster.push_back({i, "default"});
    }
    auto h = mgr.assess_quorum(cluster).get();

    BOOST_CHECK_EQUAL(h.status, quorum_status::degraded);
    BOOST_CHECK_EQUAL(h.live_node_count, 4u);
}

BOOST_FIXTURE_TEST_CASE(assess_quorum_empty_cluster_is_healthy, MockDockerServer) {
    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    auto h = mgr.assess_quorum({}).get();
    BOOST_CHECK_EQUAL(h.status, quorum_status::healthy);
    BOOST_CHECK_EQUAL(h.live_node_count, 0u);
}

// ── provision_node ────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(provision_node_success, MockDockerServer) {
    // next_node_id: no existing containers → new_id = 1
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"abc123","Warnings":[]})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    auto peer = mgr.provision_node("default", std::nullopt).get();

    BOOST_CHECK_EQUAL(peer.node_id, 1u);
    BOOST_CHECK(!peer.address.empty());
}

BOOST_FIXTURE_TEST_CASE(provision_node_increments_max_existing_id, MockDockerServer) {
    // Simulate two existing containers with node_id labels 1 and 3
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            R"([
                            {"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"1"}},
                            {"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"3"}}
                          ])",
            "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"new-id"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-4/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    auto peer = mgr.provision_node("default", std::nullopt).get();

    BOOST_CHECK_EQUAL(peer.node_id, 4u);
}

BOOST_FIXTURE_TEST_CASE(provision_node_create_failure_returns_exceptional, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 500;
        res.set_content(R"({"message":"internal error"})", "application/json");
    });
    // Cleanup Delete may be called; allow it
    server.Delete(R"(/containers/kythira-test-cluster-1)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 404; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(provision_node_start_failure_returns_exceptional, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"abc"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 500; });
    server.Delete(R"(/containers/kythira-test-cluster-1)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 200; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(provision_node_with_replacing_hint, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"x"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    // replacing hint is accepted but single-group impl ignores it
    auto peer = mgr.provision_node("default", std::optional<uint64_t>{2u}).get();
    BOOST_CHECK_EQUAL(peer.node_id, 1u);
}

// ── decommission_node ─────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(decommission_node_success_on_200, MockDockerServer) {
    server.Delete(R"(/containers/kythira-test-cluster-5)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 200; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_NO_THROW(mgr.decommission_node(5u).get());
}

BOOST_FIXTURE_TEST_CASE(decommission_node_404_is_idempotent, MockDockerServer) {
    server.Delete(R"(/containers/kythira-test-cluster-5)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 404; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_NO_THROW(mgr.decommission_node(5u).get());
}

BOOST_FIXTURE_TEST_CASE(decommission_node_failure_returns_exceptional, MockDockerServer) {
    server.Delete(R"(/containers/kythira-test-cluster-5)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 500; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.decommission_node(5u).get(), std::runtime_error);
}

// ── make_client — Unix socket path constructor ────────────────────────────────
// Verify construction with a unix:// URL doesn't throw (even if the socket
// path doesn't exist — httplib defers the connect to the first request).

BOOST_AUTO_TEST_CASE(make_client_unix_url_constructs_without_throw) {
    docker_quorum_manager_config cfg;
    cfg.daemon_url = "unix:///tmp/nonexistent-docker.sock";
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    // construction must succeed; first Get() would fail, but we don't call it
    BOOST_CHECK_NO_THROW((docker_quorum_manager<std::uint64_t>(cfg)));
}

// ── make_client — TCP without explicit port ───────────────────────────────────

BOOST_FIXTURE_TEST_CASE(make_client_tcp_no_port_uses_default, MockDockerServer) {
    // Serve the containers/json endpoint so provision_node can call next_node_id.
    // The point is to exercise the make_client() branch where the URL has no ':'.
    // We can't really test it with an ephemeral port (the URL form is "host:port"),
    // but we CAN exercise assess_quorum() with a host-only daemon_url that maps
    // to the loopback—by injecting a URL with just host and no port marker.
    // Instead, directly verify construction doesn't throw; actual network call
    // is deferred to first use.
    docker_quorum_manager_config cfg;
    cfg.daemon_url = "http://127.0.0.1";  // no colon → port defaults to 2375
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    BOOST_CHECK_NO_THROW((docker_quorum_manager<std::uint64_t>(cfg)));
}

// ── unix socket path exercises the unix:// branch of make_client() ───────────

BOOST_AUTO_TEST_CASE(assess_quorum_fails_when_daemon_socket_missing) {
    docker_quorum_manager_config cfg;
    cfg.daemon_url = "unix:///tmp/nonexistent-kythira-test.sock";
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    docker_quorum_manager<std::uint64_t> mgr(cfg);
    // Req 18 AC 10/22: an unreachable daemon fails the assessment.  Reporting
    // the node unreachable instead would read as quorum loss.
    std::vector<node_placement<uint64_t, std::string>> cluster{{1u, "default"}};
    BOOST_CHECK_THROW(mgr.assess_quorum(cluster).get(), std::runtime_error);
}

// Req 18 AC 9: every configured member is running but the cluster is smaller
// than target_count, so the leader must see a deficit.
BOOST_FIXTURE_TEST_CASE(assess_quorum_below_target_is_degraded, MockDockerServer) {
    serve_container_state("1", "running");
    serve_container_state("2", "running");

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());  // target_count = 3
    Cluster cluster{{1u, "default"}, {2u, "default"}};
    auto h = mgr.assess_quorum(cluster).get();

    BOOST_CHECK_EQUAL(h.status, quorum_status::degraded);
    BOOST_REQUIRE_EQUAL(h.groups.size(), 1u);
    BOOST_CHECK_EQUAL(h.groups[0].live_count, 2u);
    BOOST_CHECK_EQUAL(h.groups[0].target_count, 3u);
}

// Members this manager did not create carry no kythira.node_id label (e.g.
// compose-started bootstrap nodes).  Their IDs, learned from assess_quorum,
// must still never be handed out again.
BOOST_FIXTURE_TEST_CASE(provision_node_skips_ids_of_assessed_members, MockDockerServer) {
    serve_container_state("1", "running");
    serve_container_state("2", "running");
    serve_container_state("3", "exited");
    std::string list_query;
    server.Get("/containers/json", [&](const httplib::Request& req, httplib::Response& res) {
        list_query = req.has_param("all") ? req.get_param_value("all") : "";
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"n4"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-4/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    Cluster cluster{{1u, "default"}, {2u, "default"}, {3u, "default"}};
    mgr.assess_quorum(cluster).get();
    auto peer = mgr.provision_node("default", std::optional<uint64_t>{3u}).get();

    BOOST_CHECK_EQUAL(peer.node_id, 4u);
    // Exited containers still own their names, so they must be listed too.
    BOOST_CHECK_EQUAL(list_query, "true");
}

// ── extra_env / extra_args are wired into the create body ────────────────────

BOOST_FIXTURE_TEST_CASE(provision_node_forwards_extra_args, MockDockerServer) {
    auto cfg = make_cfg();
    cfg.extra_args = {"--join", "node1:7000"};

    std::string captured_body;
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [&](const httplib::Request& req, httplib::Response& res) {
        captured_body = req.body;
        res.status = 201;
        res.set_content(R"({"Id":"z"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(cfg);
    mgr.provision_node("default", std::nullopt).get();

    BOOST_CHECK(captured_body.find("--join") != std::string::npos);
    BOOST_CHECK(captured_body.find("node1:7000") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(provision_node_forwards_extra_env, MockDockerServer) {
    auto cfg = make_cfg();
    cfg.extra_env = {"FOO=bar", "BAZ=qux"};

    std::string captured_body;
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [&](const httplib::Request& req, httplib::Response& res) {
        captured_body = req.body;
        res.status = 201;
        res.set_content(R"({"Id":"y"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(cfg);
    mgr.provision_node("default", std::nullopt).get();

    BOOST_CHECK(captured_body.find("FOO=bar") != std::string::npos);
    BOOST_CHECK(captured_body.find("BAZ=qux") != std::string::npos);
}

// ── idempotency keys (elastic-shard-capacity Requirement 9.2) ────────────────

BOOST_FIXTURE_TEST_CASE(provision_node_keyed_labels_the_container_with_its_key, MockDockerServer) {
    std::string captured_body;
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [&](const httplib::Request& req, httplib::Response& res) {
        captured_body = req.body;
        res.status = 201;
        res.set_content(R"({"Id":"k"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    const auto peer = mgr.provision_node_keyed("default", std::nullopt, "intent-42").get();
    BOOST_CHECK_EQUAL(peer.node_id, 1u);
    const auto body = boost::json::parse(captured_body).as_object();
    const auto& labels = body.at("Labels").as_object();
    BOOST_CHECK_EQUAL(std::string(labels.at("kythira.idempotency-key").as_string()), "intent-42");
    // The hostname is the name peers dial.
    BOOST_CHECK_EQUAL(std::string(body.at("Hostname").as_string()), "kythira-test-cluster-1");
}

BOOST_FIXTURE_TEST_CASE(an_unkeyed_provision_carries_no_key_label, MockDockerServer) {
    std::string captured_body;
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [&](const httplib::Request& req, httplib::Response& res) {
        captured_body = req.body;
        res.status = 201;
        res.set_content(R"({"Id":"u"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    mgr.provision_node("default", std::nullopt).get();
    const auto body = boost::json::parse(captured_body).as_object();
    BOOST_CHECK(!body.at("Labels").as_object().contains("kythira.idempotency-key"));
}

BOOST_FIXTURE_TEST_CASE(find_by_idempotency_key_filters_on_cluster_and_key, MockDockerServer) {
    std::string captured_filters;
    std::string captured_all;
    server.Get("/containers/json", [&](const httplib::Request& req, httplib::Response& res) {
        captured_filters = req.get_param_value("filters");
        captured_all = req.get_param_value("all");
        res.set_content(R"([{"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"7",)"
                        R"("kythira.idempotency-key":"intent-42"}}])",
                        "application/json");
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    const auto found = mgr.find_by_idempotency_key("intent-42").get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 7u);
    BOOST_CHECK_EQUAL(found->address, "kythira-test-cluster-7:7000");
    BOOST_CHECK(captured_filters.find("kythira.idempotency-key=intent-42") != std::string::npos);
    BOOST_CHECK(captured_filters.find("kythira.cluster=test-cluster") != std::string::npos);
    BOOST_CHECK_EQUAL(captured_all, "1");  // a stopped container still exists
}

BOOST_FIXTURE_TEST_CASE(find_by_idempotency_key_returns_nothing_when_absent, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK(!mgr.find_by_idempotency_key("missing").get().has_value());
}

BOOST_FIXTURE_TEST_CASE(find_by_idempotency_key_fails_on_a_daemon_error, MockDockerServer) {
    server.Get("/containers/json",
               [](const httplib::Request&, httplib::Response& res) { res.status = 500; });
    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.find_by_idempotency_key("k").get(), std::runtime_error);
}

// ── Numeric id hardening (cloud-composite-node-ids Requirement 7) ────────────

BOOST_FIXTURE_TEST_CASE(provision_node_ignores_labels_it_could_not_have_written, MockDockerServer) {
    // "-1" read through std::stoull was the largest uint64, so the + 1 wrapped
    // to node 0; "7x" read as 7. Neither is a label this manager writes.
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            R"([{"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"2"}},
                {"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"-1"}},
                {"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"7x"}},
                {"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":5}}])",
            "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"n3"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-3/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_EQUAL(mgr.provision_node("default", std::nullopt).get().node_id, 3u);
}

BOOST_FIXTURE_TEST_CASE(provision_node_refuses_to_wrap_past_the_largest_id, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"([{"Labels":{"kythira.cluster":"test-cluster",)"
                        R"("kythira.node_id":"18446744073709551615"}}])",
                        "application/json");
    });
    bool created = false;
    server.Post("/containers/create", [&](const httplib::Request&, httplib::Response& res) {
        created = true;
        res.status = 201;
        res.set_content(R"({"Id":"n0"})", "application/json");
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::exception);
    BOOST_CHECK(!created);
}

BOOST_FIXTURE_TEST_CASE(find_by_idempotency_key_skips_an_unparseable_label, MockDockerServer) {
    // One container carrying a label nobody here wrote used to fail the whole
    // lookup with std::invalid_argument; it is now skipped.
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(
            R"([{"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"node-a",)"
            R"("kythira.idempotency-key":"k"}},)"
            R"({"Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"6",)"
            R"("kythira.idempotency-key":"k"}}])",
            "application/json");
    });
    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    const auto found = mgr.find_by_idempotency_key("k").get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK_EQUAL(found->node_id, 6u);
}

// ── Claiming a node id across processes ──────────────────────────────────────
//
// next_node_id() is only a guess: another leader's manager may list the same
// containers and create the same id first.  The container name embeds the id
// and Docker refuses a duplicate name with 409, which provision_node must read
// as "taken, try the next id" -- never as a reason to delete the container
// that holds the name, which belongs to someone else.

BOOST_FIXTURE_TEST_CASE(provision_node_moves_past_an_id_another_process_claimed, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    std::vector<std::string> create_names;
    server.Post("/containers/create", [&](const httplib::Request& req, httplib::Response& res) {
        const auto name = req.get_param_value("name");
        create_names.push_back(name);
        if (name == "kythira-test-cluster-1") {
            res.status = 409;
            res.set_content(R"({"message":"Conflict. The container name is already in use"})",
                            "application/json");
            return;
        }
        res.status = 201;
        res.set_content(R"({"Id":"n2"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-2/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    std::vector<std::string> deletes;
    server.Delete(R"(/containers/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        deletes.push_back(req.path);
        res.status = 204;
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    auto peer = mgr.provision_node("default", std::nullopt).get();

    BOOST_CHECK_EQUAL(peer.node_id, 2u);
    BOOST_CHECK_EQUAL(peer.address, "kythira-test-cluster-2:7000");
    BOOST_REQUIRE_EQUAL(create_names.size(), 2u);
    BOOST_CHECK_EQUAL(create_names[0], "kythira-test-cluster-1");
    BOOST_CHECK_EQUAL(create_names[1], "kythira-test-cluster-2");
    // The other process's container is left alone.
    BOOST_CHECK(deletes.empty());

    // The collided id stays spent for this manager too.
    server.Post(R"(/containers/kythira-test-cluster-3/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    BOOST_CHECK_EQUAL(mgr.provision_node("default", std::nullopt).get().node_id, 3u);
}

BOOST_FIXTURE_TEST_CASE(provision_node_gives_up_after_repeated_collisions, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    std::size_t creates = 0;
    server.Post("/containers/create", [&](const httplib::Request&, httplib::Response& res) {
        ++creates;
        res.status = 409;
    });
    std::size_t deletes = 0;
    server.Delete(R"(/containers/(.*))", [&](const httplib::Request&, httplib::Response& res) {
        ++deletes;
        res.status = 204;
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(creates, decltype(mgr)::max_id_claim_attempts);
    BOOST_CHECK_EQUAL(deletes, 0u);
}

// A listing that fails used to fall back to the in-memory floor alone, which
// knows nothing about containers this process never assessed.
BOOST_FIXTURE_TEST_CASE(provision_node_fails_when_the_listing_fails, MockDockerServer) {
    server.Get("/containers/json",
               [](const httplib::Request&, httplib::Response& res) { res.status = 500; });
    bool created = false;
    server.Post("/containers/create", [&](const httplib::Request&, httplib::Response& res) {
        created = true;
        res.status = 201;
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
    BOOST_CHECK(!created);
}

// After a create that failed without a clear answer, the cleanup removes the
// container under that name only if it carries this call's claim label.
BOOST_FIXTURE_TEST_CASE(provision_node_cleanup_removes_only_its_own_container, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    std::string stored_labels = "{}";
    server.Post("/containers/create", [&](const httplib::Request& req, httplib::Response& res) {
        // The daemon created the container but the request still failed.
        stored_labels =
            boost::json::serialize(boost::json::parse(req.body).as_object().at("Labels"));
        res.status = 500;
    });
    server.Get(R"(/containers/kythira-test-cluster-1/json)",
               [&](const httplib::Request&, httplib::Response& res) {
                   res.set_content(R"({"Id":"c1","Config":{"Labels":)" + stored_labels + "}}",
                                   "application/json");
               });
    std::vector<std::string> deletes;
    server.Delete(R"(/containers/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        deletes.push_back(req.path);
        res.status = 204;
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(deletes.size(), 1u);
    BOOST_CHECK_EQUAL(deletes[0], "/containers/kythira-test-cluster-1");
}

BOOST_FIXTURE_TEST_CASE(provision_node_cleanup_spares_another_process_container, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create",
                [](const httplib::Request&, httplib::Response& res) { res.status = 500; });
    server.Get(R"(/containers/kythira-test-cluster-1/json)",
               [](const httplib::Request&, httplib::Response& res) {
                   res.set_content(R"({"Id":"c1","Config":{"Labels":{"kythira.node_id":"1",)"
                                   R"("kythira.provision-claim":"someone-else"}}})",
                                   "application/json");
               });
    std::size_t deletes = 0;
    server.Delete(R"(/containers/(.*))", [&](const httplib::Request&, httplib::Response& res) {
        ++deletes;
        res.status = 204;
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(deletes, 0u);
}

// Once this call's create succeeded the container is known to be its own, so
// a failed start removes it by the id Docker returned, not by name.
BOOST_FIXTURE_TEST_CASE(provision_node_start_failure_removes_by_container_id, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("[]", "application/json");
    });
    server.Post("/containers/create", [](const httplib::Request&, httplib::Response& res) {
        res.status = 201;
        res.set_content(R"({"Id":"deadbeef"})", "application/json");
    });
    server.Post(R"(/containers/kythira-test-cluster-1/start)",
                [](const httplib::Request&, httplib::Response& res) { res.status = 500; });
    std::vector<std::string> deletes;
    server.Delete(R"(/containers/(.*))", [&](const httplib::Request& req, httplib::Response& res) {
        deletes.push_back(req.path);
        res.status = 204;
    });

    docker_quorum_manager<std::uint64_t> mgr(make_cfg());
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(deletes.size(), 1u);
    BOOST_CHECK_EQUAL(deletes[0], "/containers/deadbeef");
}

// ── Composite and string mode (cloud-composite-node-ids Requirement 5) ───────
//
// The node id is the container: {cluster_name, container name}. The manager
// generates the name before the create, so nothing is listed or allocated,
// and the id rides in the create request as canonical text.

namespace {

static_assert(quorum_manager<docker_quorum_manager<docker_container_node_id>,
                             docker_container_node_id, std::string, std::string>);
static_assert(
    quorum_manager<docker_quorum_manager<std::string>, std::string, std::string, std::string>);
static_assert(docker_quorum_manager<docker_container_node_id>::name_is_node_id);
static_assert(docker_quorum_manager<std::string>::name_is_node_id);
static_assert(!docker_quorum_manager<std::uint64_t>::name_is_node_id);

/// `kythira-<cluster part>-<8 lower-case base32>`.
auto is_generated_name(const std::string& name, const std::string& cluster_part) -> bool {
    const std::string prefix = "kythira-" + cluster_part + "-";
    if (name.size() != prefix.size() + generated_name_suffix_length || !name.starts_with(prefix)) {
        return false;
    }
    return std::ranges::all_of(name.substr(prefix.size()), [](char c) {
        return generated_name_alphabet.find(c) != std::string_view::npos;
    });
}

template<typename NodeId>
auto docker_id(const std::string& cluster, const std::string& name) -> NodeId {
    docker_container_node_id id{cluster, name};
    if constexpr (std::same_as<NodeId, std::string>) {
        return id.to_string();
    } else {
        return id;
    }
}

/// A daemon whose containers are created through the API, and which a test
/// can stop. A create under a name already held is refused with 409.
struct NamedContainers {
    std::map<std::string, std::string> state;  // name -> State.Status
    std::vector<std::string> create_names;
    std::vector<boost::json::value> create_bodies;
    std::vector<std::string> deletes;
    std::size_t lists = 0;
    std::size_t inspects = 0;
    /// Creates to refuse with 409, as if another process held the name.
    std::size_t refuse_creates = 0;
    std::mutex mu;

    void serve(httplib::Server& server) {
        server.Get("/containers/json", [this](const httplib::Request&, httplib::Response& res) {
            std::lock_guard lock(mu);
            ++lists;
            res.set_content("[]", "application/json");
        });
        server.Post("/containers/create",
                    [this](const httplib::Request& req, httplib::Response& res) {
                        std::lock_guard lock(mu);
                        const auto name = req.get_param_value("name");
                        create_names.push_back(name);
                        create_bodies.push_back(boost::json::parse(req.body));
                        if (state.contains(name) || refuse_creates > 0) {
                            refuse_creates -= refuse_creates > 0 ? 1 : 0;
                            res.status = 409;
                            return;
                        }
                        state[name] = "created";
                        res.status = 201;
                        res.set_content(R"({"Id":"id-)" + name + R"("})", "application/json");
                    });
        server.Post(R"(/containers/([^/]+)/start)",
                    [this](const httplib::Request& req, httplib::Response& res) {
                        std::lock_guard lock(mu);
                        state[req.matches[1]] = "running";
                        res.status = 204;
                    });
        server.Get(R"(/containers/([^/]+)/json)", [this](const httplib::Request& req,
                                                         httplib::Response& res) {
            std::lock_guard lock(mu);
            ++inspects;
            auto it = state.find(req.matches[1]);
            if (it == state.end()) {
                res.status = 404;
                return;
            }
            res.set_content(R"({"State":{"Status":")" + it->second + R"("}})", "application/json");
        });
        server.Delete(R"(/containers/([^/?]+))",
                      [this](const httplib::Request& req, httplib::Response& res) {
                          std::lock_guard lock(mu);
                          deletes.push_back(req.matches[1]);
                          res.status = state.erase(req.matches[1]) > 0 ? 204 : 404;
                      });
    }
};

/// Provision three nodes, check what each create carried, assess them, stop
/// one, decommission one: the same walk for composite and string mode.
template<typename NodeId> void run_named_lifecycle(MockDockerServer& srv) {
    NamedContainers daemon;
    daemon.serve(srv.server);
    auto cfg = srv.make_cfg();
    cfg.extra_env = {"PEER_ID={NODE_ID}"};
    cfg.extra_args = {"--node-id={NODE_ID}", "--port=7000"};
    docker_quorum_manager<NodeId> mgr(cfg);

    std::vector<NodeId> ids;
    for (int i = 0; i < 3; ++i) {
        auto peer = mgr.provision_node("default", std::nullopt).get();
        const auto name = daemon.create_names.back();
        BOOST_CHECK_MESSAGE(is_generated_name(name, "test-cluster"), "name " << name);
        BOOST_CHECK(peer.node_id == docker_id<NodeId>("test-cluster", name));
        BOOST_CHECK_EQUAL(peer.address, name + ":7000");
        BOOST_CHECK_EQUAL(mgr.container_name_of(peer.node_id).value_or(""), name);

        // The id is known before the create, so it rides in the request.
        const auto text = "docker:test-cluster:" + name;
        const auto& body = daemon.create_bodies.back().as_object();
        BOOST_CHECK_EQUAL(body.at("Labels").at("kythira.node_id").as_string(), text);
        BOOST_CHECK_EQUAL(body.at("Hostname").as_string(), name);
        const auto& env = body.at("Env").as_array();
        BOOST_CHECK(std::ranges::find(env, boost::json::value("KYTHIRA_NODE_ID=" + text)) !=
                    env.end());
        BOOST_CHECK(std::ranges::find(env, boost::json::value("PEER_ID=" + text)) != env.end());
        BOOST_CHECK_EQUAL(body.at("Cmd").as_array().at(0).as_string(), "--node-id=" + text);
        BOOST_CHECK_EQUAL(body.at("Cmd").as_array().at(1).as_string(), "--port=7000");
        ids.push_back(peer.node_id);
    }
    // Nothing was allocated: no label scan, and three distinct names.
    BOOST_CHECK_EQUAL(daemon.lists, 0u);
    BOOST_CHECK(ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2]);

    std::vector<node_placement<NodeId, std::string>> cluster;
    for (const auto& id : ids) {
        cluster.push_back({.node_id = id, .group_id = "default"});
    }
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK(health.status == quorum_status::healthy);
    BOOST_CHECK_EQUAL(health.live_node_count, 3u);

    {
        std::lock_guard lock(daemon.mu);
        daemon.state[*mgr.container_name_of(ids[0])] = "exited";
    }
    health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK(health.status == quorum_status::critical);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK(health.unreachable_nodes[0] == ids[0]);

    // Decommission removes the named container, and only it.
    mgr.decommission_node(ids[1]).get();
    BOOST_REQUIRE_EQUAL(daemon.deletes.size(), 1u);
    BOOST_CHECK_EQUAL(daemon.deletes[0], *mgr.container_name_of(ids[1]));
    health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK(health.status == quorum_status::lost);
}

}  // namespace

// Composite mode is the default (task 4.4); numeric mode must be asked for.
static_assert(std::same_as<kythira::docker_quorum_manager<>::node_id_type,
                           kythira::docker_container_node_id>);

BOOST_FIXTURE_TEST_CASE(composite_mode_lifecycle, MockDockerServer) {
    run_named_lifecycle<docker_container_node_id>(*this);
}

BOOST_FIXTURE_TEST_CASE(string_mode_lifecycle, MockDockerServer) {
    run_named_lifecycle<std::string>(*this);
}

// The name is also the hostname and the DNS label peers dial, so it is held to
// 63 characters: a long cluster name is cut, the random suffix never is.
BOOST_FIXTURE_TEST_CASE(composite_mode_cuts_a_long_cluster_name_to_fit, MockDockerServer) {
    NamedContainers daemon;
    daemon.serve(server);
    auto cfg = make_cfg();
    cfg.cluster_name = std::string(50, 'c') + "-" + std::string(12, 'd');  // 63 characters
    docker_quorum_manager<docker_container_node_id> mgr(cfg);

    auto peer = mgr.provision_node("default", std::nullopt).get();
    const auto& name = daemon.create_names.back();
    BOOST_CHECK_EQUAL(name.size(), 63u);
    // 63 - "kythira-" - "-" - 8 leaves 46 characters of the cluster name.
    BOOST_CHECK_MESSAGE(is_generated_name(name, std::string(46, 'c')), "name " << name);
    // The scope keeps the whole cluster name.
    BOOST_CHECK_EQUAL(peer.node_id.scope(), cfg.cluster_name);
    BOOST_CHECK_EQUAL(peer.node_id.native(), name);
}

// A cluster name that cannot be the id's scope is refused up front in the
// textual modes; numeric mode does not use it as a scope and accepts it.
BOOST_FIXTURE_TEST_CASE(composite_mode_requires_a_valid_scope, MockDockerServer) {
    auto cfg = make_cfg();
    cfg.cluster_name = "Test.Cluster";
    BOOST_CHECK_THROW(docker_quorum_manager<docker_container_node_id>{cfg}, std::invalid_argument);
    BOOST_CHECK_THROW(docker_quorum_manager<std::string>{cfg}, std::invalid_argument);
    BOOST_CHECK_NO_THROW(docker_quorum_manager<std::uint64_t>{cfg});
}

// A name another process already holds is retried under a fresh name and
// never removed.
BOOST_FIXTURE_TEST_CASE(composite_mode_retries_a_name_collision, MockDockerServer) {
    NamedContainers daemon;
    daemon.refuse_creates = 1;
    daemon.serve(server);
    docker_quorum_manager<docker_container_node_id> mgr(make_cfg());

    auto peer = mgr.provision_node("default", std::nullopt).get();
    const auto& creates = daemon.create_names;
    BOOST_REQUIRE_EQUAL(creates.size(), 2u);
    BOOST_CHECK_NE(creates[0], creates[1]);
    BOOST_CHECK_EQUAL(peer.node_id.native(), creates[1]);
    BOOST_CHECK(daemon.deletes.empty());

    // Every attempt colliding gives up, still without removing anything.
    daemon.refuse_creates = 100;
    BOOST_CHECK_THROW(mgr.provision_node("default", std::nullopt).get(), std::runtime_error);
    BOOST_CHECK_EQUAL(creates.size(), 2u + decltype(mgr)::max_id_claim_attempts);
    BOOST_CHECK(daemon.deletes.empty());
}

// An id scoped to another cluster, or text that is not canonical docker text,
// is unreachable without a daemon call and does not fail the assessment; it
// cannot be decommissioned either.
BOOST_FIXTURE_TEST_CASE(composite_mode_reports_foreign_ids_unreachable, MockDockerServer) {
    NamedContainers daemon;
    daemon.state["kythira-test-cluster-aaaaaaaa"] = "running";
    daemon.state["kythira-other-aaaaaaaa"] = "running";
    daemon.serve(server);
    docker_quorum_manager<docker_container_node_id> mgr(make_cfg());

    std::vector<node_placement<docker_container_node_id, std::string>> cluster{
        {.node_id = {"test-cluster", "kythira-test-cluster-aaaaaaaa"}, .group_id = "default"},
        {.node_id = {"other", "kythira-other-aaaaaaaa"}, .group_id = "default"}};
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_REQUIRE_EQUAL(health.unreachable_nodes.size(), 1u);
    BOOST_CHECK(health.unreachable_nodes[0] == cluster[1].node_id);
    BOOST_CHECK_EQUAL(daemon.inspects, 1u);

    BOOST_CHECK_THROW(mgr.decommission_node(cluster[1].node_id).get(), std::runtime_error);
    BOOST_CHECK(daemon.deletes.empty());
}

BOOST_FIXTURE_TEST_CASE(string_mode_rejects_non_canonical_text, MockDockerServer) {
    NamedContainers daemon;
    daemon.state["kythira-test-cluster-aaaaaaaa"] = "running";
    daemon.state["kythira-test-cluster-1"] = "running";
    daemon.serve(server);
    docker_quorum_manager<std::string> mgr(make_cfg());

    std::vector<node_placement<std::string, std::string>> cluster{
        {.node_id = "docker:test-cluster:kythira-test-cluster-aaaaaaaa", .group_id = "default"},
        {.node_id = "kythira-test-cluster-aaaaaaaa", .group_id = "default"},
        {.node_id = "1", .group_id = "default"}};
    auto health = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 1u);
    BOOST_CHECK_EQUAL(health.unreachable_nodes.size(), 2u);
    BOOST_CHECK_THROW(mgr.decommission_node("1").get(), std::runtime_error);
    BOOST_CHECK(daemon.deletes.empty());
}

// The keyed lookup reads the id from the container's own name, not from its
// label; a container whose name is not a valid id is skipped.
BOOST_FIXTURE_TEST_CASE(composite_mode_find_by_idempotency_key_reads_the_name, MockDockerServer) {
    server.Get("/containers/json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"([{"Names":["/-bad name"],"Labels":{"kythira.cluster":"test-cluster",)"
                        R"("kythira.node_id":"docker:test-cluster:-bad name"}},)"
                        R"({"Names":["/kythira-test-cluster-abcdefgh"],)"
                        R"("Labels":{"kythira.cluster":"test-cluster","kythira.node_id":"7",)"
                        R"("kythira.idempotency-key":"intent-42"}}])",
                        "application/json");
    });
    docker_quorum_manager<docker_container_node_id> mgr(make_cfg());
    const auto found = mgr.find_by_idempotency_key("intent-42").get();
    BOOST_REQUIRE(found.has_value());
    BOOST_CHECK(found->node_id ==
                docker_container_node_id("test-cluster", "kythira-test-cluster-abcdefgh"));
    BOOST_CHECK_EQUAL(found->address, "kythira-test-cluster-abcdefgh:7000");
}

// ── generated_node_name ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(generated_node_name_fits_the_limit) {
    BOOST_CHECK_EQUAL(generated_node_name("c", "abcdefgh", 63), "kythira-c-abcdefgh");
    // Exactly fits: no cut.
    const std::string fits(46, 'x');
    BOOST_CHECK_EQUAL(generated_node_name(fits, "abcdefgh", 63), "kythira-" + fits + "-abcdefgh");
    // One over: the cluster part loses one character, the suffix none.
    BOOST_CHECK_EQUAL(generated_node_name(fits + "y", "abcdefgh", 63),
                      "kythira-" + fits + "-abcdefgh");
    // A cut that ends on separators drops them.
    BOOST_CHECK_EQUAL(generated_node_name(std::string(44, 'x') + "-._zz", "abcdefgh", 63),
                      "kythira-" + std::string(44, 'x') + "-abcdefgh");
    // No room for any of the cluster: kythira-<suffix>.
    BOOST_CHECK_EQUAL(generated_node_name("cluster", "abcdefgh", 16), "kythira-abcdefgh");
    BOOST_CHECK_EQUAL(generated_node_name("cluster", "abcdefgh", 17), "kythira-abcdefgh");
    BOOST_CHECK_EQUAL(generated_node_name("cluster", "abcdefgh", 18), "kythira-c-abcdefgh");
    BOOST_CHECK_THROW(generated_node_name("cluster", "abcdefgh", 15), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(random_name_suffix_is_lower_case_base32) {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const auto s = random_name_suffix();
        BOOST_CHECK_EQUAL(s.size(), generated_name_suffix_length);
        BOOST_CHECK(std::ranges::all_of(
            s, [](char c) { return generated_name_alphabet.find(c) != std::string_view::npos; }));
        seen.insert(s);
    }
    BOOST_CHECK_GT(seen.size(), 60u);  // 40 random bits: a repeat is vanishingly rare
}
