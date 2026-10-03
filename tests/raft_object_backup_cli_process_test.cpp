// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE raft_object_backup_cli_process_test
#include <boost/test/unit_test.hpp>

// Runs the built `raft_object_backup` binary
// (`.kiro/specs/object-backup-oci-oss-credentials/`, Requirement 6.3).
//
// The unit tests prove the environment mapping; only the binary can prove
// that `main.cpp` wires it in and that a construction failure exits 2 rather
// than reaching `std::terminate`. Each run gets an explicit, scrubbed `envp`,
// so a developer's own credentials never leak into a case.
//
// Registered by `cmd/raft_object_backup/CMakeLists.txt` only when the OCI or
// OSS provider is compiled in, with the same KYTHIRA_BACKUP_PROVIDER_* macros
// the binary gets, so each case runs only where its provider exists.

#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef RAFT_OBJECT_BACKUP_PATH
#error "RAFT_OBJECT_BACKUP_PATH must name the built raft_object_backup binary"
#endif

namespace {

struct run_result {
    int status{};  ///< As from `waitpid`.
    std::string out;
    std::string err;
};

auto slurp(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// A temp file for one stream of one run, removed afterwards.
struct capture_file {
    std::string path;
    int fd{-1};

    capture_file() {
        std::string pattern = "/tmp/kythira-backup-cli-XXXXXX";
        fd = ::mkstemp(pattern.data());
        BOOST_REQUIRE(fd >= 0);
        path = pattern;
    }
    ~capture_file() {
        if (fd >= 0) {
            ::close(fd);
        }
        ::unlink(path.c_str());
    }
    capture_file(const capture_file&) = delete;
    auto operator=(const capture_file&) -> capture_file& = delete;
    capture_file(capture_file&&) = delete;
    auto operator=(capture_file&&) -> capture_file& = delete;
};

/// Runs the binary with exactly @p args and @p env, nothing inherited.
auto run(const std::vector<std::string>& args, const std::vector<std::string>& env) -> run_result {
    capture_file out_file;
    capture_file err_file;

    std::vector<char*> argv;
    std::string binary = RAFT_OBJECT_BACKUP_PATH;
    argv.push_back(binary.data());
    std::vector<std::string> arg_storage = args;
    for (auto& arg : arg_storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = env;
    std::vector<char*> envp;
    for (auto& entry : env_storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out_file.fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_file.fd, STDERR_FILENO);

    pid_t pid = 0;
    const int spawned =
        ::posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    BOOST_REQUIRE_MESSAGE(spawned == 0, "could not spawn " << binary);

    run_result result;
    BOOST_REQUIRE(::waitpid(pid, &result.status, 0) == pid);
    result.out = slurp(out_file.path);
    result.err = slurp(err_file.path);
    return result;
}

auto contains(const std::string& haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

/// `list` needs only a destination; the provider is the variable.
auto list_args(const char* provider) -> std::vector<std::string> {
    return {"list", "--provider", provider, "--dest-bucket", "backups", "--dest-prefix", "kythira"};
}

auto exited_with(const run_result& r, int code) -> bool {
    return WIFEXITED(r.status) && WEXITSTATUS(r.status) == code;
}

auto describe(const run_result& r) -> std::string {
    std::ostringstream s;
    if (WIFEXITED(r.status)) {
        s << "exit " << WEXITSTATUS(r.status);
    } else if (WIFSIGNALED(r.status)) {
        s << "killed by signal " << WTERMSIG(r.status);
    }
    s << "; stderr:\n" << r.err;
    return s.str();
}

}  // namespace

BOOST_AUTO_TEST_CASE(help_lists_credentials_for_each_compiled_in_provider) {
    const auto r = run({"--help"}, {});
    BOOST_REQUIRE_MESSAGE(exited_with(r, 0), describe(r));
    BOOST_TEST(contains(r.out, "credentials (read from the environment"));
#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
    BOOST_TEST(contains(r.out, "KYTHIRA_OCI_REGION"));
    BOOST_TEST(contains(r.out, "KYTHIRA_OCI_NAMESPACE"));
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_OSS
    BOOST_TEST(contains(r.out, "KYTHIRA_ALIBABA_ACCESS_KEY_SECRET"));
#endif
}

#ifdef KYTHIRA_BACKUP_PROVIDER_OCI

BOOST_AUTO_TEST_CASE(oci_with_no_environment_exits_1_naming_every_variable) {
    const auto r = run(list_args("oci-objectstorage"), {});
    BOOST_TEST(exited_with(r, 1), describe(r));
    for (const char* name : {"KYTHIRA_OCI_REGION", "KYTHIRA_OCI_TENANCY_ID", "KYTHIRA_OCI_USER_ID",
                             "KYTHIRA_OCI_FINGERPRINT", "KYTHIRA_OCI_PRIVATE_KEY_PEM",
                             "KYTHIRA_OCI_PRIVATE_KEY_FILE"}) {
        BOOST_TEST(contains(r.err, name), "stderr does not name " << name);
    }
}

namespace {

auto throwaway_key_pem() -> std::string {
    EVP_PKEY* key = EVP_RSA_gen(2048);
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(length));
    BIO_free(bio);
    EVP_PKEY_free(key);
    return out;
}

/// A loopback port with nothing listening: bound, read back, then closed.
auto closed_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    socklen_t len = sizeof addr;
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    ::close(fd);
    return ntohs(addr.sin_port);
}

}  // namespace

/// The regression test for the `std::terminate` defect: the namespace lookup
/// in the client's constructor fails, and the process must still exit 2 with a
/// message rather than die by SIGABRT.
BOOST_AUTO_TEST_CASE(oci_construction_failure_exits_2_with_the_namespace_hint) {
    const auto r =
        run(list_args("oci-objectstorage"),
            {
                "KYTHIRA_OCI_REGION=us-phoenix-1",
                "KYTHIRA_OCI_TENANCY_ID=ocid1.tenancy.oc1..process",
                "KYTHIRA_OCI_USER_ID=ocid1.user.oc1..process",
                "KYTHIRA_OCI_FINGERPRINT=aa:bb:cc",
                "KYTHIRA_OCI_PRIVATE_KEY_PEM=" + throwaway_key_pem(),
                "KYTHIRA_OCI_ENDPOINT_OVERRIDE=http://127.0.0.1:" + std::to_string(closed_port()),
            });
    BOOST_TEST(WIFEXITED(r.status), describe(r));
    BOOST_TEST(exited_with(r, 2), describe(r));
    BOOST_TEST(contains(r.err, "KYTHIRA_OCI_NAMESPACE"), describe(r));
}

#endif  // KYTHIRA_BACKUP_PROVIDER_OCI

#ifdef KYTHIRA_BACKUP_PROVIDER_OSS

BOOST_AUTO_TEST_CASE(oss_with_no_environment_exits_1_naming_every_variable) {
    const auto r = run(list_args("oss"), {});
    BOOST_TEST(exited_with(r, 1), describe(r));
    for (const char* name : {"KYTHIRA_ALIBABA_REGION", "KYTHIRA_ALIBABA_ACCESS_KEY_ID",
                             "KYTHIRA_ALIBABA_ACCESS_KEY_SECRET"}) {
        BOOST_TEST(contains(r.err, name), "stderr does not name " << name);
    }
}

#endif  // KYTHIRA_BACKUP_PROVIDER_OSS
