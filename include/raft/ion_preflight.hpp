// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ion-c handle owners and the binary-Ion length preflight, shared by
// ion_rpc_serializer (raft/ion_serializer.hpp) and every other reader of
// untrusted Ion, such as the verifiable ledger's Ion hash
// (services/ledger/). Split out of ion_serializer.hpp so a reader of Ion
// needs neither the serializer nor the future backend raft/types.hpp
// brings in.

#include <raft/exceptions.hpp>

#include <ionc/ion.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace kythira {

namespace detail {

/// @brief RAII owner for an `ion-c` writer handle.
///
/// `ion-c`'s `ion_writer_open_*`/`ion_writer_close` are paired C-style
/// open/close calls, not RAII. Wrapping the handle so its destructor calls
/// `ion_writer_close` means a `serialization_exception` thrown mid-encode still
/// releases the writer via stack unwinding rather than leaking it
/// (Requirement 5.6).
struct ion_writer_handle {
    hWRITER handle = nullptr;

    ion_writer_handle() = default;
    explicit ion_writer_handle(hWRITER h) : handle(h) {}
    ion_writer_handle(const ion_writer_handle&) = delete;
    auto operator=(const ion_writer_handle&) -> ion_writer_handle& = delete;
    ion_writer_handle(ion_writer_handle&&) = delete;
    auto operator=(ion_writer_handle&&) -> ion_writer_handle& = delete;

    ~ion_writer_handle() {
        if (handle != nullptr) {
            ion_writer_close(handle);
        }
    }
};

/// @brief RAII owner for an `ion-c` reader handle (see `ion_writer_handle`).
struct ion_reader_handle {
    hREADER handle = nullptr;

    ion_reader_handle() = default;
    explicit ion_reader_handle(hREADER h) : handle(h) {}
    ion_reader_handle(const ion_reader_handle&) = delete;
    auto operator=(const ion_reader_handle&) -> ion_reader_handle& = delete;
    ion_reader_handle(ion_reader_handle&&) = delete;
    auto operator=(ion_reader_handle&&) -> ion_reader_handle& = delete;

    ~ion_reader_handle() {
        if (handle != nullptr) {
            ion_reader_close(handle);
        }
    }
};

/// Nesting deeper than any Kythira message (they reach four levels) is
/// malformed; the bound also keeps the recursive preflight off the stack limit.
inline constexpr std::size_t ion_preflight_max_depth = 32;

/// True when `ion-c` will read `[begin, end)` as binary Ion: it picks the
/// encoding from a leading binary version marker, exactly as checked here.
inline auto ion_is_binary(const BYTE* begin, const BYTE* end) -> bool {
    return end - begin >= 4 && begin[0] == 0xE0 && begin[1] == 0x01 && begin[2] == 0x00 &&
           begin[3] == 0xEA;
}

/// Read one binary Ion VarUInt (7 bits per byte, the last byte flagged by its
/// high bit) without reading past `end`.
inline auto ion_preflight_var_uint(const BYTE*& pos, const BYTE* end) -> std::uint64_t {
    std::uint64_t value = 0;
    while (pos != end) {
        const BYTE b = *pos++;
        if (value > (std::numeric_limits<std::uint64_t>::max() >> 7U)) {
            throw serialization_exception("Ion decode: VarUInt overflows 64 bits");
        }
        value = (value << 7U) | (b & 0x7FU);
        if ((b & 0x80U) != 0) {
            return value;
        }
    }
    throw serialization_exception("Ion decode: truncated VarUInt");
}

/// @brief Check every length a binary Ion buffer declares against the bytes
/// that actually remain in its parent, before `ion-c` reads any value.
///
/// `ion-c` only bounds a value by its container once it steps in, and a
/// top-level container is bounded by nothing, so it reports a blob's declared
/// size (up to 2^31) from `ion_reader_get_lob_size` and sizes an arbitrary-
/// precision int's digits from the declared length before discovering the
/// bytes are missing. Without this walk a body of a few hundred bytes made
/// `read_blob` allocate a gigabyte. The CBOR serializer's `require_available`
/// is the same check; this is its binary-Ion counterpart (parity audit Z3).
///
/// Only framing is checked: type descriptors, lengths, struct field ids and
/// annotation headers. Content is left to `ion-c`, which still rejects
/// anything else that is malformed.
inline auto ion_binary_length_preflight(const BYTE* pos, const BYTE* end, bool in_struct,
                                        std::size_t depth) -> void {
    if (depth > ion_preflight_max_depth) {
        throw serialization_exception("Ion decode: nesting too deep");
    }
    constexpr unsigned type_bool = 1, type_list = 11, type_sexp = 12, type_struct = 13,
                       type_annotation = 14;
    constexpr unsigned length_varuint = 14, length_null = 15;
    while (pos != end) {
        if (in_struct) {
            (void)ion_preflight_var_uint(pos, end);  // field name symbol id
            if (pos == end) {
                throw serialization_exception("Ion decode: struct field has no value");
            }
        } else if (depth == 0 && ion_is_binary(pos, end)) {
            pos += 4;  // a version marker may start, or restart, the stream
            continue;
        }
        const BYTE descriptor = *pos++;
        const unsigned type = descriptor >> 4U;
        const unsigned low = descriptor & 0x0FU;

        std::uint64_t length = 0;
        if (type == type_bool || low == length_null) {
            length = 0;  // the low nibble is the value (bool) or the null marker
        } else if (low == length_varuint || (type == type_struct && low == 1)) {
            length = ion_preflight_var_uint(pos, end);  // L=1 marks a sorted struct
        } else {
            length = low;
        }
        if (length > static_cast<std::uint64_t>(end - pos)) {
            throw serialization_exception("Ion decode: declared length exceeds remaining input");
        }
        const BYTE* value_end = pos + length;

        if (low != length_null && (type == type_list || type == type_sexp || type == type_struct)) {
            ion_binary_length_preflight(pos, value_end, type == type_struct, depth + 1);
        } else if (type == type_annotation) {
            const std::uint64_t annotations = ion_preflight_var_uint(pos, value_end);
            if (annotations > static_cast<std::uint64_t>(value_end - pos)) {
                throw serialization_exception(
                    "Ion decode: declared length exceeds remaining input");
            }
            pos += annotations;
            ion_binary_length_preflight(pos, value_end, false, depth + 1);
        }
        pos = value_end;
    }
}

}  // namespace detail

}  // namespace kythira
