// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE multi_raft_node_s3_unavailable_test
#include <boost/test/unit_test.hpp>

// `multi_raft_node --persistence s3` in a build WITHOUT the S3 arm
// (.kiro/specs/multi-raft-node-s3-persistence/ task 7.2, Requirements 7.2 and
// 10.1): it must refuse naming the switch to turn on, not report an unknown
// mode, and `--help` must still list the mode as unavailable. Registered only
// in such a build; the no_cloud defconfig CI job is one.

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef MULTI_RAFT_NODE_PATH
#error "MULTI_RAFT_NODE_PATH must name the built multi_raft_node binary"
#endif

namespace {

struct run_result {
    int status{};
    std::string output;
};

auto run(std::vector<std::string> args) -> run_result {
    std::string pattern = "/tmp/kythira-s3-unavailable-XXXXXX";
    const int fd = ::mkstemp(pattern.data());
    BOOST_REQUIRE(fd >= 0);

    std::string binary = MULTI_RAFT_NODE_PATH;
    std::vector<char*> argv{binary.data()};
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::string path_env = "PATH=/usr/bin:/bin";
    std::vector<char*> envp{path_env.data(), nullptr};

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
    pid_t pid = 0;
    const int spawned =
        ::posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(fd);
    BOOST_REQUIRE(spawned == 0);

    run_result result;
    BOOST_REQUIRE(::waitpid(pid, &result.status, 0) == pid);
    std::ifstream in(pattern);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    result.output = buffer.str();
    ::unlink(pattern.c_str());
    return result;
}

auto contains(const std::string& haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_CASE(persistence_s3_names_the_kconfig_switch) {
    const auto r = run({"--raft-port", "19001", "--data-port", "19002", "--control-port", "19003",
                        "--peer", "1=http://127.0.0.1:19001", "--persistence", "s3", "--s3-bucket",
                        "b", "--s3-prefix", "run"});
    BOOST_TEST(WIFEXITED(r.status));
    BOOST_TEST(WEXITSTATUS(r.status) == 2);
    BOOST_TEST(contains(r.output, "CONFIG_AWS_S3_PERSISTENCE=y"), r.output);
    BOOST_TEST(!contains(r.output, "must be memory|"), r.output);
}

BOOST_AUTO_TEST_CASE(help_marks_s3_unavailable) {
    const auto r = run({"--help"});
    BOOST_TEST(WEXITSTATUS(r.status) == 0);
    BOOST_TEST(contains(r.output, "file-barrier | s3"), r.output);
    BOOST_TEST(contains(r.output, "UNAVAILABLE IN THIS BUILD"), r.output);
}
