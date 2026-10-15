// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Converts between libcoap PDUs and raft/oscore.hpp's coap_message, for tests
// that drive oscore_provider against real libcoap without the transport. On
// the libcoap backend OSCORE is Kythira's own: libcoap carries option 9 as an
// ordinary option, and the caller protects and verifies each message with the
// provider's Security Contexts (see oscore_provider's class comment).
//
// Include only where LIBCOAP_AVAILABLE is defined, after Boost.Test.

#include <raft/oscore.hpp>

#include <coap3/coap.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kythira::testing {

inline auto oscore_options_of(const coap_pdu_t* pdu) -> std::vector<oscore::coap_option> {
    std::vector<oscore::coap_option> options;
    coap_opt_iterator_t iter;
    coap_option_iterator_init(pdu, &iter, COAP_OPT_ALL);
    while (coap_opt_t* option = coap_option_next(&iter)) {
        const auto* value = reinterpret_cast<const std::byte*>(coap_opt_value(option));
        options.push_back(oscore::coap_option{static_cast<std::uint16_t>(iter.number),
                                              {value, value + coap_opt_length(option)}});
    }
    return options;
}

/// The protected message as it arrived: code, token, OSCORE option, payload.
inline auto oscore_outer_of(const coap_pdu_t* pdu) -> oscore::coap_message {
    oscore::coap_message message;
    message.type = static_cast<std::uint8_t>(coap_pdu_get_type(pdu));
    message.code = static_cast<std::uint8_t>(coap_pdu_get_code(pdu));
    message.message_id = static_cast<std::uint16_t>(coap_pdu_get_mid(pdu));
    const auto token = coap_pdu_get_token(pdu);
    const auto* token_bytes = reinterpret_cast<const std::byte*>(token.s);
    message.token.assign(token_bytes, token_bytes + token.length);
    for (auto& option : oscore_options_of(pdu)) {
        if (oscore::is_outer_option(option.number)) {
            message.options.push_back(std::move(option));
        }
    }
    std::size_t length = 0;
    const std::uint8_t* data = nullptr;
    if (coap_get_data(pdu, &length, &data) != 0) {
        const auto* bytes = reinterpret_cast<const std::byte*>(data);
        message.payload.assign(bytes, bytes + length);
        message.has_payload = true;
    }
    return message;
}

/// Writes `message`'s options (in option-number order) and payload into
/// `pdu`, whose code and token the caller has already set.
inline auto oscore_write_into(coap_pdu_t* pdu, oscore::coap_message message) -> void {
    std::stable_sort(message.options.begin(), message.options.end(),
                     [](const auto& a, const auto& b) { return a.number < b.number; });
    for (const auto& option : message.options) {
        BOOST_REQUIRE(coap_add_option(pdu, option.number, option.value.size(),
                                      reinterpret_cast<const uint8_t*>(option.value.data())) != 0);
    }
    if (message.has_payload) {
        BOOST_REQUIRE(coap_add_data(pdu, message.payload.size(),
                                    reinterpret_cast<const uint8_t*>(message.payload.data())) != 0);
    }
}

}  // namespace kythira::testing
