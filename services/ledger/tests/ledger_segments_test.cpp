// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Journal segments (.kiro/specs/verifiable-ledger/ task 8, Requirements
// 3.5, 3.6, 10.2): sealing blocks out of the engine into byte-identical
// segment files on every replica, reading and proving sealed blocks,
// redoing a seal a crash interrupted, and offloading segments to an object
// store and reading them back with their SHA-256 checked.

#define BOOST_TEST_MODULE ledger_segments_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>
#include <kythira/ledger/merkle.hpp>

#include <raft/key_object_store.hpp>
#include <raft/memory_kv_engine.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace kythira::ledger;
using kythira::column_family;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;

/// A fresh directory, removed at the end of the test.
struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        path = std::filesystem::temp_directory_path() / ("ledger_segments_" + new_id());
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

/// An in-memory key_object_store.
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

auto parse_result(const std::vector<std::byte>& out) -> ion_value {
    return ion_value::parse(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(out.data()), out.size()));
}

auto status_of(const std::vector<std::byte>& out) -> std::string {
    const ion_value v = parse_result(out);
    const ion_value* s = v.find("status");
    const std::string status = s != nullptr ? s->as_text().value_or("") : "";
    return status == "rejected" ? apply_result::decode(out).code : status;
}

/// A ledger replica with its own engine and segment directory.
struct replica {
    temp_dir dir;
    memory_kv_engine engine;
    std::unique_ptr<ledger_sm> sm;
    std::uint64_t index = 0;
    ledger_options options;

    explicit replica(ledger_options o = {}) : options(std::move(o)) {
        options.segment_directory = dir.path / "segments";
        reopen();
    }

    auto reopen() -> void {
        sm = std::make_unique<ledger_sm>(options);
        sm->open(engine, ledger_range("audit"));
    }

    auto apply(const std::vector<std::byte>& command) -> std::vector<std::byte> {
        return sm->apply(command, ++index);
    }
};

/// Commit `n` one-document transactions on `leader`, mirroring each entry
/// to `followers`; returns the Raft commands in order.
auto commit_blocks(replica& leader, const std::vector<replica*>& followers, int n,
                   std::int64_t& clock) -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> commands;
    for (int i = 0; i < n; ++i) {
        std::vector<ion_value> st;
        if (!leader.sm->table("t")) {
            st.push_back(statements::create_table("t"));
        }
        ion_value doc = ion_value::make_struct();
        doc.set("n", ion_value::integer(i)).set("pad", ion_value::string(std::string(200, 'p')));
        st.push_back(statements::insert("t", new_id(), std::move(doc)));
        clock += 10;
        const auto p = leader.sm->prepare(new_id(), clock, st);
        BOOST_REQUIRE(p.entry);
        BOOST_REQUIRE(apply_result::decode(leader.apply(p.command)).committed);
        for (auto* f : followers) {
            BOOST_REQUIRE(apply_result::decode(f->apply(p.command)).committed);
        }
        commands.push_back(p.command);
    }
    return commands;
}

auto journal_keys(const replica& r) -> std::size_t {
    std::size_t n = 0;
    const auto range = ledger_range("audit");
    const std::string from = range.start + "journal/";
    std::string end = from;
    end.back() = static_cast<char>(end.back() + 1);
    r.engine.scan(column_family::data, {from, end}, kythira::scan_direction::forward,
                  [&](std::string_view, std::string_view) {
                      ++n;
                      return true;
                  });
    return n;
}

auto check_every_proof(const ledger_sm& sm) -> void {
    const auto tip = sm.tip()->seq;
    const auto digest = sm.get_digest()->digest;
    for (std::uint64_t seq = 0; seq <= tip; ++seq) {
        const auto p = sm.get_block_proof(seq, tip);
        BOOST_TEST(p.blk.seq == seq);
        BOOST_TEST(p.blk.verify());
        BOOST_TEST(verify_proof(p.blk.block_hash, p.proof, digest));
        for (const auto& r : p.blk.revisions) {
            const auto rp = sm.get_revision_proof(r.table, r.id, r.version, tip);
            BOOST_REQUIRE(rp);
            BOOST_TEST(verify_proof(rp->rev.hash, rp->proof, rp->digest));
        }
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(sealing)

BOOST_AUTO_TEST_CASE(the_segment_codec_round_trips_and_rejects_damage) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 3, clock);
    std::vector<block> blocks;
    for (std::uint64_t s = 0; s < 3; ++s) {
        blocks.push_back(*r.sm->get_block(s));
    }
    const auto bytes = encode_segment(blocks);
    const auto back = decode_segment(bytes);
    BOOST_REQUIRE_EQUAL(back.size(), 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST(back[i].block_hash == blocks[i].block_hash);
    }
    BOOST_CHECK_THROW((void)decode_segment(bytes.substr(0, bytes.size() - 1)), ledger_format_error);
    BOOST_CHECK_THROW((void)decode_segment("NOTASEG!"), ledger_format_error);
    BOOST_TEST(encode_segment(back) == bytes);
}

BOOST_AUTO_TEST_CASE(a_seal_moves_blocks_into_identical_files_on_every_replica) {
    replica leader;
    replica follower;
    std::int64_t clock = t0;
    commit_blocks(leader, {&follower}, 10, clock);
    const auto digest_before = leader.sm->get_digest()->digest;

    BOOST_TEST(!leader.sm->seal_candidate());  // far below 64 MiB
    const auto candidate = leader.sm->seal_candidate(2000);
    BOOST_REQUIRE(candidate);
    BOOST_TEST(candidate->first == 0U);
    BOOST_TEST(candidate->last < 9U);

    const auto cmd = to_command(encode_seal(*candidate));
    BOOST_TEST(status_of(leader.apply(cmd)) == "sealed");
    BOOST_TEST(status_of(follower.apply(cmd)) == "sealed");

    const auto segs = leader.sm->segments();
    BOOST_REQUIRE_EQUAL(segs.size(), 1U);
    BOOST_TEST(segs[0].first == 0U);
    BOOST_TEST(segs[0].last == candidate->last);
    const auto a = read_file(leader.options.segment_directory / segs[0].file);
    const auto b = read_file(follower.options.segment_directory / segs[0].file);
    BOOST_REQUIRE(a);
    BOOST_REQUIRE(b);
    BOOST_TEST(*a == *b);
    BOOST_TEST(sha256_of(*a) == segs[0].sha256);
    BOOST_TEST(follower.sm->segments()[0].sha256 == segs[0].sha256);

    BOOST_TEST(journal_keys(leader) == 9U - candidate->last);
    BOOST_TEST(leader.sm->first_unsealed() == candidate->last + 1);
    BOOST_TEST(leader.sm->get_digest()->digest == digest_before);
    check_every_proof(*leader.sm);
    check_every_proof(*follower.sm);

    // History walks across the sealed blocks too.
    BOOST_TEST(leader.sm->history("t", std::nullopt, t0, std::nullopt).revisions.size() == 10U);

    // The next seal continues where this one ended; the rest of the
    // journal goes.
    const auto rest = to_command(encode_seal({candidate->last + 1, leader.sm->tip()->seq}));
    BOOST_TEST(status_of(leader.apply(rest)) == "sealed");
    BOOST_TEST(journal_keys(leader) == 0U);
    BOOST_TEST(leader.sm->first_unsealed() == 10U);
    check_every_proof(*leader.sm);

    // New blocks still commit after a full seal.
    commit_blocks(leader, {}, 2, clock);
    check_every_proof(*leader.sm);
}

BOOST_AUTO_TEST_CASE(a_seal_that_does_not_start_at_the_first_unsealed_block_is_rejected) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 4, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({1, 2})))) == "invalid_seal");
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 4})))) == "invalid_seal");
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 1})))) == "sealed");
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 1})))) == "invalid_seal");
    BOOST_TEST(r.sm->segments().size() == 1U);
}

BOOST_AUTO_TEST_CASE(a_seal_interrupted_before_its_batch_is_redone) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 5, clock);
    // A crash after writing part of the file: the batch never committed,
    // so the blocks are still in the journal and replay writes the file
    // again over the partial one (and over a leftover temporary).
    std::filesystem::create_directories(r.options.segment_directory);
    const auto name = segment_info::file_name(0, 2);
    write_file_durably(r.options.segment_directory / name, "KYLSEG01partial");
    write_file_durably(r.options.segment_directory / (name + ".tmp"), "junk");
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 2})))) == "sealed");
    check_every_proof(*r.sm);

    // A replayed seal entry the engine already holds is skipped.
    const auto again = r.sm->apply(to_command(encode_seal({0, 2})), r.index);
    BOOST_TEST(again.empty());
}

BOOST_AUTO_TEST_CASE(sealed_blocks_survive_a_restart) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 6, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 3})))) == "sealed");
    const auto digest = r.sm->get_digest()->digest;
    r.reopen();
    BOOST_TEST(r.sm->get_digest()->digest == digest);
    check_every_proof(*r.sm);
}

BOOST_AUTO_TEST_CASE(a_damaged_local_segment_is_a_read_error) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 4, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 2})))) == "sealed");
    const auto seg = r.sm->segments().at(0);
    auto bytes = *read_file(r.options.segment_directory / seg.file);
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x01);
    write_file_durably(r.options.segment_directory / seg.file, bytes);
    r.reopen();  // drop the cached segment
    BOOST_CHECK_THROW((void)r.sm->get_block(1), ledger_segment_error);
    BOOST_CHECK_THROW((void)r.sm->read_segment(0), ledger_segment_error);
    BOOST_TEST(r.sm->get_block(3).has_value());  // still in the journal

    std::filesystem::remove(r.options.segment_directory / seg.file);
    BOOST_CHECK_THROW((void)r.sm->get_block(0), ledger_segment_error);
}

BOOST_AUTO_TEST_CASE(a_seal_on_a_replica_without_a_segment_directory_throws) {
    memory_kv_engine engine;
    ledger_sm sm;
    sm.open(engine, ledger_range("audit"));
    const auto p = sm.prepare(new_id(), t0, {statements::create_table("t")});
    BOOST_REQUIRE(apply_result::decode(sm.apply(p.command, 1)).committed);
    BOOST_CHECK_THROW((void)sm.apply(to_command(encode_seal({0, 0})), 2),
                      kythira::persistent_storage_error);
    BOOST_TEST(sm.applied_index() == 1U);
}

BOOST_AUTO_TEST_CASE(drop_removes_the_segment_files) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 3, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 1})))) == "sealed");
    const auto file = r.options.segment_directory / r.sm->segments().at(0).file;
    BOOST_TEST(std::filesystem::exists(file));
    r.sm->drop();
    BOOST_TEST(!std::filesystem::exists(file));
    BOOST_TEST(r.sm->segments().empty());
    BOOST_TEST(!r.sm->tip());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(offload)

BOOST_AUTO_TEST_CASE(an_offloaded_segment_is_read_back_from_the_object_store) {
    replica leader;
    replica follower;
    std::int64_t clock = t0;
    commit_blocks(leader, {&follower}, 6, clock);
    const auto unsealed_before = leader.sm->stats().journal_bytes_unsealed;
    const auto seal = to_command(encode_seal({0, 3}));
    BOOST_TEST(status_of(leader.apply(seal)) == "sealed");
    BOOST_TEST(status_of(follower.apply(seal)) == "sealed");
    auto stats = leader.sm->stats();
    BOOST_TEST(stats.segments == 1U);
    BOOST_TEST(stats.journal_bytes_local == leader.sm->segments().at(0).size);
    BOOST_TEST(stats.journal_bytes_offloaded == 0U);
    BOOST_TEST(stats.journal_bytes_unsealed < unsealed_before);
    BOOST_TEST(stats.blocks == 6U);

    memory_store store;
    const segment_offload_target target{"bucket", "ledgers/audit/segments/"};
    const auto seg = leader.sm->segments().at(0);
    const auto key = upload_segment(store, target, seg, leader.sm->read_segment(0));
    BOOST_TEST(key == "ledgers/audit/segments/" + seg.file);
    BOOST_TEST(store.objects.size() == 1U);

    const auto offload = to_command(encode_offload({0, key}));
    leader.sm->set_segment_fetcher(segment_fetcher(store, "bucket"));
    follower.sm->set_segment_fetcher(segment_fetcher(store, "bucket"));
    BOOST_TEST(status_of(leader.apply(offload)) == "offloaded");
    BOOST_TEST(status_of(follower.apply(offload)) == "offloaded");
    BOOST_TEST(leader.sm->segments().at(0).object_key.value_or("") == key);
    stats = leader.sm->stats();
    BOOST_TEST(stats.journal_bytes_local == 0U);
    BOOST_TEST(stats.journal_bytes_offloaded == seg.size);

    // Both local copies are gone; reads fetch the object.
    BOOST_TEST(!std::filesystem::exists(leader.options.segment_directory / seg.file));
    BOOST_TEST(!std::filesystem::exists(follower.options.segment_directory / seg.file));
    check_every_proof(*leader.sm);
    check_every_proof(*follower.sm);

    // Offloading it again is refused, as is an unknown segment.
    BOOST_TEST(status_of(leader.apply(offload)) == "segment_offloaded");
    BOOST_TEST(status_of(leader.apply(to_command(encode_offload({9, "x"})))) ==
               "segment_not_found");

    // A tampered object fails its SHA-256.
    auto& object = store.objects.begin()->second;
    object[object.size() - 3] = static_cast<char>(object[object.size() - 3] ^ 0x01);
    leader.reopen();
    leader.sm->set_segment_fetcher(segment_fetcher(store, "bucket"));
    BOOST_CHECK_THROW((void)leader.sm->get_block(0), ledger_segment_error);
    // And a missing one is an error too.
    store.objects.clear();
    BOOST_CHECK_THROW((void)leader.sm->get_block(0), ledger_segment_error);
    // Without a fetcher an offloaded segment cannot be read.
    leader.reopen();
    BOOST_CHECK_THROW((void)leader.sm->get_block(0), ledger_segment_error);
}

BOOST_AUTO_TEST_CASE(a_kept_local_copy_is_preferred_and_backed_by_the_object) {
    ledger_options o;
    o.keep_offloaded_segments = true;
    replica r(o);
    std::int64_t clock = t0;
    commit_blocks(r, {}, 4, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 2})))) == "sealed");
    memory_store store;
    const auto seg = r.sm->segments().at(0);
    const auto key = upload_segment(store, {"b", "p/"}, seg, r.sm->read_segment(0));
    r.sm->set_segment_fetcher(segment_fetcher(store, "b"));
    BOOST_TEST(status_of(r.apply(to_command(encode_offload({0, key})))) == "offloaded");
    const auto local = r.options.segment_directory / seg.file;
    BOOST_TEST(std::filesystem::exists(local));

    // A damaged local copy falls back to the object.
    auto bytes = *read_file(local);
    bytes[10] = static_cast<char>(bytes[10] ^ 0x01);
    write_file_durably(local, bytes);
    r.reopen();
    r.sm->set_segment_fetcher(segment_fetcher(store, "b"));
    check_every_proof(*r.sm);
}

BOOST_AUTO_TEST_CASE(a_corrupted_local_copy_is_never_uploaded) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 3, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 1})))) == "sealed");
    memory_store store;
    const auto seg = r.sm->segments().at(0);
    auto bytes = r.sm->read_segment(0);
    bytes.back() = static_cast<char>(bytes.back() ^ 0x01);
    BOOST_CHECK_THROW((void)upload_segment(store, {"b", "p/"}, seg, bytes), ledger_segment_error);
    BOOST_TEST(store.objects.empty());
}

BOOST_AUTO_TEST_CASE(a_local_copy_left_by_a_crash_after_offload_is_removed_on_open) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 3, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 1})))) == "sealed");
    memory_store store;
    const auto seg = r.sm->segments().at(0);
    const auto bytes = r.sm->read_segment(0);
    const auto key = upload_segment(store, {"b", "p/"}, seg, bytes);
    BOOST_TEST(status_of(r.apply(to_command(encode_offload({0, key})))) == "offloaded");
    // Put the copy back as if the removal never ran.
    write_file_durably(r.options.segment_directory / seg.file, bytes);
    r.reopen();
    BOOST_TEST(!std::filesystem::exists(r.options.segment_directory / seg.file));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(export_journal_jobs)

namespace {

auto start_job(replica& r, export_format format, std::int64_t start, std::int64_t end)
    -> export_job {
    export_job job;
    job.job_id = new_id();
    job.start_ms = start;
    job.end_ms = end;
    job.format = format;
    job.bucket = "exports";
    job.prefix = "audit/";
    BOOST_TEST(status_of(r.apply(to_command(encode_export_start(job)))) == "export_started");
    return *r.sm->get_export(job.job_id);
}

auto split_lines(const std::string& body) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < body.size()) {
        const auto nl = body.find('\n', at);
        out.push_back(body.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

/// A store whose writes fail.
struct failing_store : memory_store {
    auto put_object(const std::string&, const std::string&, std::string_view) const
        -> kythira::put_result {
        throw std::runtime_error("store unavailable");
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(an_export_writes_each_format_with_a_manifest) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 8, clock);  // blocks 0-7 at t0+10 .. t0+80

    for (const auto format :
         {export_format::ion_text, export_format::ion_binary, export_format::json}) {
        // Blocks 2-6 by time.
        const auto job = start_job(r, format, t0 + 30, t0 + 70);
        BOOST_TEST(job.status == "in_progress");
        BOOST_TEST(job.tip.value_or(0) == 7U);
        memory_store store;
        const auto out = export_journal(*r.sm, store, job, 2);
        BOOST_TEST(out.completed);
        BOOST_TEST(out.message.empty());
        BOOST_TEST(out.objects == 3U);  // 2-3, 4-5, 6
        BOOST_TEST(store.objects.size() == 4U);

        const auto manifest =
            ion_value::parse(std::string_view(store.objects.at("exports/" + out.manifest)));
        BOOST_TEST(manifest.find("jobId")->as_text().value_or("") == job.job_id);
        BOOST_TEST(manifest.find("firstBlock")->as_int64().value_or(-1) == 2);
        BOOST_TEST(manifest.find("lastBlock")->as_int64().value_or(-1) == 6);
        BOOST_TEST(manifest.find("blocks")->as_int64().value_or(-1) == 5);
        const auto& objects = manifest.find("objects")->items();
        BOOST_REQUIRE_EQUAL(objects.size(), 3U);

        std::uint64_t next = 2;
        for (const auto& o : objects) {
            const auto key = o.find("key")->as_text().value_or("");
            BOOST_TEST(key.starts_with("audit/" + job.job_id + "/"));
            BOOST_TEST(key.ends_with(std::string(".") + std::string(export_extension(format))));
            const auto& body = store.objects.at("exports/" + key);
            BOOST_TEST(o.find("sha256")->as_text().value_or("") == to_hex(sha256_of(body)));
            BOOST_TEST(o.find("size")->as_int64().value_or(-1) ==
                       static_cast<std::int64_t>(body.size()));
            const auto first =
                static_cast<std::uint64_t>(o.find("firstBlock")->as_int64().value_or(-1));
            const auto last =
                static_cast<std::uint64_t>(o.find("lastBlock")->as_int64().value_or(-1));
            BOOST_TEST(first == next);
            next = last + 1;
            if (format == export_format::ion_binary) {
                std::string expected;
                for (auto seq = first; seq <= last; ++seq) {
                    const auto bytes = r.sm->get_block(seq)->to_ion().to_binary();
                    expected.append(bytes.begin(), bytes.end());
                }
                BOOST_TEST(body == expected);
                continue;
            }
            const auto lines = split_lines(body);
            BOOST_REQUIRE_EQUAL(lines.size(), last - first + 1);
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const auto v = ion_value::parse(std::string_view(lines[i]));
                const auto blk = *r.sm->get_block(first + i);
                if (format == export_format::ion_text) {
                    // Ion text round-trips exactly: the same Ion hash.
                    BOOST_TEST(block::from_ion(v).block_hash == blk.block_hash);
                    BOOST_TEST(v.hash() == blk.to_ion().hash());
                } else {
                    // JSON: hashes become base64 strings.
                    BOOST_TEST(v.find("seq")->as_int64().value_or(-1) ==
                               static_cast<std::int64_t>(first + i));
                    BOOST_TEST((v.find("blockHash")->type() == kythira::ledger::ion_type::string));
                }
            }
        }
        BOOST_TEST(next == 7U);

        BOOST_TEST(status_of(r.apply(to_command(encode_export_finish(out)))) == "completed");
        const auto done = *r.sm->get_export(job.job_id);
        BOOST_TEST(done.status == "completed");
        BOOST_TEST(done.objects == 3U);
        BOOST_TEST(done.manifest == out.manifest);
    }
    BOOST_TEST(r.sm->exports().size() == 3U);
}

BOOST_AUTO_TEST_CASE(an_export_covers_only_blocks_up_to_the_tip_at_its_start) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 3, clock);
    const auto job = start_job(r, export_format::ion_text, t0, t0 + 1'000'000);
    commit_blocks(r, {}, 3, clock);  // after the job began
    memory_store store;
    const auto out = export_journal(*r.sm, store, job, 100);
    BOOST_TEST(out.completed);
    const auto manifest =
        ion_value::parse(std::string_view(store.objects.at("exports/" + out.manifest)));
    BOOST_TEST(manifest.find("lastBlock")->as_int64().value_or(-1) == 2);

    // An empty range still completes, with an empty manifest.
    const auto empty = start_job(r, export_format::json, t0 - 100, t0 - 1);
    const auto none = export_journal(*r.sm, store, empty);
    BOOST_TEST(none.completed);
    BOOST_TEST(none.objects == 0U);
    const auto m = ion_value::parse(std::string_view(store.objects.at("exports/" + none.manifest)));
    BOOST_TEST(m.find("blocks")->as_int64().value_or(-1) == 0);
    BOOST_TEST(m.find("objects")->items().empty());
}

BOOST_AUTO_TEST_CASE(an_export_reads_sealed_and_offloaded_blocks) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 6, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_seal({0, 3})))) == "sealed");
    memory_store segments;
    const auto seg = r.sm->segments().at(0);
    const auto key = upload_segment(segments, {"segs", "p/"}, seg, r.sm->read_segment(0));
    r.sm->set_segment_fetcher(segment_fetcher(segments, "segs"));
    BOOST_TEST(status_of(r.apply(to_command(encode_offload({0, key})))) == "offloaded");

    const auto job = start_job(r, export_format::ion_binary, t0, clock);
    memory_store store;
    const auto out = export_journal(*r.sm, store, job, 4);
    BOOST_TEST(out.completed);
    BOOST_TEST(out.objects == 2U);

    // With the segment gone the export fails, and says why.
    segments.objects.clear();
    r.reopen();
    r.sm->set_segment_fetcher(segment_fetcher(segments, "segs"));
    const auto job2 = start_job(r, export_format::ion_binary, t0, clock);
    const auto failed = export_journal(*r.sm, store, job2, 4);
    BOOST_TEST(!failed.completed);
    BOOST_TEST(failed.message.find("offloaded copy is missing") != std::string::npos);
    BOOST_TEST(status_of(r.apply(to_command(encode_export_finish(failed)))) == "failed");
    BOOST_TEST(r.sm->get_export(job2.job_id)->message == failed.message);
}

BOOST_AUTO_TEST_CASE(export_entries_are_checked) {
    replica r;
    std::int64_t clock = t0;
    commit_blocks(r, {}, 2, clock);
    auto job = start_job(r, export_format::json, t0, clock);
    BOOST_TEST(status_of(r.apply(to_command(encode_export_start(job)))) == "export_exists");

    failing_store broken;
    const auto out = export_journal(*r.sm, broken, job);
    BOOST_TEST(!out.completed);
    BOOST_TEST(out.message == "store unavailable");
    BOOST_TEST(status_of(r.apply(to_command(encode_export_finish(out)))) == "failed");
    BOOST_TEST(status_of(r.apply(to_command(encode_export_finish(out)))) == "export_finished");

    export_outcome unknown;
    unknown.job_id = new_id();
    BOOST_TEST(status_of(r.apply(to_command(encode_export_finish(unknown)))) == "export_not_found");

    job.job_id = new_id();
    job.end_ms = job.start_ms - 1;
    BOOST_TEST(status_of(r.apply(to_command(encode_export_start(job)))) == "invalid_entry");
}

BOOST_AUTO_TEST_SUITE_END()
