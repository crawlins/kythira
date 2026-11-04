// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Unit tests for the format-1 InstallSnapshot stream reader and writer
// (.kiro/specs/file-install-snapshot/ task 3).

#include <raft/snapshot_stream.hpp>

#define BOOST_TEST_MODULE snapshot_stream_test
#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using kythira::install_snapshot_reject_reason;
using kythira::snapshot_manifest;
using kythira::snapshot_stream_error;
using kythira::snapshot_stream_reader;
using kythira::snapshot_stream_writer;
using kythira::snapshot_stream_writer_options;

namespace {

/// A fresh directory under the system temp dir, removed on scope exit.
class temp_dir {
public:
    temp_dir() {
        auto tmpl = (fs::temp_directory_path() / "kythira-snapstream-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        _path = tmpl;
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;

    [[nodiscard]] auto path() const -> const fs::path& { return _path; }

private:
    fs::path _path;
};

auto write_file(const fs::path& p, const std::string& contents) -> void {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

auto read_file(const fs::path& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto pattern(std::size_t n, char seed) -> std::string {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        s[i] = static_cast<char>(seed + static_cast<char>(i % 23));
    }
    return s;
}

struct checkpoint {
    snapshot_manifest manifest;
    std::string stream;  ///< All files concatenated in manifest order.
};

/// Writes the named files into `dir` and builds their manifest.
auto make_checkpoint(const fs::path& dir,
                     const std::vector<std::pair<std::string, std::string>>& files) -> checkpoint {
    checkpoint cp;
    cp.manifest.engine_format = "kythira-memory-kv/1";
    cp.manifest.range = kythira::key_range{.start = "a", .end = std::string("m")};
    for (const auto& [name, contents] : files) {
        write_file(dir / name, contents);
        cp.manifest.files.push_back(kythira::checkpoint_file{
            .name = name, .size = contents.size(), .sha256 = kythira::sha256_of(contents)});
        cp.stream += contents;
    }
    return cp;
}

/// Zero-length files at the start, middle and end, and files of awkward
/// sizes so chunks of most lengths span a boundary.
auto mixed_files() -> std::vector<std::pair<std::string, std::string>> {
    return {{"000-empty.sst", ""},           {"001.sst", pattern(1000, 'a')},
            {"002-empty.sst", ""},           {"003.sst", pattern(17, 'k')},
            {"004.log", pattern(4096, 'A')}, {"005.sst", pattern(1, 'z')},
            {"OPTIONS", pattern(333, '0')},  {"zzz-empty", ""}};
}

auto as_bytes(const std::string& s) -> std::span<const std::byte> {
    return std::as_bytes(std::span(s.data(), s.size()));
}

auto to_string(const std::vector<std::byte>& b) -> std::string {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/// Streams `cp` from the reader into the writer in chunks of `chunk`.
auto pump(const snapshot_stream_reader& reader, snapshot_stream_writer& writer, std::size_t chunk,
          std::uint64_t stop_at = UINT64_MAX) -> void {
    while (writer.bytes_received() < reader.total_bytes() && writer.bytes_received() < stop_at) {
        const auto want = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk, stop_at - writer.bytes_received()));
        const auto data = reader.read(writer.bytes_received(), want);
        BOOST_REQUIRE(!data.empty());
        writer.write(writer.bytes_received(), data);
    }
}

template<typename Fn> auto expect_reason(Fn&& fn, install_snapshot_reject_reason want) -> void {
    try {
        std::forward<Fn>(fn)();
        BOOST_ERROR("expected snapshot_stream_error with reason " << kythira::to_string(want));
    } catch (const snapshot_stream_error& e) {
        BOOST_TEST(kythira::to_string(e.reason()) == kythira::to_string(want), e.what());
    }
}

}  // namespace

// ── Reader ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(reader_reads_whole_stream_at_every_chunk_size) {
    temp_dir src;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    BOOST_TEST(reader.total_bytes() == cp.stream.size());

    for (const std::size_t chunk : {1UL, 2UL, 7UL, 17UL, 999UL, 1000UL, 1001UL, 4096UL, 100000UL}) {
        std::string got;
        std::uint64_t off = 0;
        while (off < reader.total_bytes()) {
            const auto data = reader.read(off, chunk);
            BOOST_REQUIRE(!data.empty());
            BOOST_REQUIRE(data.size() <= chunk);
            got += to_string(data);
            off += data.size();
        }
        BOOST_TEST((got == cp.stream), "chunk size " << chunk);
    }
}

BOOST_AUTO_TEST_CASE(reader_random_offsets_match_concatenation) {
    temp_dir src;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    for (std::uint64_t off = 0; off <= cp.stream.size(); off += 37) {
        const auto data = reader.read(off, 1500);
        BOOST_TEST((to_string(data) == cp.stream.substr(off, 1500)), "offset " << off);
    }
}

BOOST_AUTO_TEST_CASE(reader_end_and_past_end) {
    temp_dir src;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    BOOST_TEST(reader.read(reader.total_bytes(), 100).empty());
    expect_reason([&] { (void)reader.read(reader.total_bytes() + 1, 100); },
                  install_snapshot_reject_reason::offset_mismatch);
}

BOOST_AUTO_TEST_CASE(reader_refuses_missing_or_resized_file) {
    temp_dir src;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    write_file(src.path() / "003.sst", "short");
    expect_reason([&] { snapshot_stream_reader r(src.path(), cp.manifest); },
                  install_snapshot_reject_reason::io_error);
    fs::remove(src.path() / "003.sst");
    expect_reason([&] { snapshot_stream_reader r(src.path(), cp.manifest); },
                  install_snapshot_reject_reason::io_error);
}

BOOST_AUTO_TEST_CASE(reader_of_empty_checkpoint) {
    temp_dir src;
    const auto cp = make_checkpoint(src.path(), {});
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    BOOST_TEST(reader.total_bytes() == 0U);
    BOOST_TEST(reader.read(0, 10).empty());
}

// ── Writer ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(writer_round_trip_with_chunks_spanning_files) {
    for (const std::size_t chunk : {1UL, 13UL, 1000UL, 1018UL, 5000UL, 1UL << 20}) {
        temp_dir src;
        temp_dir dst;
        const auto cp = make_checkpoint(src.path(), mixed_files());
        const snapshot_stream_reader reader(src.path(), cp.manifest);
        const auto staging = dst.path() / "staging";
        auto writer = snapshot_stream_writer::create(staging, cp.manifest);
        pump(reader, *writer, chunk);
        BOOST_TEST(writer->complete());
        BOOST_TEST(!writer->failed());
        BOOST_TEST(writer->bytes_received() == cp.stream.size());
        writer->finish();
        for (const auto& f : cp.manifest.files) {
            BOOST_TEST((read_file(staging / f.name) == read_file(src.path() / f.name)),
                       f.name << " chunk " << chunk);
        }
        BOOST_TEST(fs::exists(staging / "MANIFEST.kythira"));
    }
}

BOOST_AUTO_TEST_CASE(writer_zero_length_files_only) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), {{"a", ""}, {"b", ""}});
    auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest);
    BOOST_TEST(writer->complete());
    writer->finish();
    BOOST_TEST(fs::exists(dst.path() / "s" / "a"));
    BOOST_TEST(fs::exists(dst.path() / "s" / "b"));
    BOOST_TEST(fs::file_size(dst.path() / "s" / "b") == 0U);
}

BOOST_AUTO_TEST_CASE(writer_refuses_out_of_order_chunks) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest);
    const auto head = cp.stream.substr(0, 100);
    writer->write(0, as_bytes(head));
    // A gap, a rewind and a repeat are all offset mismatches, and none of
    // them poison the transfer: the right offset still works afterwards.
    expect_reason([&] { writer->write(150, as_bytes(head)); },
                  install_snapshot_reject_reason::offset_mismatch);
    expect_reason([&] { writer->write(0, as_bytes(head)); },
                  install_snapshot_reject_reason::offset_mismatch);
    BOOST_TEST(!writer->failed());
    const auto next = cp.stream.substr(100, 50);
    writer->write(100, as_bytes(next));
    BOOST_TEST(writer->bytes_received() == 150U);
}

BOOST_AUTO_TEST_CASE(writer_refuses_chunk_past_end_of_stream) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), {{"one", "abc"}});
    auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest);
    expect_reason([&] { writer->write(0, as_bytes(std::string("abcd"))); },
                  install_snapshot_reject_reason::offset_mismatch);
}

BOOST_AUTO_TEST_CASE(writer_detects_corrupt_chunk) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest);
    auto bad = cp.stream.substr(0, 1000);  // exactly 001.sst
    bad[500] ^= 0x01;
    expect_reason([&] { writer->write(0, as_bytes(bad)); },
                  install_snapshot_reject_reason::checksum);
    BOOST_TEST(writer->failed());
    expect_reason([&] { writer->write(1000, as_bytes(cp.stream.substr(1000, 10))); },
                  install_snapshot_reject_reason::io_error);
    expect_reason([&] { writer->finish(); }, install_snapshot_reject_reason::io_error);
}

BOOST_AUTO_TEST_CASE(writer_finish_before_complete_throws) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest);
    writer->write(0, as_bytes(cp.stream.substr(0, 10)));
    expect_reason([&] { writer->finish(); }, install_snapshot_reject_reason::io_error);
}

BOOST_AUTO_TEST_CASE(writer_refuses_invalid_manifest_before_creating_anything) {
    temp_dir src;
    temp_dir dst;
    const auto base = make_checkpoint(src.path(), {{"one", "abc"}}).manifest;

    auto escape = base;
    escape.files.at(0).name = "../escape";
    auto reserved = base;
    reserved.files.at(0).name = "MANIFEST.kythira";
    auto overflow = base;
    overflow.files.at(0).size = UINT64_MAX;
    overflow.files.push_back({.name = "two", .size = 2, .sha256 = {}});
    auto no_format = base;
    no_format.engine_format.clear();

    for (const auto* m : {&escape, &reserved, &overflow, &no_format}) {
        expect_reason([&] { (void)snapshot_stream_writer::create(dst.path() / "s", *m); },
                      install_snapshot_reject_reason::manifest_invalid);
        BOOST_TEST(!fs::exists(dst.path() / "s"));
    }
}

BOOST_AUTO_TEST_CASE(writer_checks_disk_space_with_margin) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), {{"one", pattern(1000, 'q')}});
    snapshot_stream_writer_options opts;
    opts.disk_margin_bytes = 500;
    opts.available_bytes = [](const fs::path&) -> std::uint64_t { return 1499; };
    expect_reason(
        [&] { (void)snapshot_stream_writer::create(dst.path() / "s", cp.manifest, opts); },
        install_snapshot_reject_reason::disk_space);
    BOOST_TEST(!fs::exists(dst.path() / "s"));

    opts.available_bytes = [](const fs::path&) -> std::uint64_t { return 1500; };
    auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest, opts);
    BOOST_TEST(writer->total_bytes() == 1000U);
}

BOOST_AUTO_TEST_CASE(writer_refuses_existing_staging_directory) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), {{"one", "abc"}});
    fs::create_directory(dst.path() / "s");
    expect_reason([&] { (void)snapshot_stream_writer::create(dst.path() / "s", cp.manifest); },
                  install_snapshot_reject_reason::io_error);
}

// ── Reopen ──────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(reopen_continues_from_written_prefix) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    const auto staging = dst.path() / "s";
    // Stop part-way through 004.log (stream bytes 1017..5113).
    const std::uint64_t stop = 1017 + 1234;
    {
        auto writer = snapshot_stream_writer::create(staging, cp.manifest);
        pump(reader, *writer, 300, stop);
        BOOST_TEST(writer->bytes_received() == stop);
    }
    auto writer = snapshot_stream_writer::reopen(staging);
    BOOST_TEST(writer->bytes_received() == stop);
    BOOST_TEST((writer->manifest() == cp.manifest));
    pump(reader, *writer, 777);
    BOOST_TEST(writer->complete());
    writer->finish();
    for (const auto& f : cp.manifest.files) {
        BOOST_TEST((read_file(staging / f.name) == read_file(src.path() / f.name)), f.name);
    }
}

BOOST_AUTO_TEST_CASE(reopen_rolls_back_to_last_verified_file) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    const auto staging = dst.path() / "s";
    {
        auto writer = snapshot_stream_writer::create(staging, cp.manifest);
        pump(reader, *writer, 4096, 1017 + 4096 + 1);  // into 005.sst's end
    }
    // Damage 003.sst (stream bytes 1000..1017) after it was verified.
    auto damaged = read_file(staging / "003.sst");
    damaged[3] ^= 0x10;
    write_file(staging / "003.sst", damaged);

    auto writer = snapshot_stream_writer::reopen(staging);
    BOOST_TEST(writer->bytes_received() == 1000U);
    BOOST_TEST(!fs::exists(staging / "004.log"));
    pump(reader, *writer, 1024);
    BOOST_TEST(writer->complete());
    writer->finish();
    BOOST_TEST((read_file(staging / "003.sst") == read_file(src.path() / "003.sst")));
}

BOOST_AUTO_TEST_CASE(reopen_of_finished_transfer_is_complete) {
    temp_dir src;
    temp_dir dst;
    const auto cp = make_checkpoint(src.path(), mixed_files());
    const snapshot_stream_reader reader(src.path(), cp.manifest);
    {
        auto writer = snapshot_stream_writer::create(dst.path() / "s", cp.manifest);
        pump(reader, *writer, 2048);
        writer->finish();
    }
    auto writer = snapshot_stream_writer::reopen(dst.path() / "s");
    BOOST_TEST(writer->complete());
    BOOST_TEST(writer->bytes_received() == cp.stream.size());
}

BOOST_AUTO_TEST_CASE(reopen_without_valid_manifest_throws) {
    temp_dir dst;
    fs::create_directory(dst.path() / "s");
    expect_reason([&] { (void)snapshot_stream_writer::reopen(dst.path() / "s"); },
                  install_snapshot_reject_reason::manifest_invalid);
    write_file(dst.path() / "s" / "MANIFEST.kythira", "not a manifest");
    expect_reason([&] { (void)snapshot_stream_writer::reopen(dst.path() / "s"); },
                  install_snapshot_reject_reason::manifest_invalid);
}
