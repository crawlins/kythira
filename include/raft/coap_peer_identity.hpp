// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file coap_peer_identity.hpp
/// @brief Binding CoAP peers' credentials to node ids of any type
/// (.kiro/specs/cloud-composite-node-ids/, Requirements 9.2 and 12.2).
///
/// The CoAP transports never key authentication by node id themselves: a
/// DTLS session trusts whatever chains to `ca_file`, a PSK session whatever
/// knows the key. Binding a certificate to the node it names is opt-in,
/// through `pki_credentials::cn_validator`, which all three backends
/// (libcoap, cantcoap, libnyoci) run on the peer's own certificate after
/// chain validation. `coap_peer_identity_validator<NodeId>` builds that hook
/// from a set of node ids: a peer is accepted only if its certificate carries
/// one of their reserved peer names (`raft::testing::peer_identity_name`) in
/// the SAN kind its id type uses (`cert_has_peer_name_in<NodeId>`): a DNS SAN
/// `ca-cluster-node-<n>` for a numeric id, a URI SAN
/// `urn:kythira:node:<percent-encoded canonical text>` for a textual one,
/// exactly as TLS-TCP's trust policy matches them. Canonical text holds ':'
/// and '%', so it is never put into a DNS name.
///
/// PSK identities and OSCORE ids are deliberately not derived from node ids:
///
///  * A DTLS-PSK identity (`psk_credentials::identity`) is an opaque octet
///    string (RFC 4279 §5.3 has every stack accept at least 128) that both ends are provisioned
///    with alongside the key. Nothing in the transport maps it to or from a node id, so a textual
///    cluster works unchanged. An operator who wants per-node identities can use the id's canonical
///    text verbatim: ':' and '%' need no escaping in an opaque octet string.
///  * OSCORE contexts are keyed by Raft group id, never node id, and Sender
///    and Recipient IDs (`oscore_credentials`) are provisioned byte strings that
///    must fit the AEAD nonce (7 bytes for AES-CCM-16-64-128, RFC 8613
///    §3.3); canonical text such as "aws-ec2:us-east-1:i-0123456789abcdef0"
///    cannot. Deriving them from node ids would only work for small numeric
///    ids, so they stay operator-provisioned for every id type.
///
/// Requires OpenSSL and the same `CPPHTTPLIB_OPENSSL_SUPPORT` coupling as
/// `tls_tcp_rpc.hpp`, which provides the SAN matching helpers.

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

#include <raft/ca_http_helpers.hpp>
#include <raft/composite_node_id.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace kythira {

/// @brief A `pki_credentials::cn_validator` that accepts a peer certificate
/// only when it names one of @p peers as a Raft peer, in the SAN kind
/// `NodeId` uses. An unparseable certificate, an empty peer set, or the right
/// name in the wrong SAN kind is refused.
template<typename NodeId>
[[nodiscard]] auto coap_peer_identity_validator(const std::vector<NodeId>& peers)
    -> std::function<bool(const std::string& peer_cert_pem)> {
    std::set<std::string> names;
    for (const auto& peer : peers) {
        names.insert(raft::testing::peer_identity_name(peer));
    }
    return [names = std::move(names)](const std::string& peer_cert_pem) -> bool {
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(
            BIO_new_mem_buf(peer_cert_pem.data(), static_cast<int>(peer_cert_pem.size())),
            &BIO_free);
        if (!bio) {
            return false;
        }
        std::unique_ptr<X509, decltype(&X509_free)> cert(
            PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
        if (!cert) {
            return false;
        }
        return raft::testing::cert_has_peer_name_in<NodeId>(cert.get(), names);
    };
}

}  // namespace kythira

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT
