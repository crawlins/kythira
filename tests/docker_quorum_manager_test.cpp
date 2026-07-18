#define BOOST_TEST_MODULE docker_quorum_manager_test
#include <boost/test/unit_test.hpp>

#include <raft/docker_quorum_manager.hpp>

#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>

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
    BOOST_CHECK_THROW((docker_quorum_manager<>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_empty_cluster_name) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.network_name = "n";
    BOOST_CHECK_THROW((docker_quorum_manager<>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_empty_network_name) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.cluster_name = "c";
    BOOST_CHECK_THROW((docker_quorum_manager<>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_zero_target_count) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    cfg.target_count = 0;
    BOOST_CHECK_THROW((docker_quorum_manager<>(cfg)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(constructor_rejects_zero_node_port) {
    docker_quorum_manager_config cfg;
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    cfg.node_port = 0;
    BOOST_CHECK_THROW((docker_quorum_manager<>(cfg)), std::invalid_argument);
}

// ── topology ──────────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(topology_returns_configured_group, MockDockerServer) {
    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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
    docker_quorum_manager<> mgr(cfg);

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
    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
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

    docker_quorum_manager<> mgr(make_cfg());
    // replacing hint is accepted but single-group impl ignores it
    auto peer = mgr.provision_node("default", std::optional<uint64_t>{2u}).get();
    BOOST_CHECK_EQUAL(peer.node_id, 1u);
}

// ── decommission_node ─────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_CASE(decommission_node_success_on_200, MockDockerServer) {
    server.Delete(R"(/containers/kythira-test-cluster-5)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 200; });

    docker_quorum_manager<> mgr(make_cfg());
    BOOST_CHECK_NO_THROW(mgr.decommission_node(5u).get());
}

BOOST_FIXTURE_TEST_CASE(decommission_node_404_is_idempotent, MockDockerServer) {
    server.Delete(R"(/containers/kythira-test-cluster-5)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 404; });

    docker_quorum_manager<> mgr(make_cfg());
    BOOST_CHECK_NO_THROW(mgr.decommission_node(5u).get());
}

BOOST_FIXTURE_TEST_CASE(decommission_node_failure_returns_exceptional, MockDockerServer) {
    server.Delete(R"(/containers/kythira-test-cluster-5)",
                  [](const httplib::Request&, httplib::Response& res) { res.status = 500; });

    docker_quorum_manager<> mgr(make_cfg());
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
    BOOST_CHECK_NO_THROW((docker_quorum_manager<>(cfg)));
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
    BOOST_CHECK_NO_THROW((docker_quorum_manager<>(cfg)));
}

// ── unix socket path exercises the unix:// branch of make_client() ───────────

BOOST_AUTO_TEST_CASE(make_client_unix_socket_path_coverage) {
    docker_quorum_manager_config cfg;
    cfg.daemon_url = "unix:///tmp/nonexistent-kythira-test.sock";
    cfg.image = "img";
    cfg.cluster_name = "c";
    cfg.network_name = "n";
    docker_quorum_manager<> mgr(cfg);
    // When the socket doesn't exist httplib returns a null result; the manager
    // treats the container as unreachable rather than throwing, so the future
    // resolves to a lost-quorum report (1 unreachable out of 1 = lost).
    std::vector<node_placement<uint64_t, std::string>> cluster{{1u, "default"}};
    auto h = mgr.assess_quorum(cluster).get();
    BOOST_CHECK_EQUAL(h.unreachable_nodes.size(), 1u);
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

    docker_quorum_manager<> mgr(cfg);
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

    docker_quorum_manager<> mgr(cfg);
    mgr.provision_node("default", std::nullopt).get();

    BOOST_CHECK(captured_body.find("FOO=bar") != std::string::npos);
    BOOST_CHECK(captured_body.find("BAZ=qux") != std::string::npos);
}
