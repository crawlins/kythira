// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A small in-memory Ion value for the ledger (.kiro/specs/verifiable-ledger/
// task 3): enough to take a transaction entry apart into its statements, to
// patch a document by path, and to build revisions and blocks.
//
// Containers (struct, list, sexp) are parsed into children. A scalar is kept
// as the Ion binary encoding of that one value, annotations included, and is
// copied value by value when the tree is written, so nothing about a scalar
// (arbitrary-precision ints and decimals, timestamp precision and offset,
// symbol text) is lost or normalised on the way through. Ion hash does not
// depend on the encoding, so a document hashes the same as text, as the
// client's binary and as this tree's binary.
//
// Strings and lobs are read with partial reads, for the reason given at
// ion_hash_detail::read_in_pieces: ion-c's text reader refuses a long value
// read in one call.

#include <kythira/ledger/ion_hash.hpp>
#include <kythira/ledger/ledger_hash.hpp>
#include <raft/ion_preflight.hpp>

#include <ionc/ion.h>
#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// A malformed or unsupported Ion value.
class ion_value_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ion_type : std::uint8_t {
    null,
    boolean,
    integer,
    floating,
    decimal,
    timestamp,
    symbol,
    string,
    clob,
    blob,
    list,
    sexp,
    structure,
};

namespace ion_value_detail {

inline auto check(iERR err, const char* what) -> void {
    if (err != IERR_OK) {
        throw ion_value_error(std::string("ion value: ") + what + ": " + ion_error_to_str(err));
    }
}

inline auto type_of(ION_TYPE type) -> ion_type {
    switch (ION_TYPE_INT(type)) {
        case tid_NULL_INT:
            return ion_type::null;
        case tid_BOOL_INT:
            return ion_type::boolean;
        case tid_INT_INT:
            return ion_type::integer;
        case tid_FLOAT_INT:
            return ion_type::floating;
        case tid_DECIMAL_INT:
            return ion_type::decimal;
        case tid_TIMESTAMP_INT:
            return ion_type::timestamp;
        case tid_SYMBOL_INT:
            return ion_type::symbol;
        case tid_STRING_INT:
            return ion_type::string;
        case tid_CLOB_INT:
            return ion_type::clob;
        case tid_BLOB_INT:
            return ion_type::blob;
        case tid_LIST_INT:
            return ion_type::list;
        case tid_SEXP_INT:
            return ion_type::sexp;
        case tid_STRUCT_INT:
            return ion_type::structure;
        default:
            throw ion_value_error("ion value: unknown type");
    }
}

inline auto ion_type_of(ion_type type) -> ION_TYPE {
    switch (type) {
        case ion_type::null:
            return tid_NULL;
        case ion_type::boolean:
            return tid_BOOL;
        case ion_type::integer:
            return tid_INT;
        case ion_type::floating:
            return tid_FLOAT;
        case ion_type::decimal:
            return tid_DECIMAL;
        case ion_type::timestamp:
            return tid_TIMESTAMP;
        case ion_type::symbol:
            return tid_SYMBOL;
        case ion_type::string:
            return tid_STRING;
        case ion_type::clob:
            return tid_CLOB;
        case ion_type::blob:
            return tid_BLOB;
        case ion_type::list:
            return tid_LIST;
        case ion_type::sexp:
            return tid_SEXP;
        case ion_type::structure:
            return tid_STRUCT;
    }
    return tid_NULL;
}

/// Collects what a stream writer emits. ion-c copies output into
/// [curr, limit) and calls the handler each time it has advanced `curr`
/// (and once at the start, with both null), so the handler takes the
/// bytes up to `curr` and hands the block back empty.
struct output_sink {
    static constexpr std::size_t block_size = 16 * 1024;
    std::vector<std::uint8_t> out;
    std::vector<BYTE> block = std::vector<BYTE>(block_size);

    static auto handler(struct _ion_user_stream* stream) -> iERR {
        auto* self = static_cast<output_sink*>(stream->handler_state);
        if (stream->curr != nullptr) {
            self->out.insert(self->out.end(), self->block.data(), stream->curr);
        }
        stream->curr = self->block.data();
        stream->limit = self->block.data() + self->block.size();
        return IERR_OK;
    }
};

/// Run `body(writer)` against a binary writer and return what it wrote.
/// A stream writer rather than a fixed buffer: a body that copies from a
/// reader consumes it, so it cannot be rerun into a bigger buffer.
/// Ion text instead of binary when `text`; JSON (ion-c's down-conversion:
/// no annotations, symbols and timestamps as strings, blobs as base64
/// strings) when `json`.
enum class output_format : std::uint8_t {
    binary,
    text,
    json
};

template<typename Body>
auto write_with(output_format format, Body&& body) -> std::vector<std::uint8_t> {
    output_sink sink;
    ION_WRITER_OPTIONS options;
    std::memset(&options, 0, sizeof(options));
    options.output_as_binary = format == output_format::binary ? TRUE : FALSE;
    options.json_downconvert = format == output_format::json ? TRUE : FALSE;
    hWRITER writer = nullptr;
    check(ion_writer_open_stream(&writer, &output_sink::handler, &sink, &options),
          "opening writer");
    kythira::detail::ion_writer_handle guard{writer};
    body(writer);
    guard.handle = nullptr;
    check(ion_writer_close(writer), "closing writer");
    return std::move(sink.out);
}

template<typename Body> auto write_binary(Body&& body) -> std::vector<std::uint8_t> {
    return write_with(output_format::binary, std::forward<Body>(body));
}

/// A reader over `bytes` (text or binary) holding the buffer it reads.
class owned_reader {
public:
    explicit owned_reader(std::span<const std::uint8_t> bytes)
        : buffer_(bytes.begin(), bytes.end()) {
        if (kythira::detail::ion_is_binary(buffer_.data(), buffer_.data() + buffer_.size())) {
            try {
                kythira::detail::ion_binary_length_preflight(
                    buffer_.data(), buffer_.data() + buffer_.size(), false, 0);
            } catch (const kythira::serialization_exception& e) {
                throw ion_value_error(std::string("ion value: ") + e.what());
            }
        }
        ION_READER_OPTIONS options = ledger_ion_reader_options();
        hREADER reader = nullptr;
        check(ion_reader_open_buffer(&reader, buffer_.empty() ? &dummy_ : buffer_.data(),
                                     static_cast<SIZE>(buffer_.size()), &options),
              "opening reader");
        guard_.handle = reader;
    }

    [[nodiscard]] auto get() const -> hREADER { return guard_.handle; }

    /// Move to the next top-level value; its type, or tid_EOF.
    auto next() -> ION_TYPE {
        ION_TYPE type = nullptr;
        check(ion_reader_next(guard_.handle, &type), "reading value");
        return type;
    }

private:
    std::vector<BYTE> buffer_;
    BYTE dummy_ = 0;
    kythira::detail::ion_reader_handle guard_;
};

/// Text of a symbol, or nullopt for a symbol with unknown text.
inline auto symbol_text(const ION_SYMBOL& symbol) -> std::optional<std::string> {
    if (symbol.value.value == nullptr) {
        return std::nullopt;
    }
    return std::string(reinterpret_cast<const char*>(symbol.value.value),
                       static_cast<std::size_t>(symbol.value.length));
}

inline auto to_istring(std::string_view s) -> ION_STRING {
    ION_STRING out;
    out.length = static_cast<SIZE>(s.size());
    out.value = reinterpret_cast<BYTE*>(const_cast<char*>(s.data()));
    return out;
}

struct int_deleter {
    auto operator()(ION_INT* p) const -> void { ion_int_free(p); }
};

struct decimal_deleter {
    auto operator()(ION_DECIMAL* p) const -> void { ion_decimal_free(p); }
};

/// Copy the scalar (or null) the reader is on into the writer, with its
/// annotations and without its field name.
inline auto copy_scalar(hREADER reader, hWRITER writer) -> void {
    namespace hd = ion_hash_detail;
    ION_TYPE type = nullptr;
    check(ion_reader_get_type(reader, &type), "reading type");
    SIZE annotation_count = 0;
    check(ion_reader_get_annotation_count(reader, &annotation_count), "reading annotations");
    if (annotation_count > 0) {
        std::vector<ION_SYMBOL> annotations(static_cast<std::size_t>(annotation_count));
        SIZE got = 0;
        check(ion_reader_get_annotation_symbols(reader, annotations.data(), annotation_count, &got),
              "reading annotations");
        check(ion_writer_write_annotation_symbols(writer, annotations.data(), got),
              "writing annotations");
    }
    BOOL is_null = FALSE;
    check(ion_reader_is_null(reader, &is_null), "reading null flag");
    if (is_null == TRUE) {
        check(ion_writer_write_typed_null(writer, type), "writing null");
        return;
    }
    switch (ION_TYPE_INT(type)) {
        case tid_BOOL_INT: {
            BOOL value = FALSE;
            check(ion_reader_read_bool(reader, &value), "reading bool");
            check(ion_writer_write_bool(writer, value), "writing bool");
            return;
        }
        case tid_INT_INT: {
            ION_INT* big = nullptr;
            check(ion_int_alloc(nullptr, &big), "allocating int");
            const std::unique_ptr<ION_INT, int_deleter> guard(big);
            check(ion_reader_read_ion_int(reader, big), "reading int");
            check(ion_writer_write_ion_int(writer, big), "writing int");
            return;
        }
        case tid_FLOAT_INT: {
            double value = 0;
            check(ion_reader_read_double(reader, &value), "reading float");
            check(ion_writer_write_double(writer, value), "writing float");
            return;
        }
        case tid_DECIMAL_INT: {
            ION_DECIMAL dec;
            std::memset(&dec, 0, sizeof(dec));
            check(ion_reader_read_ion_decimal(reader, &dec), "reading decimal");
            const std::unique_ptr<ION_DECIMAL, decimal_deleter> guard(&dec);
            check(ion_writer_write_ion_decimal(writer, &dec), "writing decimal");
            return;
        }
        case tid_TIMESTAMP_INT: {
            ION_TIMESTAMP ts;
            std::memset(&ts, 0, sizeof(ts));
            check(ion_reader_read_timestamp(reader, &ts), "reading timestamp");
            check(ion_writer_write_timestamp(writer, &ts), "writing timestamp");
            return;
        }
        case tid_SYMBOL_INT: {
            ION_SYMBOL symbol;
            std::memset(&symbol, 0, sizeof(symbol));
            check(ion_reader_read_ion_symbol(reader, &symbol), "reading symbol");
            if (symbol.value.value == nullptr) {
                throw ion_value_error("ion value: symbol with unknown text");
            }
            check(ion_writer_write_symbol(writer, &symbol.value), "writing symbol");
            return;
        }
        case tid_STRING_INT: {
            auto bytes = hd::read_string_bytes(reader);
            ION_STRING s;
            s.length = static_cast<SIZE>(bytes.size());
            s.value = bytes.data();
            check(ion_writer_write_string(writer, &s), "writing string");
            return;
        }
        case tid_CLOB_INT:
        case tid_BLOB_INT: {
            auto bytes = hd::read_lob_bytes(reader);
            BYTE dummy = 0;
            BYTE* data = bytes.empty() ? &dummy : bytes.data();
            const auto size = static_cast<SIZE>(bytes.size());
            check(type == tid_CLOB ? ion_writer_write_clob(writer, data, size)
                                   : ion_writer_write_blob(writer, data, size),
                  "writing lob");
            return;
        }
        default:
            throw ion_value_error("ion value: unexpected scalar type");
    }
}

}  // namespace ion_value_detail

/// An Ion value: a scalar, or a container of ion_values.
class ion_value {
public:
    /// Containers nested deeper than this are rejected (the ledger's reader
    /// limit, see ledger_ion_reader_options).
    static constexpr std::size_t max_depth = kythira::detail::ion_preflight_max_depth;

    /// `null`.
    ion_value() = default;

    /// The single top-level value in `ion` (text or binary). Throws
    /// ion_value_error on malformed input or anything but exactly one value.
    static auto parse(std::span<const std::uint8_t> ion) -> ion_value {
        ion_value_detail::owned_reader reader(ion);
        if (reader.next() == tid_EOF) {
            throw ion_value_error("ion value: no value");
        }
        ion_value out = read_current(reader.get());
        if (reader.next() != tid_EOF) {
            throw ion_value_error("ion value: more than one top-level value");
        }
        return out;
    }

    static auto parse(std::string_view text) -> ion_value {
        return parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    /// The value the reader is on (its field name is not part of it).
    static auto read_current(hREADER reader, std::size_t depth = 0) -> ion_value {
        namespace d = ion_value_detail;
        ION_TYPE type = nullptr;
        d::check(ion_reader_get_type(reader, &type), "reading type");
        BOOL is_null = FALSE;
        d::check(ion_reader_is_null(reader, &is_null), "reading null flag");
        const ion_type t = d::type_of(type);
        const bool container =
            t == ion_type::list || t == ion_type::sexp || t == ion_type::structure;
        ion_value out;
        out.type_ = t;
        out.null_ = is_null == TRUE;
        if (!container || out.null_) {
            out.scalar_ = d::write_binary([&](hWRITER w) { d::copy_scalar(reader, w); });
            return out;
        }
        if (depth >= max_depth) {
            throw ion_value_error("ion value: nesting too deep");
        }
        SIZE annotation_count = 0;
        d::check(ion_reader_get_annotation_count(reader, &annotation_count), "reading annotations");
        if (annotation_count > 0) {
            std::vector<ION_SYMBOL> annotations(static_cast<std::size_t>(annotation_count));
            SIZE got = 0;
            d::check(ion_reader_get_annotation_symbols(reader, annotations.data(), annotation_count,
                                                       &got),
                     "reading annotations");
            for (SIZE i = 0; i < got; ++i) {
                auto text = d::symbol_text(annotations[static_cast<std::size_t>(i)]);
                if (!text) {
                    throw ion_value_error("ion value: annotation with unknown text");
                }
                out.annotations_.push_back(std::move(*text));
            }
        }
        d::check(ion_reader_step_in(reader), "stepping in");
        for (;;) {
            ION_TYPE child = nullptr;
            d::check(ion_reader_next(reader, &child), "reading container");
            if (child == tid_EOF) {
                break;
            }
            if (t == ion_type::structure) {
                ION_SYMBOL* name = nullptr;
                d::check(ion_reader_get_field_name_symbol(reader, &name), "reading field name");
                auto text = name == nullptr ? std::nullopt : d::symbol_text(*name);
                if (!text) {
                    throw ion_value_error("ion value: field name with unknown text");
                }
                out.names_.push_back(std::move(*text));
            }
            out.items_.push_back(read_current(reader, depth + 1));
        }
        d::check(ion_reader_step_out(reader), "stepping out");
        return out;
    }

    // ── Building ────────────────────────────────────────────────────────────

    static auto boolean(bool value) -> ion_value {
        return scalar(ion_type::boolean, [&](hWRITER w) {
            ion_value_detail::check(ion_writer_write_bool(w, value ? TRUE : FALSE), "writing bool");
        });
    }

    static auto integer(std::int64_t value) -> ion_value {
        return scalar(ion_type::integer, [&](hWRITER w) {
            ion_value_detail::check(ion_writer_write_int64(w, value), "writing int");
        });
    }

    static auto string(std::string_view value) -> ion_value {
        return scalar(ion_type::string, [&](hWRITER w) {
            ION_STRING s = ion_value_detail::to_istring(value);
            ion_value_detail::check(ion_writer_write_string(w, &s), "writing string");
        });
    }

    static auto symbol(std::string_view value) -> ion_value {
        return scalar(ion_type::symbol, [&](hWRITER w) {
            ION_STRING s = ion_value_detail::to_istring(value);
            ion_value_detail::check(ion_writer_write_symbol(w, &s), "writing symbol");
        });
    }

    static auto blob(std::span<const std::uint8_t> value) -> ion_value {
        return scalar(ion_type::blob, [&](hWRITER w) {
            BYTE dummy = 0;
            BYTE* data = value.empty() ? &dummy : const_cast<BYTE*>(value.data());
            ion_value_detail::check(ion_writer_write_blob(w, data, static_cast<SIZE>(value.size())),
                                    "writing blob");
        });
    }

    /// A timestamp from its Ion text form, e.g. "2026-11-09T12:00:00.000Z".
    static auto timestamp(std::string_view text) -> ion_value {
        std::string buffer(text);
        ION_TIMESTAMP ts;
        std::memset(&ts, 0, sizeof(ts));
        decContext context;
        decContextDefault(&context, DEC_INIT_DECQUAD);
        SIZE used = 0;
        ion_value_detail::check(
            ion_timestamp_parse(&ts, buffer.data(), static_cast<SIZE>(buffer.size()), &used,
                                &context),
            "parsing timestamp");
        if (static_cast<std::size_t>(used) != buffer.size()) {
            throw ion_value_error("ion value: trailing characters after timestamp");
        }
        return scalar(ion_type::timestamp, [&](hWRITER w) {
            ion_value_detail::check(ion_writer_write_timestamp(w, &ts), "writing timestamp");
        });
    }

    static auto make_struct() -> ion_value { return container(ion_type::structure); }
    static auto make_list() -> ion_value { return container(ion_type::list); }

    // ── Inspecting ──────────────────────────────────────────────────────────

    [[nodiscard]] auto type() const -> ion_type { return type_; }
    [[nodiscard]] auto is_null() const -> bool { return null_; }
    [[nodiscard]] auto is_struct() const -> bool { return type_ == ion_type::structure && !null_; }
    [[nodiscard]] auto is_sequence() const -> bool {
        return (type_ == ion_type::list || type_ == ion_type::sexp) && !null_;
    }

    /// A container's annotations (a scalar keeps its own in its encoding).
    [[nodiscard]] auto annotations() const -> const std::vector<std::string>& {
        return annotations_;
    }

    [[nodiscard]] auto as_bool() const -> std::optional<bool> {
        if (type_ != ion_type::boolean || null_) {
            return std::nullopt;
        }
        return read_scalar([](hREADER r) {
            BOOL v = FALSE;
            ion_value_detail::check(ion_reader_read_bool(r, &v), "reading bool");
            return v == TRUE;
        });
    }

    /// An int that fits in 64 bits.
    [[nodiscard]] auto as_int64() const -> std::optional<std::int64_t> {
        if (type_ != ion_type::integer || null_) {
            return std::nullopt;
        }
        return read_scalar([](hREADER r) -> std::optional<std::int64_t> {
            int64_t v = 0;
            if (ion_reader_read_int64(r, &v) != IERR_OK) {
                return std::nullopt;
            }
            return v;
        });
    }

    /// A string's or symbol's text.
    [[nodiscard]] auto as_text() const -> std::optional<std::string> {
        if (null_) {
            return std::nullopt;
        }
        if (type_ == ion_type::string) {
            return read_scalar([](hREADER r) {
                auto bytes = ion_hash_detail::read_string_bytes(r);
                return std::string(bytes.begin(), bytes.end());
            });
        }
        if (type_ == ion_type::symbol) {
            return read_scalar([](hREADER r) {
                ION_SYMBOL symbol;
                std::memset(&symbol, 0, sizeof(symbol));
                ion_value_detail::check(ion_reader_read_ion_symbol(r, &symbol), "reading symbol");
                return ion_value_detail::symbol_text(symbol);
            });
        }
        return std::nullopt;
    }

    /// A blob's or clob's bytes.
    [[nodiscard]] auto as_bytes() const -> std::optional<std::vector<std::uint8_t>> {
        if (null_ || (type_ != ion_type::blob && type_ != ion_type::clob)) {
            return std::nullopt;
        }
        return read_scalar([](hREADER r) { return ion_hash_detail::read_lob_bytes(r); });
    }

    /// A timestamp in Ion text form, as ion-c prints it.
    [[nodiscard]] auto as_timestamp_text() const -> std::optional<std::string> {
        if (type_ != ion_type::timestamp || null_) {
            return std::nullopt;
        }
        return read_scalar([](hREADER r) {
            ION_TIMESTAMP ts;
            std::memset(&ts, 0, sizeof(ts));
            ion_value_detail::check(ion_reader_read_timestamp(r, &ts), "reading timestamp");
            std::string out(64, '\0');
            SIZE length = 0;
            decContext context;
            decContextDefault(&context, DEC_INIT_DECQUAD);
            ion_value_detail::check(
                ion_timestamp_to_string(&ts, out.data(), static_cast<SIZE>(out.size()), &length,
                                        &context),
                "printing timestamp");
            out.resize(static_cast<std::size_t>(length));
            return out;
        });
    }

    // ── Structs ─────────────────────────────────────────────────────────────

    [[nodiscard]] auto size() const -> std::size_t { return items_.size(); }
    [[nodiscard]] auto field_name(std::size_t i) const -> const std::string& {
        return names_.at(i);
    }
    [[nodiscard]] auto field_value(std::size_t i) const -> const ion_value& { return items_.at(i); }

    /// The first field named `name`, or nullptr (also for a non-struct).
    [[nodiscard]] auto find(std::string_view name) const -> const ion_value* {
        if (!is_struct()) {
            return nullptr;
        }
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (names_[i] == name) {
                return &items_[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto find(std::string_view name) -> ion_value* {
        return const_cast<ion_value*>(std::as_const(*this).find(name));
    }

    /// Set field `name` to `value`: the first field of that name is
    /// replaced and any later ones removed, or the field is appended.
    auto set(std::string_view name, ion_value value) -> ion_value& {
        require_struct();
        std::size_t first = names_.size();
        for (std::size_t i = 0; i < names_.size();) {
            if (names_[i] != name) {
                ++i;
            } else if (first == names_.size()) {
                first = i++;
            } else {
                erase_at(i);
            }
        }
        if (first == names_.size()) {
            names_.emplace_back(name);
            items_.push_back(std::move(value));
        } else {
            items_[first] = std::move(value);
        }
        return *this;
    }

    /// Remove every field named `name`; true if any was there.
    auto erase(std::string_view name) -> bool {
        require_struct();
        bool any = false;
        for (std::size_t i = 0; i < names_.size();) {
            if (names_[i] == name) {
                erase_at(i);
                any = true;
            } else {
                ++i;
            }
        }
        return any;
    }

    // ── Lists and sexps ─────────────────────────────────────────────────────

    [[nodiscard]] auto items() const -> const std::vector<ion_value>& { return items_; }
    [[nodiscard]] auto items() -> std::vector<ion_value>& { return items_; }

    auto push_back(ion_value value) -> ion_value& {
        if (!is_sequence()) {
            throw ion_value_error("ion value: push_back on a non-sequence");
        }
        items_.push_back(std::move(value));
        return *this;
    }

    // ── Encoding and hashing ────────────────────────────────────────────────

    /// Write this value (after any field name the caller wrote).
    auto write(hWRITER writer) const -> void {
        namespace d = ion_value_detail;
        if (scalar_.empty() && (null_ || !is_container_type())) {
            d::check(ion_writer_write_typed_null(writer, d::ion_type_of(type_)), "writing null");
            return;
        }
        if (!scalar_.empty()) {
            d::owned_reader reader(scalar_);
            reader.next();
            d::copy_scalar(reader.get(), writer);
            return;
        }
        if (!annotations_.empty()) {
            std::vector<ION_STRING> names;
            names.reserve(annotations_.size());
            for (const auto& a : annotations_) {
                names.push_back(d::to_istring(a));
            }
            d::check(
                ion_writer_write_annotations(writer, names.data(), static_cast<SIZE>(names.size())),
                "writing annotations");
        }
        d::check(ion_writer_start_container(writer, d::ion_type_of(type_)), "starting container");
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (type_ == ion_type::structure) {
                ION_STRING name = d::to_istring(names_[i]);
                d::check(ion_writer_write_field_name(writer, &name), "writing field name");
            }
            items_[i].write(writer);
        }
        d::check(ion_writer_finish_container(writer), "finishing container");
    }

    /// Ion binary for this value alone.
    [[nodiscard]] auto to_binary() const -> std::vector<std::uint8_t> {
        return ion_value_detail::write_binary([this](hWRITER w) { write(w); });
    }

    /// Ion text for this value alone.
    [[nodiscard]] auto to_text() const -> std::string {
        const auto bytes = ion_value_detail::write_with(ion_value_detail::output_format::text,
                                                        [this](hWRITER w) { write(w); });
        return {bytes.begin(), bytes.end()};
    }

    /// JSON for this value alone, for export: annotations dropped,
    /// symbols and timestamps as strings, blobs and clobs as base64
    /// strings, s-expressions as arrays. Scalars go through ion-c's JSON
    /// down-conversion, except lobs, which ion-c 1.1.3 still writes as
    /// {{...}}.
    [[nodiscard]] auto to_json() const -> std::string {
        std::string out;
        append_json(out);
        return out;
    }

    /// Its SHA-256 Ion hash.
    [[nodiscard]] auto hash() const -> hash256 { return ion_hash(to_binary()); }

private:
    auto append_json(std::string& out) const -> void {
        if (null_) {
            out += "null";
            return;
        }
        if (type_ == ion_type::structure) {
            out += '{';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                out += i == 0 ? "" : ",";
                append_json_string(out, names_[i]);
                out += ':';
                items_[i].append_json(out);
            }
            out += '}';
            return;
        }
        if (type_ == ion_type::list || type_ == ion_type::sexp) {
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                out += i == 0 ? "" : ",";
                items_[i].append_json(out);
            }
            out += ']';
            return;
        }
        if (type_ == ion_type::blob || type_ == ion_type::clob) {
            const auto bytes = as_bytes().value_or(std::vector<std::uint8_t>{});
            std::string b64(((bytes.size() + 2) / 3 * 4) + 1, '\0');
            const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64.data()),
                                          bytes.data(), static_cast<int>(bytes.size()));
            b64.resize(static_cast<std::size_t>(n));
            out += '"';
            out += b64;
            out += '"';
            return;
        }
        ion_value bare = *this;
        bare.annotations_.clear();
        const auto bytes = ion_value_detail::write_with(ion_value_detail::output_format::json,
                                                        [&bare](hWRITER w) { bare.write(w); });
        out.append(bytes.begin(), bytes.end());
    }

    static auto append_json_string(std::string& out, std::string_view text) -> void {
        static constexpr std::string_view hex = "0123456789abcdef";
        out += '"';
        for (const char c : text) {
            const auto u = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (u < 0x20U) {
                out += "\\u00";
                out += hex[u >> 4U];
                out += hex[u & 0x0FU];
            } else {
                out += c;
            }
        }
        out += '"';
    }

    template<typename Body> static auto scalar(ion_type type, Body&& body) -> ion_value {
        ion_value out;
        out.type_ = type;
        out.null_ = false;
        out.scalar_ = ion_value_detail::write_binary(std::forward<Body>(body));
        return out;
    }

    static auto container(ion_type type) -> ion_value {
        ion_value out;
        out.type_ = type;
        out.null_ = false;
        return out;
    }

    template<typename Fn> auto read_scalar(Fn&& fn) const -> std::invoke_result_t<Fn, hREADER> {
        ion_value_detail::owned_reader reader(scalar_);
        reader.next();
        return fn(reader.get());
    }

    [[nodiscard]] auto is_container_type() const -> bool {
        return type_ == ion_type::list || type_ == ion_type::sexp || type_ == ion_type::structure;
    }

    auto require_struct() const -> void {
        if (!is_struct()) {
            throw ion_value_error("ion value: not a struct");
        }
    }

    auto erase_at(std::size_t i) -> void {
        names_.erase(names_.begin() + static_cast<std::ptrdiff_t>(i));
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    ion_type type_ = ion_type::null;
    bool null_ = true;
    std::vector<std::uint8_t> scalar_;  // one scalar's binary; empty for a container
    std::vector<std::string> annotations_;
    std::vector<std::string> names_;  // struct field names, parallel to items_
    std::vector<ion_value> items_;
};

}  // namespace kythira::ledger
