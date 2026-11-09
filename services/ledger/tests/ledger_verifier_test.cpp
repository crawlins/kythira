// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Offline verification (.kiro/specs/verifiable-ledger/ task 10,
// Requirements 9.1-9.3, 12.3): journal_verifier over a state machine,
// segment files and exports; every single-byte change to a segment file
// is caught; each kind of tampered block is named; proofs check against a
// saved digest; and ledger_ctl's exit codes and messages, when the test is
// built with the tool's path.

#define BOOST_TEST_MODULE ledger_verifier_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_export.hpp>
#include <kythira/ledger/journal_segments.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/key_object_store.hpp>
#include <raft/memory_kv_engine.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/wait.h>

using namespace kythira::ledger;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;

struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        path = std::filesystem::temp_directory_path() / ("ledger_verifier_" + new_id());
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
    auto list_keys(const std::string& /*bucket*/, const std::string& /*prefix*/) const
        -> std::vector<std::string> {
        return {};
    }
    [[nodiscard]] auto provider_name() const -> std::string_view { return "memory"; }
};

auto to_command(const std::vector<std::uint8_t>& bytes) -> std::vector<std::byte> {
    std::vector<std::byte> out(bytes.size());
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

/// A ledger of `n` blocks with a mix of inserts, replaces and deletes,
/// with segments in its own directory.
struct journal {
    temp_dir dir;
    memory_kv_engine engine;
    std::unique_ptr<ledger_sm> sm;
    std::uint64_t index = 0;
    std::int64_t clock = t0;

    explicit journal(int n) {
        ledger_options o;
        o.segment_directory = dir.path / "segments";
        sm = std::make_unique<ledger_sm>(o);
        sm->open(engine, ledger_range("audit"));
        std::vector<std::string> ids;
        for (int i = 0; i < n; ++i) {
            std::vector<ion_value> st;
            if (i == 0) {
                st.push_back(statements::create_table("t"));
            }
            ion_value doc = ion_value::make_struct();
            doc.set("n", ion_value::integer(i))
                .set("note", ion_value::string("block " + std::to_string(i)));
            if (i % 3 == 2 && !ids.empty()) {
                st.push_back(statements::replace("t", ids.back(), doc));
            } else if (i % 5 == 4 && ids.size() > 1) {
                st.push_back(statements::remove("t", ids.front()));
                ids.erase(ids.begin());
            } else {
                ids.push_back(new_id());
                st.push_back(statements::insert("t", ids.back(), doc));
            }
            clock += 10;
            const auto p = sm->prepare(new_id(), clock, st);
            BOOST_REQUIRE(p.entry);
            BOOST_REQUIRE(apply_result::decode(sm->apply(p.command, ++index)).committed);
        }
    }

    auto seal(std::uint64_t first, std::uint64_t last) -> void {
        sm->apply(to_command(encode_seal({first, last})), ++index);
    }

    [[nodiscard]] auto blocks() const -> std::vector<block> {
        std::vector<block> out;
        for (std::uint64_t s = 0; s <= sm->tip()->seq; ++s) {
            out.push_back(*sm->get_block(s));
        }
        return out;
    }

    [[nodiscard]] auto saved(std::uint64_t tip) const -> saved_digest {
        return {tip, sm->digest_at(tip)};
    }
};

auto run(const std::vector<block>& blocks, std::optional<saved_digest> saved = {})
    -> std::optional<verify_failure> {
    journal_verifier v(blocks.empty() ? 0 : blocks.front().seq, saved);
    for (const auto& b : blocks) {
        if (!v.add(b)) {
            break;
        }
    }
    v.finish();
    return v.failure();
}

auto failed_at(const std::optional<verify_failure>& f) -> std::int64_t {
    return f && f->seq ? static_cast<std::int64_t>(*f->seq) : -1;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(verifying)

BOOST_AUTO_TEST_CASE(a_good_journal_verifies_with_its_digest) {
    journal j(12);
    j.seal(0, 5);
    journal_verifier v(0, j.saved(11));
    BOOST_TEST(verify_state_machine(*j.sm, v));
    BOOST_TEST(v.blocks() == 12U);
    BOOST_TEST(v.digest_checked());
    BOOST_TEST(*v.digest() == j.sm->get_digest()->digest);

    // A saved digest at an earlier tip is checked when the verifier gets
    // there.
    BOOST_TEST(!run(j.blocks(), j.saved(4)));
}

BOOST_AUTO_TEST_CASE(a_wrong_or_unreachable_digest_fails) {
    journal j(6);
    auto bad = j.saved(3);
    bad.digest[0] ^= 0x01U;
    auto f = run(j.blocks(), bad);
    BOOST_REQUIRE(f);
    BOOST_TEST(f->reason.find("not the saved") != std::string::npos);

    f = run(j.blocks(), saved_digest{9, j.sm->get_digest()->digest});
    BOOST_REQUIRE(f);
    BOOST_TEST(f->reason.find("beyond the journal") != std::string::npos);

    // A journal that starts after block 0 checks blocks, not digests.
    auto tail = j.blocks();
    tail.erase(tail.begin(), tail.begin() + 2);
    BOOST_TEST(!run(tail));
    f = run(tail, j.saved(5));
    BOOST_REQUIRE(f);
    BOOST_TEST(f->reason.find("from block 0") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(each_kind_of_tampering_names_the_block) {
    journal j(8);
    const auto good = j.blocks();
    BOOST_REQUIRE(!run(good));

    {  // revision data
        auto b = good;
        b[3].revisions.at(0).data->set("n", ion_value::integer(999));
        BOOST_TEST(failed_at(run(b)) == 3);
    }
    {  // a statement
        auto b = good;
        b[4].statements.at(0).set("table", ion_value::string("u"));
        BOOST_TEST(failed_at(run(b)) == 4);
    }
    {  // the timestamp, rehashed so only the chain or order can catch it
        auto b = good;
        b[5].timestamp = b[4].timestamp - 1;
        BOOST_TEST(failed_at(run(b)) == 5);
    }
    {  // a block rewritten consistently: its own hashes match, the next
       // block's previous hash does not
        auto b = good;
        b[2].revisions.at(0).data->set("n", ion_value::integer(-1));
        b[2].revisions.at(0).hash = b[2].revisions.at(0).compute_hash();
        b[2].entries_hash = b[2].compute_entries_hash();
        b[2].block_hash = b[2].compute_block_hash();
        BOOST_TEST(failed_at(run(b)) == 3);
        // ...and rewriting every later block to match moves the failure to
        // the digest.
        for (std::size_t i = 3; i < b.size(); ++i) {
            b[i].previous_block_hash = b[i - 1].block_hash;
            b[i].block_hash = b[i].compute_block_hash();
        }
        BOOST_TEST(!run(b));
        const auto f = run(b, j.saved(7));
        BOOST_REQUIRE(f);
        BOOST_TEST(!f->seq);
    }
    {  // a missing block
        auto b = good;
        b.erase(b.begin() + 6);
        BOOST_TEST(failed_at(run(b)) == 6);
    }
    {  // swapped blocks
        auto b = good;
        std::swap(b[1], b[2]);
        BOOST_TEST(failed_at(run(b)) == 1);
    }
    {  // the labels outside the revision hash: table and block address
        auto b = good;
        b[1].revisions.at(0).table = "u";
        BOOST_TEST(failed_at(run(b)) == 1);
        b = good;
        b[2].revisions.at(0).seq = 1;
        BOOST_TEST(failed_at(run(b)) == 2);
        BOOST_TEST(!b[2].verify());
    }
    {  // the block hash itself
        auto b = good;
        b[7].block_hash[31] ^= 0x01U;
        BOOST_TEST(failed_at(run(b)) == 7);
    }
}

BOOST_AUTO_TEST_CASE(every_byte_of_a_segment_file_is_covered) {
    journal j(5);
    j.seal(0, 4);
    const auto seg = j.sm->segments().at(0);
    const auto bytes = *read_file(j.dir.path / "segments" / seg.file);
    const auto digest = j.saved(4);
    std::size_t undetected = 0;
    for (std::size_t at = 0; at < bytes.size(); ++at) {
        auto changed = bytes;
        changed[at] = static_cast<char>(changed[at] ^ 0x01);
        bool caught = false;
        try {
            caught = run(decode_segment(changed), digest).has_value();
        } catch (const std::exception&) {
            caught = true;  // not even a readable segment
        }
        if (!caught) {
            ++undetected;
            BOOST_TEST_MESSAGE("undetected change at byte " << at);
        }
    }
    BOOST_TEST(undetected == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(sources)

BOOST_AUTO_TEST_CASE(a_segment_directory_reads_back_in_order) {
    journal j(9);
    j.seal(0, 2);
    j.seal(3, 6);
    const auto blocks = read_segment_directory(j.dir.path / "segments");
    BOOST_REQUIRE_EQUAL(blocks.size(), 7U);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        BOOST_TEST(blocks[i].seq == i);
    }
    BOOST_TEST(!run(blocks, j.saved(6)));
}

BOOST_AUTO_TEST_CASE(an_export_directory_verifies_and_refuses_json) {
    journal j(7);
    for (const auto format : {export_format::ion_text, export_format::ion_binary}) {
        temp_dir out;
        export_job job;
        job.job_id = new_id();
        job.start_ms = t0;
        job.end_ms = j.clock;
        job.format = format;
        job.bucket = "b";
        job.prefix = "x/";
        job.tip = j.sm->tip()->seq;
        memory_store store;
        BOOST_REQUIRE(export_journal(*j.sm, store, job, 3).completed);
        for (const auto& [key, body] : store.objects) {
            write_file_durably(out.path / std::filesystem::path(key).filename(), body);
        }
        const auto [manifest, blocks] = read_export_directory(out.path);
        BOOST_TEST(blocks.size() == 7U);
        BOOST_TEST(!run(blocks, j.saved(6)));

        // An object changed without the manifest is caught by its SHA-256.
        const auto object =
            out.path /
            std::filesystem::path(
                manifest.find("objects")->items().at(1).find("key")->as_text().value_or(""))
                .filename();
        auto body = *read_file(object);
        body[body.size() / 2] = static_cast<char>(body[body.size() / 2] ^ 0x01);
        write_file_durably(object, body);
        BOOST_CHECK_THROW((void)read_export_directory(out.path), ledger_segment_error);
    }

    temp_dir json;
    export_job job;
    job.job_id = new_id();
    job.start_ms = t0;
    job.end_ms = j.clock;
    job.format = export_format::json;
    job.bucket = "b";
    job.prefix = "";
    memory_store store;
    BOOST_REQUIRE(export_journal(*j.sm, store, job).completed);
    for (const auto& [key, body] : store.objects) {
        write_file_durably(json.path / std::filesystem::path(key).filename(), body);
    }
    BOOST_CHECK_THROW((void)read_export_directory(json.path), ledger_format_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(proofs)

BOOST_AUTO_TEST_CASE(block_and_revision_proofs_check_offline) {
    journal j(10);
    j.seal(0, 4);
    const auto saved = j.saved(8);
    for (std::uint64_t seq = 0; seq <= 8; ++seq) {
        const auto p = j.sm->get_block_proof(seq, 8);
        proof_document doc{p.blk, std::nullopt, p.proof, p.digest, 8};
        // Through Ion, as ledger_ctl reads it.
        const auto back =
            proof_document::from_ion(ion_value::parse(std::string_view(doc.to_ion().to_text())));
        BOOST_TEST(!check_proof(back, saved));
        for (const auto& r : p.blk.revisions) {
            const auto rp = j.sm->get_revision_proof(r.table, r.id, r.version, 8);
            BOOST_REQUIRE(rp);
            const proof_document rdoc{std::nullopt, rp->rev, rp->proof, rp->digest, 8};
            BOOST_TEST(!check_proof(proof_document::from_ion(rdoc.to_ion()), saved));
        }
    }

    const auto p = j.sm->get_block_proof(3, 8);
    proof_document doc{p.blk, std::nullopt, p.proof, p.digest, 8};
    {
        auto bad = doc;
        bad.blk->revisions.at(0).data->set("n", ion_value::integer(-7));
        BOOST_TEST(check_proof(bad, saved).has_value());
    }
    {
        auto bad = doc;
        bad.proof.at(0)[0] ^= 0x01U;
        BOOST_TEST(check_proof(bad, saved).has_value());
    }
    {
        auto other = saved;
        other.digest[5] ^= 0x01U;
        BOOST_TEST(check_proof(doc, other).has_value());
        BOOST_TEST(check_proof(doc, j.saved(9)).has_value());  // a different tip
    }
    {
        const auto rp =
            *j.sm->get_revision_proof(p.blk.revisions.at(0).table, p.blk.revisions.at(0).id,
                                      p.blk.revisions.at(0).version, 8);
        proof_document rdoc{std::nullopt, rp.rev, rp.proof, rp.digest, 8};
        rdoc.rev->data->set("note", ion_value::string("forged"));
        BOOST_TEST(check_proof(rdoc, saved).has_value());
    }
    BOOST_CHECK_THROW((void)proof_document::from_ion(
                          ion_value::parse(std::string_view("{proof: [], digest: {{}}, tip: 1}"))),
                      std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef KYTHIRA_LEDGER_CTL

BOOST_AUTO_TEST_SUITE(ledger_ctl_tool)

namespace {

/// Run ledger_ctl with `args`; its exit status.
auto ledger_ctl(const std::string& args, const std::filesystem::path& log) -> int {
    const std::string cmd =
        std::string(KYTHIRA_LEDGER_CTL) + " " + args + " >" + log.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

auto write_ion(const std::filesystem::path& p, const ion_value& v) -> void {
    write_file_durably(p, v.to_text());
}

}  // namespace

BOOST_AUTO_TEST_CASE(exit_codes_and_messages) {
    journal j(6);
    j.seal(0, 5);
    temp_dir work;
    const auto log = work.path / "out.txt";
    const auto digest = work.path / "digest.ion";
    write_ion(digest, j.saved(5).to_ion());
    const auto segs = (j.dir.path / "segments").string();

    BOOST_TEST(ledger_ctl("verify --segments " + segs + " --digest " + digest.string(), log) == 0);
    BOOST_TEST(read_file(log)->find("verified 6 blocks") != std::string::npos);

    const auto proof = work.path / "proof.ion";
    const auto p = j.sm->get_block_proof(2, 5);
    write_ion(proof, proof_document{p.blk, std::nullopt, p.proof, p.digest, 5}.to_ion());
    BOOST_TEST(
        ledger_ctl("prove --proof " + proof.string() + " --digest " + digest.string(), log) == 0);

    // Tamper with block 3's revision inside the segment and re-frame it,
    // so the file still parses: the failure names block 3.
    auto blocks = read_segment_directory(segs);
    blocks.at(3).revisions.at(0).data->set("n", ion_value::integer(12345));
    const auto file = std::filesystem::path(segs) / j.sm->segments().at(0).file;
    write_file_durably(file, encode_segment(blocks));
    BOOST_TEST(ledger_ctl("verify --segments " + segs, log) == 1);
    BOOST_TEST(read_file(log)->find("FAILED at block 3") != std::string::npos);

    // A wrong digest fails the proof.
    auto other = j.saved(5);
    other.digest[0] ^= 0x01U;
    write_ion(digest, other.to_ion());
    BOOST_TEST(
        ledger_ctl("prove --proof " + proof.string() + " --digest " + digest.string(), log) == 1);

    BOOST_TEST(ledger_ctl("verify", log) == 2);
    BOOST_TEST(ledger_ctl("frobnicate", log) == 2);
    BOOST_TEST(ledger_ctl("--help", log) == 0);
}

BOOST_AUTO_TEST_SUITE_END()

#endif
