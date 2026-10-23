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
// Registered by `cmd/raft_object_backup/CMakeLists.txt` only when the S3, OCI
// or OSS provider is compiled in, with the same KYTHIRA_BACKUP_PROVIDER_*
// macros the binary gets, so each case runs only where its provider exists.
//
// The S3 cases (`.kiro/specs/object-backup-s3-endpoint/`, Requirement 5) run
// the binary against an in-process cpp-httplib server bound for `localhost`,
// so they prove `--endpoint` and `--region` reach the wire without an account.

#ifdef KYTHIRA_BACKUP_PROVIDER_OCI
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif
#ifdef KYTHIRA_BACKUP_PROVIDER_S3
#include <httplib.h>

#include <filesystem>
#include <mutex>
#include <thread>
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

#ifdef KYTHIRA_BACKUP_PROVIDER_OCI

// object-backup-s3-endpoint Requirement 2: a flag the OCI arm would ignore is
// refused, naming OCI's own variable, before any client exists.
BOOST_AUTO_TEST_CASE(oci_refuses_endpoint_and_region_flags_naming_its_own_variables) {
    auto endpoint_args = list_args("oci-objectstorage");
    endpoint_args.insert(endpoint_args.end(), {"--endpoint", "http://x"});
    const auto e = run(endpoint_args, {});
    BOOST_TEST(exited_with(e, 1), describe(e));
    BOOST_TEST(contains(e.err, "--endpoint applies to --provider s3 only"), describe(e));
    BOOST_TEST(contains(e.err, "KYTHIRA_OCI_ENDPOINT_OVERRIDE"), describe(e));

    auto region_args = list_args("oci-objectstorage");
    region_args.insert(region_args.end(), {"--region", "us-phoenix-1"});
    const auto r = run(region_args, {});
    BOOST_TEST(exited_with(r, 1), describe(r));
    BOOST_TEST(contains(r.err, "KYTHIRA_OCI_REGION"), describe(r));
}

#endif  // KYTHIRA_BACKUP_PROVIDER_OCI

#ifdef KYTHIRA_BACKUP_PROVIDER_S3

namespace {

struct recorded_request {
    std::string method;
    std::string path;
    std::string query_list_type;
    std::string host;
    std::string authorization;
};

/// A stand-in S3 service on `localhost`: records every request and answers
/// each GET with an empty ListObjectsV2 result. `localhost`, not 127.0.0.1,
/// because the SDK goes path style for IP literals by itself, so only a host
/// name proves the client's own path-style switch (Requirement 5.2).
class fake_s3 {
public:
    fake_s3() {
        _server.Get(".*", [this](const httplib::Request& req, httplib::Response& res) {
            record(req);
            res.set_content(R"(<?xml version="1.0" encoding="UTF-8"?>)"
                            R"(<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)"
                            R"(<Name>backups</Name><Prefix></Prefix><KeyCount>0</KeyCount>)"
                            R"(<MaxKeys>1000</MaxKeys><IsTruncated>false</IsTruncated>)"
                            R"(</ListBucketResult>)",
                            "application/xml");
        });
        _server.set_error_handler(
            [this](const httplib::Request& req, httplib::Response&) { record(req); });
        _port = _server.bind_to_any_port("localhost");
        BOOST_REQUIRE(_port > 0);
        _thread = std::thread([this] { _server.listen_after_bind(); });
        _server.wait_until_ready();
    }
    ~fake_s3() {
        _server.stop();
        _thread.join();
    }
    fake_s3(const fake_s3&) = delete;
    auto operator=(const fake_s3&) -> fake_s3& = delete;
    fake_s3(fake_s3&&) = delete;
    auto operator=(fake_s3&&) -> fake_s3& = delete;

    [[nodiscard]] auto endpoint() const -> std::string {
        return "http://localhost:" + std::to_string(_port);
    }
    [[nodiscard]] auto port() const -> int { return _port; }
    [[nodiscard]] auto requests() -> std::vector<recorded_request> {
        std::lock_guard lock(_mutex);
        return _requests;
    }

private:
    void record(const httplib::Request& req) {
        std::lock_guard lock(_mutex);
        _requests.push_back({req.method, req.path, req.get_param_value("list-type"),
                             req.get_header_value("Host"), req.get_header_value("Authorization")});
    }

    httplib::Server _server;
    int _port{};
    std::thread _thread;
    std::mutex _mutex;
    std::vector<recorded_request> _requests;
};

/// Static keys and a HOME with no `.aws`, so the SDK chain finds credentials
/// without reaching a profile, and nothing on the developer's machine leaks in.
auto s3_env() -> std::vector<std::string> {
    static const auto home = [] {
        auto dir = std::filesystem::temp_directory_path() / "kythira-backup-cli-empty-home";
        std::filesystem::create_directories(dir);
        return dir.string();
    }();
    return {"AWS_ACCESS_KEY_ID=AKIDPROCESSTEST", "AWS_SECRET_ACCESS_KEY=process-test-secret",
            "HOME=" + home};
}

auto s3_list_args(std::initializer_list<std::string> extra) -> std::vector<std::string> {
    auto args = list_args("s3");
    args.insert(args.end(), extra);
    return args;
}

}  // namespace

// Requirement 5.2. The region is deliberately not us-east-1: that is also the
// SDK's last-resort default, so it would pass even if --region were dropped.
BOOST_AUTO_TEST_CASE(s3_endpoint_and_region_reach_the_wire_path_style) {
    fake_s3 server;
    const auto r =
        run(s3_list_args({"--endpoint", server.endpoint(), "--region", "eu-west-3"}), s3_env());
    BOOST_TEST(exited_with(r, 0), describe(r));
    const auto requests = server.requests();
    BOOST_REQUIRE_MESSAGE(!requests.empty(), "no request reached the server; " << describe(r));
    const auto& first = requests.front();
    BOOST_TEST(first.method == "GET");
    BOOST_TEST(first.path == "/backups");
    BOOST_TEST(first.query_list_type == "2");
    BOOST_TEST(first.host == "localhost:" + std::to_string(server.port()));
    BOOST_TEST(contains(first.authorization, "/eu-west-3/s3/aws4_request"),
               "Authorization was: " << first.authorization);
}

// Requirement 3.1/3.2: a shape error is exit 1 before any connection.
BOOST_AUTO_TEST_CASE(s3_endpoint_without_a_scheme_exits_1_before_connecting) {
    fake_s3 server;
    const auto r =
        run(s3_list_args({"--endpoint", "localhost:" + std::to_string(server.port())}), s3_env());
    BOOST_TEST(exited_with(r, 1), describe(r));
    BOOST_TEST(contains(r.err, "needs a scheme"), describe(r));
    BOOST_TEST(server.requests().empty());
}

// Requirement 3.3 and 5.3: the flag wins over AWS_ENDPOINT_URL, which is not
// read, and nothing warns about it when --endpoint is given.
BOOST_AUTO_TEST_CASE(s3_endpoint_flag_wins_over_aws_endpoint_url) {
    fake_s3 server;
    auto env = s3_env();
    env.emplace_back("AWS_ENDPOINT_URL=http://secret-host.example");
    const auto r = run(s3_list_args({"--endpoint", server.endpoint()}), env);
    BOOST_TEST(exited_with(r, 0), describe(r));
    BOOST_TEST(!server.requests().empty(), describe(r));
    BOOST_TEST(!contains(r.err, "AWS_ENDPOINT_URL"), describe(r));
    BOOST_TEST(!contains(r.err, "secret-host"), describe(r));
}

BOOST_AUTO_TEST_CASE(help_lists_the_region_and_endpoint_flags) {
    const auto r = run({"--help"}, {});
    BOOST_REQUIRE_MESSAGE(exited_with(r, 0), describe(r));
    BOOST_TEST(contains(r.out, "--region <name>"));
    BOOST_TEST(contains(r.out, "--endpoint <url>"));
    BOOST_TEST(contains(r.out, "AWS_ENDPOINT_URL is not read"));
}

#endif  // KYTHIRA_BACKUP_PROVIDER_S3
