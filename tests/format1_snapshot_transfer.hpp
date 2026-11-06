// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A format-1 InstallSnapshot transfer for transport tests
// (.kiro/specs/file-install-snapshot/ task 2, Requirement 2.2).
//
// `transfer` writes a small checkpoint (zero-length files included) to a
// temp directory and cuts it into format-1 requests: the manifest on the
// first chunk, stream offsets, and chunk sizes that make at least one chunk
// span a file boundary. `receiver` is the follower end: give it to the
// transport's InstallSnapshot handler and it feeds each chunk to a real
// `snapshot_stream_writer`, so a transport that drops, reorders or alters a
// field fails on the writer's offset or SHA-256 checks, not on a hand-rolled
// comparison.
//
//   kythira::test::format1::transfer t;
//   kythira::test::format1::receiver rx;
//   server.register_install_snapshot_handler(
//       [&](const auto& req) { return rx.handle<kythira::install_snapshot_response<>>(req); });
//   for (const auto& req : t.requests<kythira::install_snapshot_request<>>(1)) {
//       auto resp = client.send_install_snapshot(1, req, timeout).get();
//       kythira::test::format1::check_ack(req, resp);
//   }
//   BOOST_TEST(rx.matches(t));

#include <raft/persistent_state_machine.hpp>
#include <raft/snapshot_stream.hpp>
#include <raft/types.hpp>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira::test::format1 {

/// A fresh directory under the system temp dir, removed on scope exit.
class temp_dir {
public:
    temp_dir() {
        auto tmpl = (std::filesystem::temp_directory_path() / "kythira-format1-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        _path = tmpl;
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

inline auto read_file(const std::filesystem::path& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The default chunk size: small enough for every transport's default
/// message limit, and coprime with the file sizes so chunks straddle files.
inline constexpr std::size_t k_default_chunk = 1531;

/// The leader end: a checkpoint on disk and the requests that carry it.
class transfer {
public:
    transfer() {
        _manifest.engine_format = "kythira-memory-kv/1";
        _manifest.range = key_range{.start = "", .end = std::nullopt};
        add("000-empty", 0, 'x');
        add("001.sst", 2000, 'a');
        add("002-empty", 0, 'y');
        add("003.sst", 4097, 'K');
        add("004.log", 1, 'z');
        add("OPTIONS", 1500, '0');
        add("zzz-empty", 0, 'q');
        _reader = std::make_unique<snapshot_stream_reader>(_src.path(), _manifest);
    }

    [[nodiscard]] auto manifest() const -> const snapshot_manifest& { return _manifest; }
    [[nodiscard]] auto directory() const -> const std::filesystem::path& { return _src.path(); }
    [[nodiscard]] auto total_bytes() const -> std::uint64_t { return _reader->total_bytes(); }

    /// The format-1 requests for the whole checkpoint, in order. Fails the
    /// test if no chunk spans a file boundary, since that is the case
    /// Requirement 2.2 asks every transport to prove.
    template<typename Request>
    [[nodiscard]] auto requests(decltype(Request{}._leader_id) leader,
                                std::size_t chunk = k_default_chunk, std::uint64_t term = 3,
                                std::uint64_t last_index = 120) const -> std::vector<Request> {
        std::vector<Request> out;
        const auto encoded = _manifest.encode();
        std::uint64_t offset = 0;
        bool spans = false;
        do {
            Request req{};
            req._term = term;
            req._leader_id = leader;
            req._last_included_index = last_index;
            req._last_included_term = term;
            req._offset = static_cast<std::size_t>(offset);
            req._data = _reader->read(offset, chunk);
            req._format = install_snapshot_format::file_set;
            if (offset == 0) {
                req._manifest.resize(encoded.size());
                std::memcpy(req._manifest.data(), encoded.data(), encoded.size());
            }
            spans = spans || crosses_file_boundary(offset, req._data.size());
            offset += req._data.size();
            req._done = offset == total_bytes();
            out.push_back(std::move(req));
        } while (offset < total_bytes());
        BOOST_REQUIRE_MESSAGE(spans, "no chunk of size " << chunk << " spans a file boundary");
        return out;
    }

private:
    auto add(const std::string& name, std::size_t size, char seed) -> void {
        std::string contents(size, '\0');
        for (std::size_t i = 0; i < size; ++i) {
            contents[i] = static_cast<char>(seed + static_cast<char>(i % 29));
        }
        std::ofstream(_src.path() / name, std::ios::binary)
            .write(contents.data(), static_cast<std::streamsize>(contents.size()));
        _manifest.files.push_back(
            checkpoint_file{.name = name, .size = size, .sha256 = sha256_of(contents)});
    }

    [[nodiscard]] auto crosses_file_boundary(std::uint64_t offset, std::size_t len) const -> bool {
        std::uint64_t start = 0;
        for (const auto& f : _manifest.files) {
            const auto end = start + f.size;
            if (f.size > 0 && offset < end && offset + len > end) {
                return true;
            }
            start = end;
        }
        return false;
    }

    temp_dir _src;
    snapshot_manifest _manifest;
    std::unique_ptr<snapshot_stream_reader> _reader;
};

/// The follower end: writes what arrives into a staging directory.
class receiver {
public:
    template<typename Response, typename Request> auto handle(const Request& req) -> Response {
        std::lock_guard lock(_mutex);
        Response resp{};
        resp._term = req.term();
        resp._group_id = req.group_id();
        try {
            if (req.format() != install_snapshot_format::file_set) {
                throw snapshot_stream_error(install_snapshot_reject_reason::format_mismatch,
                                            "not a format-1 request");
            }
            if (req.offset() == 0) {
                if (req.manifest().empty()) {
                    throw snapshot_stream_error(install_snapshot_reject_reason::missing_first_chunk,
                                                "first chunk without a manifest");
                }
                const std::string encoded(reinterpret_cast<const char*>(req.manifest().data()),
                                          req.manifest().size());
                _writer.reset();
                std::filesystem::remove_all(staging());
                _writer =
                    snapshot_stream_writer::create(staging(), snapshot_manifest::decode(encoded));
            } else if (!_writer) {
                throw snapshot_stream_error(install_snapshot_reject_reason::missing_first_chunk,
                                            "chunk before the first");
            }
            _writer->write(req.offset(), req.data());
            if (req.done()) {
                _writer->finish();
            }
            resp._bytes_received = _writer->bytes_received();
        } catch (const snapshot_stream_error& e) {
            resp._reject_reason = e.reason();
            _last_error = e.what();
        }
        return resp;
    }

    /// True when the transfer completed and every staged file equals its
    /// source.
    [[nodiscard]] auto matches(const transfer& t) const -> bool {
        std::lock_guard lock(_mutex);
        if (!_writer || !_writer->complete() || _writer->failed()) {
            return false;
        }
        for (const auto& f : t.manifest().files) {
            if (read_file(staging() / f.name) != read_file(t.directory() / f.name)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] auto last_error() const -> std::string {
        std::lock_guard lock(_mutex);
        return _last_error;
    }

private:
    [[nodiscard]] auto staging() const -> std::filesystem::path { return _dst.path() / "staging"; }

    mutable std::mutex _mutex;
    temp_dir _dst;
    std::unique_ptr<snapshot_stream_writer> _writer;
    std::string _last_error;
};

/// The response to `req` acknowledges exactly the bytes sent so far.
template<typename Request, typename Response>
auto check_ack(const Request& req, const Response& resp) -> void {
    BOOST_TEST(!resp.reject_reason().has_value());
    BOOST_REQUIRE(resp.bytes_received().has_value());
    BOOST_TEST(*resp.bytes_received() == req.offset() + req.data().size());
}

}  // namespace kythira::test::format1
