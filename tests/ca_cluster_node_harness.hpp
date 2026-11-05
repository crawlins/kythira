// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Spawning ca_cluster_node subprocesses from a test: shared by
// ca_cluster_node_test and the real-cloud OIDC federation test
// (oidc_federation_real_test).

#include "ca_cluster_node_process_wait.hpp"

#include <boost/test/unit_test.hpp>

#include <httplib.h>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef CA_CLUSTER_NODE_PATH
#define CA_CLUSTER_NODE_PATH "ca_cluster_node"
#endif

namespace ca_cluster_harness {

using raft::testing::wait_for_exit_or_kill;

inline auto find_free_port() -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// One ca_cluster_node subprocess. RAII: SIGTERM + waitpid on destruction
// (or explicit stop()/kill_hard() for tests that need to control the moment
// of death, e.g. simulating leader crash).
struct cluster_node_process {
    pid_t pid{-1};
    std::uint64_t node_id;
    int http_port;
    int rpc_port;
    std::string data_dir;
    std::string unseal_key_file;
    std::string auth_token;
    std::string peers_arg;
    bool bootstrap;
    // Appended to every spawn. These nodes have no RPC TLS material and no
    // client-API TLS listener, so they bind both Raft RPC and the HTTP API to
    // loopback, the one plaintext case ca_cluster_node allows without
    // --allow-plaintext-rpc / --allow-plaintext-http.
    std::vector<std::string> extra_args{"--rpc-address", "127.0.0.1", "--http-address",
                                        "127.0.0.1"};

    cluster_node_process(std::uint64_t id, int rpc_port_, int http_port_, std::string data_dir_,
                         std::string unseal_key_file_, std::string auth_token_,
                         std::string peers_arg_, bool bootstrap_,
                         std::optional<std::vector<std::string>> extra_args_ = std::nullopt)
        : node_id(id),
          http_port(http_port_),
          rpc_port(rpc_port_),
          data_dir(std::move(data_dir_)),
          unseal_key_file(std::move(unseal_key_file_)),
          auth_token(std::move(auth_token_)),
          peers_arg(std::move(peers_arg_)),
          bootstrap(bootstrap_) {
        if (extra_args_) {
            extra_args = std::move(*extra_args_);
        }
        std::filesystem::create_directories(data_dir);
        spawn();
    }

    ~cluster_node_process() { stop(); }

    cluster_node_process(const cluster_node_process&) = delete;
    cluster_node_process& operator=(const cluster_node_process&) = delete;

    auto spawn() -> void {
        std::vector<std::string> argv_strs = {
            CA_CLUSTER_NODE_PATH,
            "--node-id",
            std::to_string(node_id),
            "--rpc-port",
            std::to_string(rpc_port),
            "--http-port",
            std::to_string(http_port),
            "--data-dir",
            data_dir,
            "--unseal-key-file",
            unseal_key_file,
            "--auth-token",
            auth_token,
        };
        if (!peers_arg.empty()) {
            argv_strs.emplace_back("--peers");
            argv_strs.push_back(peers_arg);
        }
        if (bootstrap) {
            argv_strs.emplace_back("--bootstrap-ca");
        }
        argv_strs.insert(argv_strs.end(), extra_args.begin(), extra_args.end());

        std::vector<char*> argv;
        argv.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);

        // fork()+prctl(PR_SET_PDEATHSIG)+execve() rather than posix_spawn:
        // if this test process dies abnormally (killed by an external
        // timeout, or a signal whose delivery doesn't unwind through this
        // stack frame) before cluster_node_process's destructor runs, an
        // orphaned ca_cluster_node child inherits this process's stdout/
        // stderr pipe and keeps it open indefinitely - observed to wedge
        // ctest's own output capture, hanging the whole ctest invocation
        // on what should have been a single failed test. PR_SET_PDEATHSIG
        // makes the kernel SIGKILL this child directly the moment its
        // parent dies, for any reason, without relying on that parent's
        // own cleanup code running at all.
        pid_t parent_pid = getpid();
        pid = fork();
        BOOST_REQUIRE_MESSAGE(pid >= 0, "fork() failed: " << std::strerror(errno));
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent_pid) {
                // Parent already died in the narrow window between fork()
                // and prctl() taking effect - exit immediately rather than
                // run on as an unsupervised orphan.
                _exit(1);
            }
            execve(CA_CLUSTER_NODE_PATH, argv.data(), environ);
            _exit(127);  // execve() only returns on failure
        }
    }

    auto stop() -> void {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            // Bounded, not a plain blocking waitpid(): see
            // ca_cluster_node_process_wait.hpp's own comment for why -- this
            // is a safety net against a regression of the exact shutdown-
            // ordering bug that used to hang this process indefinitely on
            // SIGTERM, not an expectation that 30s is ever actually needed.
            if (!wait_for_exit_or_kill(pid, std::chrono::seconds(30))) {
                BOOST_ERROR("cluster_node_process::stop(): node " << node_id << " (pid " << pid
                                                                  << ") did not exit within 30s "
                                                                     "of SIGTERM; force-killed");
            }
            pid = -1;
        }
    }

    auto kill_hard() -> void {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    // Restarts this node against the SAME data_dir/ports (simulates a
    // process crash-and-restart, not a fresh node).
    auto restart(bool bootstrap_again = false) -> void {
        BOOST_REQUIRE(pid <= 0);  // must be stopped first
        bootstrap = bootstrap_again;
        spawn();
    }

    [[nodiscard]] auto is_running() const -> bool { return pid > 0; }

    // Reaps the process if it has already exited and describes how it ended,
    // or returns nullopt while it is still running.
    auto reap_if_exited() -> std::optional<std::string> {
        if (pid <= 0) {
            return std::nullopt;
        }
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) != pid) {
            return std::nullopt;
        }
        pid = -1;
        if (WIFEXITED(status)) {
            return "exited with status " + std::to_string(WEXITSTATUS(status));
        }
        if (WIFSIGNALED(status)) {
            return "killed by signal " + std::to_string(WTERMSIG(status));
        }
        return std::string("ended");
    }

    // Waits for the process to exit on its own and returns its exit code, or
    // nullopt if it is still running at the deadline or died from a signal.
    auto wait_for_own_exit(std::chrono::milliseconds timeout) -> std::optional<int> {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (pid > 0 && std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                pid = -1;
                if (WIFEXITED(status)) {
                    return WEXITSTATUS(status);
                }
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return std::nullopt;
    }
};

inline auto wait_healthy(int http_port, std::chrono::seconds timeout) -> bool {
    httplib::Client c("127.0.0.1", http_port);
    c.set_connection_timeout(1, 0);
    c.set_read_timeout(10, 0);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto res = c.Get("/healthz");
        if (res && res->status == 200) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

struct lone_node_env {
    std::string tmp_root;
    std::string unseal_key_file;
    std::string peers;

    explicit lone_node_env(const std::string& name) {
        tmp_root = (std::filesystem::temp_directory_path() /
                    ("ca_cluster_node_" + name + "_" + std::to_string(::getpid())))
                       .string();
        std::filesystem::create_directories(tmp_root);
        unseal_key_file = tmp_root + "/unseal.key";
        std::ofstream(unseal_key_file) << "plaintext-opt-in-test-passphrase\n";
        std::ostringstream p;
        p << "2:127.0.0.1:" << find_free_port() << "@http://127.0.0.1:1";
        peers = p.str();
    }
    ~lone_node_env() {
        std::error_code ec;
        std::filesystem::remove_all(tmp_root, ec);
    }
    lone_node_env(const lone_node_env&) = delete;
    auto operator=(const lone_node_env&) -> lone_node_env& = delete;
};

}  // namespace ca_cluster_harness
