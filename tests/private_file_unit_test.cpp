// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE private_file_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/private_file.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// write_file_atomically() exists so private keys are never readable by other
// users, not even between creation and a later chmod. These tests pin the mode
// a file is created with (under a permissive umask that would otherwise leave
// it world-readable), that a replace never exposes a partial or wrong-mode
// file, and that a symlink or leftover temporary is never written through.

namespace {

auto mode_of(const std::filesystem::path& p) -> mode_t {
    struct stat st{};
    BOOST_REQUIRE(::lstat(p.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

auto read_all(const std::filesystem::path& p) -> std::string {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Runs the test body under @p mask and restores the previous umask after.
struct umask_guard {
    explicit umask_guard(mode_t mask) : previous(::umask(mask)) {}
    ~umask_guard() { ::umask(previous); }
    umask_guard(const umask_guard&) = delete;
    auto operator=(const umask_guard&) -> umask_guard& = delete;
    mode_t previous;
};

struct scratch_dir {
    scratch_dir()
        : path(raft::make_private_temp_directory(std::filesystem::temp_directory_path(),
                                                 "kythira_private_file_test_")) {}
    ~scratch_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    scratch_dir(const scratch_dir&) = delete;
    auto operator=(const scratch_dir&) -> scratch_dir& = delete;
    std::filesystem::path path;
};

auto entries_in(const std::filesystem::path& dir) -> std::size_t {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(dir),
                                                  std::filesystem::directory_iterator()));
}

}  // namespace

BOOST_AUTO_TEST_CASE(private_file_is_0600_under_permissive_umask) {
    umask_guard mask(0);
    scratch_dir dir;
    const auto key = dir.path / "key.pem";

    raft::write_private_file(key, "secret");

    BOOST_TEST(mode_of(key) == 0600U);
    BOOST_TEST(read_all(key) == "secret");
    BOOST_TEST(entries_in(dir.path) == 1U);  // No temporary left behind.
}

BOOST_AUTO_TEST_CASE(private_file_is_0600_even_when_umask_strips_owner_bits) {
    scratch_dir dir;  // Before the umask: under 0277 it would be read-only.
    umask_guard mask(0277);
    const auto key = dir.path / "key.pem";

    raft::write_private_file(key, "secret");

    BOOST_TEST(mode_of(key) == 0600U);
}

BOOST_AUTO_TEST_CASE(replace_narrows_an_existing_world_readable_file) {
    umask_guard mask(022);
    scratch_dir dir;
    const auto key = dir.path / "key.pem";
    {
        std::ofstream(key) << "old";  // 0644, as the pre-fix writers left it.
    }
    BOOST_REQUIRE(mode_of(key) == 0644U);

    raft::write_private_file(key, "new");

    BOOST_TEST(mode_of(key) == 0600U);
    BOOST_TEST(read_all(key) == "new");
    BOOST_TEST(entries_in(dir.path) == 1U);
}

BOOST_AUTO_TEST_CASE(standard_file_follows_umask) {
    umask_guard mask(022);
    scratch_dir dir;
    const auto cert = dir.path / "cert.pem";

    raft::write_file_atomically(cert, "public", raft::file_visibility::standard);

    BOOST_TEST(mode_of(cert) == 0644U);
    BOOST_TEST(read_all(cert) == "public");
}

BOOST_AUTO_TEST_CASE(symlink_at_target_is_replaced_not_followed) {
    scratch_dir dir;
    const auto victim = dir.path / "victim";
    {
        std::ofstream(victim) << "untouched";
    }
    const auto key = dir.path / "key.pem";
    std::filesystem::create_symlink(victim, key);

    raft::write_private_file(key, "secret");

    BOOST_TEST(!std::filesystem::is_symlink(key));
    BOOST_TEST(read_all(key) == "secret");
    BOOST_TEST(read_all(victim) == "untouched");
}

BOOST_AUTO_TEST_CASE(missing_parent_throws_and_leaves_nothing) {
    scratch_dir dir;
    const auto key = dir.path / "absent" / "key.pem";

    BOOST_CHECK_THROW(raft::write_private_file(key, "secret"), std::filesystem::filesystem_error);
    BOOST_TEST(entries_in(dir.path) == 0U);
}

BOOST_AUTO_TEST_CASE(private_temp_directory_is_0700_even_under_restrictive_umask) {
    scratch_dir parent;
    umask_guard mask(0277);

    const auto d = raft::make_private_temp_directory(parent.path, "x_");

    BOOST_TEST(mode_of(d) == 0700U);
}

BOOST_AUTO_TEST_CASE(private_temp_directory_is_0700_and_unique) {
    umask_guard mask(0);
    scratch_dir parent;

    const auto a = raft::make_private_temp_directory(parent.path, "x_");
    const auto b = raft::make_private_temp_directory(parent.path, "x_");

    BOOST_TEST(a != b);
    BOOST_TEST(mode_of(a) == 0700U);
    BOOST_TEST(mode_of(b) == 0700U);
    BOOST_TEST(a.filename().string().starts_with("x_"));
}
