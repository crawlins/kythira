// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file certificate_serial_x509.hpp
/// @brief OpenSSL conversions for `certificate_serial`: to and from
///        `ASN1_INTEGER`, and reading the serial out of an issued certificate.
///
/// Cloud CA APIs do not all return the serial of what they issued (ACM
/// Private CA returns only PEM), and those that do use their own spelling, so
/// providers read it from the certificate itself, which is authoritative.

#include <raft/certificate_serial.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace raft::testing {

struct asn1_integer_deleter {
    void operator()(ASN1_INTEGER* p) const noexcept { ASN1_INTEGER_free(p); }
};
using asn1_integer_ptr = std::unique_ptr<ASN1_INTEGER, asn1_integer_deleter>;

/// The serial an `ASN1_INTEGER` holds. `std::nullopt` for a negative value
/// (RFC 5280 requires positive serials; some legacy CAs emitted negative ones)
/// or one longer than `certificate_serial::max_octets`.
[[nodiscard]] inline auto serial_from_asn1(const ASN1_INTEGER* value)
    -> std::optional<certificate_serial> {
    if (value == nullptr || ASN1_STRING_type(value) != V_ASN1_INTEGER) {
        return std::nullopt;
    }
    // The content octets are the magnitude, without the DER sign octet.
    const auto* data = ASN1_STRING_get0_data(value);
    const auto len = ASN1_STRING_length(value);
    if (len < 0) {
        return std::nullopt;
    }
    return certificate_serial::from_bytes(
        std::span<const std::uint8_t>(data, static_cast<std::size_t>(len)));
}

/// A fresh `ASN1_INTEGER` holding `serial`. Throws `std::runtime_error` when
/// OpenSSL cannot allocate it.
[[nodiscard]] inline auto serial_to_asn1(const certificate_serial& serial) -> asn1_integer_ptr {
    asn1_integer_ptr out{ASN1_INTEGER_new()};
    if (out == nullptr) {
        throw std::runtime_error("serial_to_asn1: ASN1_INTEGER_new failed");
    }
    const auto bytes = serial.bytes();
    // The zero serial has no significant octets; DER still needs one.
    static constexpr unsigned char zero = 0;
    const unsigned char* data = bytes.empty() ? &zero : bytes.data();
    const int len = bytes.empty() ? 1 : static_cast<int>(bytes.size());
    if (ASN1_STRING_set(out.get(), data, len) != 1) {
        throw std::runtime_error("serial_to_asn1: ASN1_STRING_set failed");
    }
    return out;
}

/// Sets `cert`'s serialNumber to `serial`. Throws `std::runtime_error` on
/// failure.
inline void set_certificate_serial(X509* cert, const certificate_serial& serial) {
    auto value = serial_to_asn1(serial);
    if (X509_set_serialNumber(cert, value.get()) != 1) {
        throw std::runtime_error("X509_set_serialNumber failed");
    }
}

/// `cert`'s serial, or `std::nullopt` if it is negative or too long.
[[nodiscard]] inline auto serial_of(const X509* cert) -> std::optional<certificate_serial> {
    if (cert == nullptr) {
        return std::nullopt;
    }
    return serial_from_asn1(X509_get0_serialNumber(cert));
}

/// The serial of the first certificate in `pem`, or `std::nullopt` when there
/// is none or it cannot be parsed.
[[nodiscard]] inline auto serial_from_certificate_pem(std::string_view pem)
    -> std::optional<certificate_serial> {
    if (pem.empty()) {
        return std::nullopt;
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
    if (bio == nullptr) {
        return std::nullopt;
    }
    std::unique_ptr<X509, decltype(&X509_free)> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
    return serial_of(cert.get());
}

}  // namespace raft::testing
