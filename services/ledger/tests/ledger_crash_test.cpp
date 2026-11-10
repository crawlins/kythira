// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Crash tests for the ledger state machine (.kiro/specs/verifiable-ledger/
// task 11, Requirement 12.4): a replica on RocksDB is killed at each of
// the state machine's kill points, at each time it passes one, while it
// applies a fixed run of Raft entries (commits, seals, an offload). It is
// then reopened and given the same entries again, as Raft replays the log
// after a restart, and must end with exactly the journal of a replica that
// never crashed: the same blocks, digest and segments, every block
// re-hashing, and no stray files in its segment directory.
//
// The kill is a real process exit (`std::_Exit` from a libfiu failpoint, so
// nothing is flushed or destroyed): the test re-runs its own binary as a
// worker, chosen by environment variables a global fixture reads before any
// test case runs. Kill points (`fiu_do_on("ledger/...")`):
//   ledger/apply/before_engine_write   the entry worked out, nothing written
//   ledger/apply/after_engine_write    the batch written, memory not updated
//                                      (for an offload: the local copy not
//                                      yet removed)
//   ledger/seal/before_segment_file    a seal before its segment file
//   ledger/segment/before_rename       the segment file's bytes written to
//                                      its temporary name only
//   ledger/seal/after_segment_file     the segment file complete, the
//                                      seal's batch not written

#define BOOST_TEST_MODULE ledger_crash_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/key_object_store.hpp>
#include <raft/memory_kv_engine.hpp>
#include <raft/rocksdb_kv_engine.hpp>

#include <fiu-control.h>
#include <fiu.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration)

using namespace kythira::ledger;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;
constexpr const char* env_dir = "KYTHIRA_LEDGER_CRASH_DIR";
constexpr const char* env_point = "KYTHIRA_LEDGER_CRASH_POINT";
constexpr const char* env_hit = "KYTHIRA_LEDGER_CRASH_HIT";

/// An in-memory key_object_store: offloaded segments are read back from
/// it when the recovered replica is checked.
struct memory_store {
    mutable std::map<std::string, std::string> objects;

    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> kythira::put_result {
        objects[bucket + "/" + key] = std::string(bytes);
        return {"v1"};
    }
    auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<kythira::get_result> {
        const auto it = objects.find(bucket + "/" + key);
        if (it == objects.end()) {
            return std::nullopt;
        }
        return kythira::get_result{it->second, "v1"};
    }
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        objects.erase(bucket + "/" + key);
    }
    auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        std::vector<std::string> out;
        for (const auto& [k, v] : objects) {
            if (k.starts_with(bucket + "/" + prefix)) {
                out.push_back(k.substr(bucket.size() + 1));
            }
        }
        return out;
    }
    [[nodiscard]] auto provider_name() const -> std::string_view { return "memory"; }
};
static_assert(kythira::key_object_store<memory_store>);

auto to_command(const std::vector<std::uint8_t>& bytes) -> std::vector<std::byte> {
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

// ── The entries, written to a file the worker reads ─────────────────────────

auto save_entries(const std::filesystem::path& path,
                  const std::vector<std::vector<std::byte>>& entries) -> void {
    std::ofstream out(path, std::ios::binary);
    for (const auto& e : entries) {
        const auto n = static_cast<std::uint32_t>(e.size());
        out.write(reinterpret_cast<const char*>(&n), sizeof n);
        out.write(reinterpret_cast<const char*>(e.data()), static_cast<std::streamsize>(e.size()));
    }
}

auto load_entries(const std::filesystem::path& path) -> std::vector<std::vector<std::byte>> {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::vector<std::byte>> out;
    std::uint32_t n = 0;
    while (in.read(reinterpret_cast<char*>(&n), sizeof n)) {
        std::vector<std::byte> e(n);
        in.read(reinterpret_cast<char*>(e.data()), n);
        out.push_back(std::move(e));
    }
    return out;
}

/// A replica on RocksDB in `dir`: engine/ and segments/.
struct disk_replica {
    std::unique_ptr<kythira::rocksdb_kv_engine> engine;
    std::unique_ptr<basic_ledger_state_machine<kythira::rocksdb_kv_engine>> sm;

    explicit disk_replica(const std::filesystem::path& dir) {
        std::filesystem::create_directories(dir);
        engine = std::make_unique<kythira::rocksdb_kv_engine>(dir / "engine");
        ledger_options o;
        o.segment_directory = dir / "segments";
        sm = std::make_unique<basic_ledger_state_machine<kythira::rocksdb_kv_engine>>(o);
        sm->open(*engine, ledger_range("audit"));
    }

    /// Apply entries 1..n as Raft does after a restart: an entry at or
    /// below the applied index is a replay the state machine skips.
    auto apply_all(const std::vector<std::vector<std::byte>>& entries) -> void {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            (void)sm->apply(entries[i], i + 1);
        }
    }
};

// ── The worker: apply the entries until the armed kill point fires ──────────

std::atomic<int> g_countdown{0};

auto countdown_hit(const char* /*name*/, int* failnum, void** /*failinfo*/, unsigned* /*flags*/)
    -> int {
    *failnum = 1;
    return g_countdown.fetch_sub(1) == 1 ? 1 : 0;
}

/// Runs in the re-executed binary; never returns. Exit 0 means the kill
/// point was never reached that many times; ledger_crash_exit_code means
/// it fired.
[[noreturn]] auto run_worker(const char* dir, const char* point, const char* hit) -> void {
    try {
        const std::filesystem::path root(dir);
        const auto entries = load_entries(root / "entries.bin");
        disk_replica r(root / "replica");
        g_countdown = std::atoi(hit);
        fiu_init(0);
        fiu_enable_external(point, 1, nullptr, 0, countdown_hit);
        r.apply_all(entries);
        std::_Exit(0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crash worker: %s\n", e.what());
        std::_Exit(3);
    }
}

struct worker_dispatch {
    worker_dispatch() {
        const char* dir = std::getenv(env_dir);  // NOLINT(concurrency-mt-unsafe)
        if (dir != nullptr) {
            run_worker(dir,
                       std::getenv(env_point),  // NOLINT(concurrency-mt-unsafe)
                       std::getenv(env_hit));   // NOLINT(concurrency-mt-unsafe)
        }
    }
};

/// Start this binary as a worker and return its exit status.
auto run_crash(const std::filesystem::path& dir, const std::string& point, int hit) -> int {
    std::vector<std::string> env_strs;
    for (char** e = environ; *e != nullptr; ++e) {
        env_strs.emplace_back(*e);
    }
    env_strs.push_back(std::string(env_dir) + "=" + dir.string());
    env_strs.push_back(std::string(env_point) + "=" + point);
    env_strs.push_back(std::string(env_hit) + "=" + std::to_string(hit));
    std::vector<char*> envp;
    for (auto& s : env_strs) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);
    std::string self = std::filesystem::read_symlink("/proc/self/exe").string();
    std::vector<char*> argv{self.data(), nullptr};
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execve(self.c_str(), argv.data(), envp.data());
        std::_Exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

// ── The reference run ───────────────────────────────────────────────────────

/// A fresh directory, removed at the end of the test.
struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        path = std::filesystem::temp_directory_path() / ("ledger_crash_" + new_id());
        std::filesystem::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    temp_dir(const temp_dir&) = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
};

/// The entries, and a replica that applied them without a crash: 12
/// one-document commits with three seals between them and an offload of
/// the first segment (uploaded to `store` first, as the node does).
struct reference {
    temp_dir dir;
    memory_store store;
    kythira::memory_kv_engine engine;
    std::unique_ptr<basic_ledger_state_machine<kythira::memory_kv_engine>> sm;
    std::vector<std::vector<std::byte>> entries;
    std::size_t seals = 0;
    std::size_t offloads = 0;

    reference() {
        ledger_options o;
        o.segment_directory = dir.path / "segments";
        sm = std::make_unique<basic_ledger_state_machine<kythira::memory_kv_engine>>(o);
        sm->open(engine, ledger_range("audit"));
        std::int64_t clock = t0;
        const auto apply = [&](std::vector<std::byte> e) {
            (void)sm->apply(e, entries.size() + 1);
            entries.push_back(std::move(e));
        };
        const auto commit = [&](int n) {
            std::vector<ion_value> st;
            if (!sm->table("t")) {
                st.push_back(statements::create_table("t"));
            }
            ion_value doc = ion_value::make_struct();
            doc.set("n", ion_value::integer(n))
                .set("pad", ion_value::string(std::string(300, 'p')));
            st.push_back(statements::insert("t", new_id(), std::move(doc)));
            clock += 10;
            const auto p = sm->prepare(new_id(), clock, st);
            BOOST_REQUIRE(p.entry);
            apply(p.command);
        };
        for (int i = 0; i < 4; ++i) {
            commit(i);
        }
        apply(to_command(encode_seal({0, 3})));
        ++seals;
        const auto seg = sm->segments().at(0);
        const auto key = upload_segment(store, {"b", "audit/"}, seg, sm->read_segment(0));
        sm->set_segment_fetcher(segment_fetcher(store, "b"));
        apply(to_command(encode_offload({0, key})));
        ++offloads;
        for (int i = 4; i < 8; ++i) {
            commit(i);
        }
        apply(to_command(encode_seal({4, 7})));
        ++seals;
        for (int i = 8; i < 12; ++i) {
            commit(i);
        }
        apply(to_command(encode_seal({8, 9})));
        ++seals;
        BOOST_REQUIRE(sm->tip());
        BOOST_REQUIRE(sm->tip()->seq == 11U);
        BOOST_REQUIRE(sm->segments().size() == 3U);
    }
};

/// Kill a fresh replica at the `hit`th pass of `point`, recover it, and
/// check its journal against the reference.
auto crash_and_recover(const reference& ref, const std::string& point, int hit) -> void {
    BOOST_TEST_CONTEXT("kill point " << point << ", pass " << hit) {
        const temp_dir dir;
        save_entries(dir.path / "entries.bin", ref.entries);
        BOOST_TEST(run_crash(dir.path, point, hit) == ledger_crash_exit_code);

        disk_replica r(dir.path / "replica");
        r.sm->set_segment_fetcher(segment_fetcher(ref.store, "b"));
        r.apply_all(ref.entries);

        // The same journal: tip, digest, every block's hash, the segments.
        BOOST_REQUIRE(r.sm->tip());
        BOOST_TEST(r.sm->tip()->seq == ref.sm->tip()->seq);
        BOOST_TEST(to_hex(r.sm->get_digest()->digest) == to_hex(ref.sm->get_digest()->digest));
        for (std::uint64_t seq = 0; seq <= ref.sm->tip()->seq; ++seq) {
            const auto ours = r.sm->get_block(seq);
            const auto theirs = ref.sm->get_block(seq);
            BOOST_REQUIRE(ours && theirs);
            BOOST_TEST(to_hex(ours->block_hash) == to_hex(theirs->block_hash), "block " << seq);
        }
        const auto ours = r.sm->segments();
        const auto theirs = ref.sm->segments();
        BOOST_REQUIRE(ours.size() == theirs.size());
        for (std::size_t i = 0; i < ours.size(); ++i) {
            BOOST_TEST(ours[i].file == theirs[i].file);
            BOOST_TEST(ours[i].sha256 == theirs[i].sha256);
            BOOST_TEST(ours[i].object_key.value_or("") == theirs[i].object_key.value_or(""));
        }
        journal_verifier v(0, std::nullopt);
        BOOST_TEST(verify_state_machine(*r.sm, v));
        BOOST_TEST(r.sm->stats().hash_mismatches == 0U);

        // The segment directory holds exactly the local segments: no
        // temporary file, no offloaded copy left behind.
        std::vector<std::string> files;
        for (const auto& e :
             std::filesystem::directory_iterator(dir.path / "replica" / "segments")) {
            files.push_back(e.path().filename().string());
        }
        std::ranges::sort(files);
        std::vector<std::string> expected;
        for (const auto& seg : theirs) {
            if (!seg.object_key) {
                expected.push_back(seg.file);
            }
        }
        std::ranges::sort(expected);
        BOOST_TEST(files == expected, std::filesystem::path(dir.path / "replica" / "segments"));
    }
}

}  // namespace

BOOST_GLOBAL_FIXTURE(worker_dispatch);

BOOST_AUTO_TEST_CASE(a_replica_killed_at_any_apply_recovers_the_same_journal) {
    const reference ref;
    const auto n = static_cast<int>(ref.entries.size());
    for (int hit = 1; hit <= n; ++hit) {
        crash_and_recover(ref, "ledger/apply/before_engine_write", hit);
        crash_and_recover(ref, "ledger/apply/after_engine_write", hit);
    }
}

BOOST_AUTO_TEST_CASE(a_replica_killed_while_sealing_recovers_the_same_journal) {
    const reference ref;
    const auto seals = static_cast<int>(ref.seals);
    for (int hit = 1; hit <= seals; ++hit) {
        crash_and_recover(ref, "ledger/seal/before_segment_file", hit);
        crash_and_recover(ref, "ledger/segment/before_rename", hit);
        crash_and_recover(ref, "ledger/seal/after_segment_file", hit);
    }
}

BOOST_AUTO_TEST_CASE(a_kill_point_past_the_last_pass_lets_the_run_finish) {
    // The harness's own check: a worker that never reaches its kill point
    // exits 0, so the cases above really did die where they say.
    const reference ref;
    const temp_dir dir;
    save_entries(dir.path / "entries.bin", ref.entries);
    BOOST_TEST(run_crash(dir.path, "ledger/seal/after_segment_file",
                         static_cast<int>(ref.seals) + 1) == 0);
}
