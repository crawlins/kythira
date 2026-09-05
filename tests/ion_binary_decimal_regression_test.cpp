// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Regression coverage for vcpkg-overlays/ion-c/0003-fix-gcc-14-incompatible-
// pointer-types.patch, whose second hunk rewrites ion-c's
// ion_binary_read_int_64_and_sign() (ionc/ion_binary.c).
//
// Why this test exists at all: that patch was the one change in PR #316 that no
// automated job exercised. It is behaviour-affecting -- the unpatched code
// passes a uint64_t* where ion_stream_read_byte() takes an int*, so the callee
// writes an int through a uint64_t*, an aliasing violation that initialises
// only 4 of the variable's 8 bytes -- and until now nothing would have noticed
// if it were dropped or rewritten wrongly.
//
// Reaching the function is the hard part, and is why this test is written
// against ion-c's C API directly rather than against ion_rpc_serializer:
//
//   * The five existing ion_* tests never execute a single instruction of it.
//     That was confirmed under gdb with a positive control --
//     ion_writer_write_int64/ion_reader_read_int64 are each hit 22,123 times in
//     ion_serialization_property_test while this function is hit zero times.
//     The binary reader's int64 path does not route through it.
//   * The only reachable caller is ion_binary_read_decimal() (ion_binary.c:501),
//     decoding a decimal's mantissa, and only when that mantissa encodes in
//     <= 8 bytes; wider mantissas go to _ion_binary_read_decimal_helper()
//     instead. Nothing in kythira's own wire format emits a decimal, so no
//     serializer-level test can get here.
//
// What this test proves and does not prove. It proves the mantissa decode is
// value-correct across the branches the patch touches: the sign bit test
// (`b & 0x80`), the mask that follows it (`b &= 0x7f`), the multi-byte shift
// (`b <<= len * 8`), and the single-byte fall-through. It does NOT, on its own,
// detect a straight revert of the patch: on a little-endian host the truncating
// 4-byte write into a zero-initialised uint64_t happens to leave the right
// value, which is exactly why the bug survived upstream. The guard against a
// revert is the ion-build CI job, which installs the vcpkg `ion` feature under
// GCC 14 -- there the unpatched source is a hard compile error. The two are
// meant to be read together.
//
// Both a plain buffer reader and a chunked user-stream reader are driven, so
// the decode is covered when ion-c refills its internal page mid-document as
// well as when the whole document is resident.

#define BOOST_TEST_MODULE IonBinaryDecimalRegressionTest
#include <boost/test/unit_test.hpp>

#include <ionc/ion.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ion-c reports every failure as a non-zero iERR; BOOST_REQUIRE_EQUAL on the
// raw code gives a usable message without a wrapper type.
#define ION_REQUIRE(expr) BOOST_REQUIRE_EQUAL(static_cast<int>(expr), static_cast<int>(IERR_OK))

auto decimal_to_string(const ION_DECIMAL& value) -> std::string {
    // ION_DECIMAL_STRLEN() is not exported; decQuad's own bound is 43 bytes for
    // a 34-digit coefficient, and the widest literal used below is far inside
    // that. 256 leaves room for the decNumber-backed representation too.
    std::array<char, 256> buffer{};
    ION_REQUIRE(ion_decimal_to_string(&value, buffer.data()));
    return std::string{buffer.data()};
}

// Encodes one decimal literal as a standalone binary Ion document.
auto encode_binary_decimal(const std::string& literal) -> std::vector<BYTE> {
    ION_STREAM* stream = nullptr;
    ION_REQUIRE(ion_stream_open_memory_only(&stream));

    ION_WRITER_OPTIONS options{};
    options.output_as_binary = TRUE;
    hWRITER writer = nullptr;
    ION_REQUIRE(ion_writer_open(&writer, stream, &options));

    decContext context;
    decContextDefault(&context, DEC_INIT_DECQUAD);
    ION_DECIMAL value;
    ION_REQUIRE(ion_decimal_from_string(&value, literal.c_str(), &context));
    ION_REQUIRE(ion_writer_write_ion_decimal(writer, &value));
    ION_REQUIRE(ion_writer_close(writer));
    ION_REQUIRE(ion_decimal_free(&value));

    POSITION length = ion_stream_get_position(stream);
    std::vector<BYTE> encoded(static_cast<std::size_t>(length));
    ION_REQUIRE(ion_stream_seek(stream, 0));
    SIZE read = 0;
    ION_REQUIRE(ion_stream_read(stream, encoded.data(), static_cast<SIZE>(length), &read));
    ION_REQUIRE(ion_stream_close(stream));
    BOOST_REQUIRE_EQUAL(static_cast<std::size_t>(read), encoded.size());
    return encoded;
}

// Pulls the single decimal value back out of a reader that has already been
// opened over the encoding, whatever kind of reader that is.
auto read_single_decimal(hREADER reader) -> std::string {
    ION_TYPE type = nullptr;
    ION_REQUIRE(ion_reader_next(reader, &type));
    BOOST_REQUIRE_EQUAL(static_cast<int>(ION_TYPE_INT(type)), static_cast<int>(tid_DECIMAL_INT));

    ION_DECIMAL decoded;
    ION_REQUIRE(ion_reader_read_ion_decimal(reader, &decoded));
    std::string text = decimal_to_string(decoded);
    ION_REQUIRE(ion_decimal_free(&decoded));

    ION_REQUIRE(ion_reader_next(reader, &type));
    BOOST_REQUIRE_EQUAL(static_cast<int>(ION_TYPE_INT(type)), static_cast<int>(tid_EOF_INT));
    return text;
}

auto decode_from_buffer(std::vector<BYTE>& encoded) -> std::string {
    ION_READER_OPTIONS options{};
    hREADER reader = nullptr;
    ION_REQUIRE(ion_reader_open_buffer(&reader, encoded.data(), static_cast<SIZE>(encoded.size()),
                                       &options));
    std::string text = read_single_decimal(reader);
    ION_REQUIRE(ion_reader_close(reader));
    return text;
}

// Hands ion-c the document a few bytes at a time, so its internal page is
// refilled repeatedly while the value is being decoded. The handler owns the
// buffer it publishes, per ion_reader_open_stream()'s contract: it sets curr
// and limit, and returns IERR_EOF once the source is drained.
struct chunked_source {
    static constexpr SIZE chunk_bytes = 3;

    const std::vector<BYTE>* data = nullptr;
    std::size_t offset = 0;
    std::array<BYTE, chunk_bytes> chunk{};
};

auto chunked_handler(struct _ion_user_stream* stream) -> iERR {
    auto* source = static_cast<chunked_source*>(stream->handler_state);
    if (source->offset >= source->data->size()) {
        stream->curr = nullptr;
        stream->limit = nullptr;
        return IERR_EOF;
    }
    SIZE count = 0;
    while (count < chunked_source::chunk_bytes && source->offset < source->data->size()) {
        source->chunk[static_cast<std::size_t>(count)] = (*source->data)[source->offset];
        ++source->offset;
        ++count;
    }
    stream->curr = source->chunk.data();
    stream->limit = source->chunk.data() + count;
    return IERR_OK;
}

auto decode_from_chunked_stream(const std::vector<BYTE>& encoded) -> std::string {
    chunked_source source;
    source.data = &encoded;

    ION_READER_OPTIONS options{};
    hREADER reader = nullptr;
    ION_REQUIRE(ion_reader_open_stream(&reader, &source, chunked_handler, &options));
    std::string text = read_single_decimal(reader);
    ION_REQUIRE(ion_reader_close(reader));
    return text;
}

struct decimal_case {
    std::string_view literal;
    std::string_view what_it_covers;
};

// Mantissa widths are chosen against ion_binary_read_decimal()'s dispatch: a
// mantissa encoding in <= 8 bytes reaches the patched function, anything wider
// is handed to _ion_binary_read_decimal_helper(). The sign occupies the high
// bit of the first mantissa byte, so a magnitude whose own top bit is set costs
// an extra byte -- that is why 12345678 (0xBC614E, top bit set) is a 4-byte
// mantissa and not a 3-byte one.
constexpr std::array<decimal_case, 9> decimal_cases = {{
    {"0.", "zero mantissa: value_len == 0, the branch that never calls the patched function"},
    {"0.7", "single-byte positive mantissa: the len == 0 fall-through, sign bit clear"},
    {"-0.7", "single-byte negative mantissa: sign bit set, then masked by b &= 0x7f"},
    {"12345.678", "four-byte positive mantissa: exercises b <<= (len * 8)"},
    {"-12345.678", "four-byte negative mantissa: sign bit and the shift together"},
    {"-1.27", "two-byte mantissa whose magnitude byte is 0x7f, adjacent to the sign bit"},
    {"9223372036854775.807", "eight-byte positive mantissa: the widest that still reaches it"},
    {"-9223372036854775.807", "eight-byte negative mantissa: the widest, with the sign bit"},
    {"-18446744073709551.615", "nine-byte mantissa: routes to the helper instead, as a control"},
}};

}  // namespace

// Round-trips each literal through binary Ion and requires the decoded value to
// be both bit-equal (ion_decimal_equals) and textually identical to the parsed
// original. Textual identity is the stronger of the two here: a mantissa decode
// that lost or gained a byte would still compare unequal, but the string makes
// the failure readable.
BOOST_AUTO_TEST_CASE(binary_decimal_mantissa_round_trips) {
    decContext context;
    decContextDefault(&context, DEC_INIT_DECQUAD);

    for (const auto& test_case : decimal_cases) {
        BOOST_TEST_CONTEXT("literal=" << test_case.literal << " (" << test_case.what_it_covers
                                      << ")") {
            const std::string literal{test_case.literal};

            ION_DECIMAL expected;
            ION_REQUIRE(ion_decimal_from_string(&expected, literal.c_str(), &context));
            const std::string expected_text = decimal_to_string(expected);

            std::vector<BYTE> encoded = encode_binary_decimal(literal);
            BOOST_REQUIRE(!encoded.empty());

            BOOST_CHECK_EQUAL(decode_from_buffer(encoded), expected_text);
            BOOST_CHECK_EQUAL(decode_from_chunked_stream(encoded), expected_text);

            ION_READER_OPTIONS options{};
            hREADER reader = nullptr;
            ION_REQUIRE(ion_reader_open_buffer(&reader, encoded.data(),
                                               static_cast<SIZE>(encoded.size()), &options));
            ION_TYPE type = nullptr;
            ION_REQUIRE(ion_reader_next(reader, &type));
            ION_DECIMAL decoded;
            ION_REQUIRE(ion_reader_read_ion_decimal(reader, &decoded));

            BOOL is_equal = FALSE;
            ION_REQUIRE(ion_decimal_equals(&expected, &decoded, &context, &is_equal));
            BOOST_CHECK_MESSAGE(is_equal == TRUE, "decoded " << decimal_to_string(decoded)
                                                             << " != expected " << expected_text);

            ION_REQUIRE(ion_decimal_free(&decoded));
            ION_REQUIRE(ion_reader_close(reader));
            ION_REQUIRE(ion_decimal_free(&expected));
        }
    }
}

// The sign is carried by the high bit of the mantissa's first byte, and the
// patch's `b & 0x80` / `b &= 0x7f` pair is the only thing separating it from
// the magnitude. A magnitude that differs from its negation only in that bit is
// the case that would break first if the mask were dropped or misapplied, so it
// is asserted directly rather than left to the table above.
BOOST_AUTO_TEST_CASE(sign_bit_is_separated_from_the_mantissa_magnitude) {
    decContext context;
    decContextDefault(&context, DEC_INIT_DECQUAD);

    for (std::string_view magnitude : {"0.7", "1.27", "12345.678", "9223372036854775.807"}) {
        BOOST_TEST_CONTEXT("magnitude=" << magnitude) {
            const std::string positive{magnitude};
            const std::string negative = "-" + positive;

            std::vector<BYTE> positive_encoded = encode_binary_decimal(positive);
            std::vector<BYTE> negative_encoded = encode_binary_decimal(negative);

            const std::string positive_text = decode_from_buffer(positive_encoded);
            const std::string negative_text = decode_from_buffer(negative_encoded);

            BOOST_CHECK_EQUAL(positive_text, positive);
            BOOST_CHECK_EQUAL(negative_text, negative);
            // The decoded magnitudes must agree once the sign is removed: an
            // unmasked sign bit would inflate the negative one instead.
            BOOST_CHECK_EQUAL(negative_text.substr(1), positive_text);
        }
    }
}
