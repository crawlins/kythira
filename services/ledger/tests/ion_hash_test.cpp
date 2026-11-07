// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Ion hash (`.kiro/specs/verifiable-ledger/` task 2, Requirement 12.1):
// every case of the amazon-ion/ion-hash-test suite (vectors/, format in the
// file's header) with an identity or MD5 expectation, then the properties
// the ledger relies on: encoding independence, the SHA-256 composition, UTC
// timestamps, long values, and rejection of bad input.

#define BOOST_TEST_MODULE ion_hash_test

#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_hash.hpp>

#include <openssl/evp.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ledger = kythira::ledger;
using bytes = std::vector<std::uint8_t>;

namespace {

auto read_file(const std::filesystem::path& p) -> bytes {
    std::ifstream in(p, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in.good(), "cannot open " << p);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto text(std::string_view s) -> std::span<const std::uint8_t> {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

auto hex(const bytes& b) -> std::string {
    return ledger::to_hex(b);
}

auto identity() -> ledger::ion_hash_function {
    return [](std::span<const std::uint8_t> in) { return bytes(in.begin(), in.end()); };
}

auto md5() -> ledger::ion_hash_function {
    return [](std::span<const std::uint8_t> in) {
        bytes out(16);
        unsigned int length = 0;
        EVP_Digest(in.data(), in.size(), out.data(), &length, EVP_md5(), nullptr);
        out.resize(length);
        return out;
    };
}

auto check(iERR err) -> void {
    BOOST_REQUIRE_EQUAL(std::string(ion_error_to_str(err)), "IERR_OK");
}

/// The bytes of the sexp of ints the reader is on, e.g. (0x0b 0x20 0x0e).
auto read_byte_sexp(hREADER reader) -> bytes {
    bytes out;
    check(ion_reader_step_in(reader));
    ION_TYPE t = nullptr;
    for (;;) {
        check(ion_reader_next(reader, &t));
        if (t == tid_EOF) {
            break;
        }
        int v = 0;
        check(ion_reader_read_int(reader, &v));
        out.push_back(static_cast<std::uint8_t>(v));
    }
    check(ion_reader_step_out(reader));
    return out;
}

auto first_annotation(hREADER reader) -> std::string {
    SIZE count = 0;
    check(ion_reader_get_annotation_count(reader, &count));
    if (count == 0) {
        return {};
    }
    ION_STRING s;
    check(ion_reader_get_an_annotation(reader, 0, &s));
    return {reinterpret_cast<const char*>(s.value), static_cast<std::size_t>(s.length)};
}

auto field_name(hREADER reader) -> std::string {
    ION_STRING s;
    check(ion_reader_get_field_name(reader, &s));
    return {reinterpret_cast<const char*>(s.value), static_cast<std::size_t>(s.length)};
}

/// The final digest the `algorithm` expectation names: the last digest:: or
/// final_digest:: in its sexp, with the reader on the `expect` struct.
auto expected_digest(hREADER reader, std::string_view algorithm) -> std::optional<bytes> {
    std::optional<bytes> result;
    check(ion_reader_step_in(reader));
    ION_TYPE t = nullptr;
    for (;;) {
        check(ion_reader_next(reader, &t));
        if (t == tid_EOF) {
            break;
        }
        if (field_name(reader) != algorithm) {
            continue;
        }
        check(ion_reader_step_in(reader));
        for (;;) {
            check(ion_reader_next(reader, &t));
            if (t == tid_EOF) {
                break;
            }
            const std::string kind = first_annotation(reader);
            bytes b = read_byte_sexp(reader);
            if (kind == "digest" || kind == "final_digest") {
                result = std::move(b);
            }
        }
        check(ion_reader_step_out(reader));
    }
    check(ion_reader_step_out(reader));
    return result;
}

struct suite_result {
    int checked = 0;
    int failed = 0;
};

/// Run every case of the suite that has an expectation for `algorithm`.
auto run_suite(std::string_view algorithm, const ledger::ion_hash_function& h) -> suite_result {
    const ledger::ion_hasher hasher(h);
    bytes file =
        read_file(std::filesystem::path(KYTHIRA_LEDGER_VECTORS_DIR) / "ion_hash_tests.ion");
    ION_READER_OPTIONS options = ledger::ledger_ion_reader_options();
    hREADER reader = nullptr;
    check(ion_reader_open_buffer(&reader, file.data(), static_cast<SIZE>(file.size()), &options));
    kythira::detail::ion_reader_handle guard{reader};

    suite_result result;
    int index = 0;
    ION_TYPE t = nullptr;
    for (;;) {
        check(ion_reader_next(reader, &t));
        if (t == tid_EOF) {
            break;
        }
        ++index;
        std::string name = first_annotation(reader);
        std::optional<bytes> actual;
        std::optional<bytes> expected;
        std::string error;
        check(ion_reader_step_in(reader));
        ION_TYPE ft = nullptr;
        for (;;) {
            check(ion_reader_next(reader, &ft));
            if (ft == tid_EOF) {
                break;
            }
            const std::string field = field_name(reader);
            try {
                if (field == "ion") {
                    actual = hasher.hash_current(reader);
                } else if (field == "10n") {
                    bytes binary{0xE0, 0x01, 0x00, 0xEA};
                    const bytes body = read_byte_sexp(reader);
                    binary.insert(binary.end(), body.begin(), body.end());
                    actual = ledger::ion_hash_with(hasher, binary);
                }
            } catch (const ledger::ion_hash_error& e) {
                error = e.what();
            }
            if (field == "expect") {
                expected = expected_digest(reader, algorithm);
            }
        }
        check(ion_reader_step_out(reader));
        if (!expected) {
            continue;
        }
        if (name.empty()) {
            name = "case " + std::to_string(index);
        }
        ++result.checked;
        if (!actual || *actual != *expected) {
            ++result.failed;
            BOOST_ERROR(std::string(algorithm) + " " + name + ": expected " + hex(*expected) +
                        ", got " + (actual ? hex(*actual) : "error: " + error));
        }
    }
    return result;
}

auto identity_of(std::string_view ion) -> std::string {
    return hex(ledger::ion_hash_with(ledger::ion_hasher(identity()), text(ion)));
}

}  // namespace

BOOST_AUTO_TEST_CASE(ion_hash_test_suite_identity) {
    const auto r = run_suite("identity", identity());
    BOOST_TEST_MESSAGE("identity cases checked: " << r.checked);
    BOOST_TEST(r.failed == 0);
    BOOST_TEST(r.checked >= 160);  // the suite has 169; fewer means the runner skipped cases
}

BOOST_AUTO_TEST_CASE(ion_hash_test_suite_md5) {
    const auto r = run_suite("md5", md5());
    BOOST_TEST_MESSAGE("md5 cases checked: " << r.checked);
    BOOST_TEST(r.failed == 0);
    BOOST_TEST(r.checked >= 5);
}

BOOST_AUTO_TEST_CASE(hash_ignores_encoding_and_field_order) {
    const ledger::hash256 a =
        ledger::ion_hash("{name: \"x\", n: 1, tags: [a, b], when: 2026-01-02T03:04Z}");
    const ledger::hash256 b =
        ledger::ion_hash("{when: 2026-01-02T03:04:00Z, tags:[a,b], n:1, name:\"x\"}");
    // 03:04 and 03:04:00 differ in precision, so these two are different values.
    BOOST_TEST(a != b);
    const ledger::hash256 c =
        ledger::ion_hash("{tags:[a,b], 'when': 2026-01-02T03:04Z, \"n\":1, name:'''x'''}");
    BOOST_TEST(a == c);

    // The same value in binary, written by ion-c.
    hWRITER writer = nullptr;
    ION_STREAM* stream = nullptr;
    check(ion_stream_open_memory_only(&stream));
    ION_WRITER_OPTIONS wopts;
    std::memset(&wopts, 0, sizeof(wopts));
    wopts.output_as_binary = TRUE;
    check(ion_writer_open(&writer, stream, &wopts));
    ION_STRING s;
    check(ion_writer_start_container(writer, tid_STRUCT));
    check(
        ion_writer_write_field_name(writer, ion_string_assign_cstr(&s, const_cast<char*>("n"), 1)));
    check(ion_writer_write_int(writer, 1));
    check(ion_writer_write_field_name(writer,
                                      ion_string_assign_cstr(&s, const_cast<char*>("name"), 4)));
    check(ion_writer_write_string(writer, ion_string_assign_cstr(&s, const_cast<char*>("x"), 1)));
    check(ion_writer_write_field_name(writer,
                                      ion_string_assign_cstr(&s, const_cast<char*>("tags"), 4)));
    check(ion_writer_start_container(writer, tid_LIST));
    check(ion_writer_write_symbol(writer, ion_string_assign_cstr(&s, const_cast<char*>("a"), 1)));
    check(ion_writer_write_symbol(writer, ion_string_assign_cstr(&s, const_cast<char*>("b"), 1)));
    check(ion_writer_finish_container(writer));
    check(ion_writer_write_field_name(writer,
                                      ion_string_assign_cstr(&s, const_cast<char*>("when"), 4)));
    ION_TIMESTAMP ts;
    check(ion_timestamp_for_minute(&ts, 2026, 1, 2, 3, 4));
    check(ion_timestamp_set_local_offset(&ts, 0));
    check(ion_writer_write_timestamp(writer, &ts));
    check(ion_writer_finish_container(writer));
    check(ion_writer_close(writer));
    POSITION length = ion_stream_get_position(stream);
    check(ion_stream_seek(stream, 0));
    bytes binary(static_cast<std::size_t>(length));
    SIZE got = 0;
    check(ion_stream_read(stream, binary.data(), static_cast<SIZE>(binary.size()), &got));
    check(ion_stream_close(stream));
    binary.resize(static_cast<std::size_t>(got));
    BOOST_REQUIRE(binary.size() > 4);
    BOOST_TEST((binary[0] == 0xE0 && binary[3] == 0xEA));
    BOOST_TEST(ledger::to_hex(ledger::ion_hash(binary)) == ledger::to_hex(a));
}

BOOST_AUTO_TEST_CASE(sha256_hash_composes_as_specified) {
    // H({a:1}) = sha256(0B D0 escape(sha256(s(a) || s(1))) 0E).
    const bytes field = {0x0B, 0x70, 0x61, 0x0E, 0x0B, 0x20, 0x01, 0x0E};
    const ledger::hash256 field_hash = ledger::sha256(field);
    bytes s = {0x0B, 0xD0};
    for (const std::uint8_t b : field_hash) {
        if (b == 0x0B || b == 0x0C || b == 0x0E) {
            s.push_back(0x0C);
        }
        s.push_back(b);
    }
    s.push_back(0x0E);
    BOOST_TEST(ledger::to_hex(ledger::ion_hash("{a:1}")) == ledger::to_hex(ledger::sha256(s)));
    BOOST_TEST(ledger::to_hex(ledger::ion_hash("5")) ==
               ledger::to_hex(ledger::sha256(bytes{0x0B, 0x20, 0x05, 0x0E})));
}

BOOST_AUTO_TEST_CASE(timestamps_hash_in_utc) {
    // 2000-01-01T00:30+01:00 is 1999-12-31T23:30Z: offset +60 (0xBC), year
    // 1999 (0x0F 0xCF), 12 (0x8C), 31 (0x9F), 23 (0x97), 30 (0x9E).
    BOOST_TEST(identity_of("2000-01-01T00:30+01:00") == "0b60bc0fcf8c9f979e0e");
    // A negative offset rolls forward across a year: 1999-12-31T23:00-02:00
    // is 2000-01-01T01:00Z; offset -120 is 0x40 0xF8 (0x40 the sign, 120 =
    // 0x78 in the low bits of the second byte), year 2000 0x0F 0xD0, then
    // 1, 1, 1 and 0.
    BOOST_TEST(identity_of("1999-12-31T23:00-02:00") == "0b6040f80fd0818181800e");
    BOOST_TEST(identity_of("2000-01-01T00:30+01:00") != identity_of("1999-12-31T23:30Z"));
}

BOOST_AUTO_TEST_CASE(long_values_are_read_in_full) {
    // Longer than ion-c's 4 KiB single-value threshold, and than the 64 KiB
    // pieces a long text string is read in.
    for (const std::size_t n : {std::size_t{20000}, std::size_t{200000}}) {
        std::string big;
        for (std::size_t i = 0; i < n; ++i) {
            big.push_back(static_cast<char>('a' + (i % 26)));
        }
        const bytes s =
            ledger::ion_hash_with(ledger::ion_hasher(identity()), text("\"" + big + "\""));
        BOOST_REQUIRE_EQUAL(s.size(), big.size() + 3);
        BOOST_TEST(std::string(s.begin() + 2, s.end() - 1) == big);
    }
    for (const std::size_t n : {std::size_t{8000}, std::size_t{200000}}) {
        // n base64 characters are 3n/4 bytes, none of them a marker byte
        // ('A' decodes to zeros).
        const bytes blob = ledger::ion_hash_with(ledger::ion_hasher(identity()),
                                                 text("{{" + std::string(n, 'A') + "}}"));
        BOOST_TEST(blob.size() == (n / 4 * 3) + 3);
    }
    // The binary encodings, with a VarUInt length (type descriptor L=14).
    for (const std::uint8_t type : {std::uint8_t{0x80}, std::uint8_t{0xA0}}) {
        const std::size_t n = 200000;
        bytes binary{0xE0, 0x01, 0x00, 0xEA, static_cast<std::uint8_t>(type | 0x0E)};
        binary.push_back(static_cast<std::uint8_t>(n >> 14U));
        binary.push_back(static_cast<std::uint8_t>((n >> 7U) & 0x7FU));
        binary.push_back(static_cast<std::uint8_t>((n & 0x7FU) | 0x80U));
        binary.insert(binary.end(), n, 'q');
        const bytes s = ledger::ion_hash_with(ledger::ion_hasher(identity()), binary);
        BOOST_REQUIRE_EQUAL(s.size(), n + 3);
        BOOST_TEST(s[1] == type);
        BOOST_TEST((s[2] == 'q' && s[n + 1] == 'q'));
    }
    // An int wider than 64 bits, by magnitude.
    BOOST_TEST(identity_of("123456789012345678901234567890123456789") ==
               "0b205ce0e9a56015fec5aadfa328ae3981150e");
}

BOOST_AUTO_TEST_CASE(bad_input_is_rejected) {
    BOOST_CHECK_THROW((void)ledger::ion_hash(""), ledger::ion_hash_error);
    BOOST_CHECK_THROW((void)ledger::ion_hash("1 2"), ledger::ion_hash_error);
    BOOST_CHECK_THROW((void)ledger::ion_hash("{a:"), ledger::ion_hash_error);
    // A binary string declaring 13 bytes with one present.
    BOOST_CHECK_THROW((void)ledger::ion_hash(bytes{0xE0, 0x01, 0x00, 0xEA, 0x8D, 0x41}),
                      ledger::ion_hash_error);
    // Deeper than the reader allows.
    BOOST_CHECK_THROW((void)ledger::ion_hash(std::string(100, '[') + std::string(100, ']')),
                      ledger::ion_hash_error);
    BOOST_CHECK_NO_THROW((void)ledger::ion_hash(std::string(20, '[') + std::string(20, ']')));
}
