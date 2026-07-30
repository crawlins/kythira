/**
 * Example: CBOR RPC Serializer
 *
 * Demonstrates `kythira::cbor_serializer` (`cbor_rpc_serializer<std::vector<
 * std::byte>>`), the CBOR (RFC 8949) implementation of the `rpc_serializer`
 * concept, as a drop-in alternative to `kythira::json_serializer`. It shows:
 *
 * 1. Naming `cbor_serializer` as a `Types::serializer_type` (compile-time
 *    concept satisfaction) — the only change needed to switch a node to CBOR.
 * 2. Round-tripping every family of Raft RPC message over CBOR.
 * 3. CBOR's wire-size advantage over JSON on byte-carrying messages (log-entry
 *    commands and snapshot chunks ride as raw CBOR byte strings, not base64).
 * 4. `name()` resolving to the `application/cbor` media type that both the CoAP
 *    and HTTP transports now put on the wire.
 *
 * Registered as a CTest example; it prints a short report and exits non-zero if
 * any round-trip fails.
 */

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/coap_utils.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

using data_type = std::vector<std::byte>;

// (1) A Types bundle adopts CBOR by naming cbor_serializer as its
// serializer_type — nothing else changes, because every transport treats the
// serializer as an opaque byte-in/byte-out component. Both serializers satisfy
// the same rpc_serializer concept, so they are interchangeable at this seam.
struct cbor_types {
    using serialized_data_type = data_type;
    using serializer_type = kythira::cbor_serializer;
};
static_assert(
    kythira::rpc_serializer<cbor_types::serializer_type, cbor_types::serialized_data_type>,
    "cbor_serializer must satisfy the rpc_serializer concept");

auto make_command(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return bytes;
}

// Track failures so the process exit code reflects correctness.
int g_failures = 0;

auto check(bool ok, std::string_view what) -> void {
    std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] " << what << '\n';
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

auto main() -> int {
    kythira::cbor_serializer cbor;
    kythira::json_serializer json;

    std::cout << "=== CBOR RPC Serializer example ===\n\n";

    // (2) Round-trip a representative message from each family.
    std::cout << "Round-trip over CBOR:\n";

    kythira::request_vote_request<> rv{/*term=*/7, /*candidate_id=*/3,
                                       /*last_log_index=*/42, /*last_log_term=*/6};
    {
        auto back = cbor.deserialize_request_vote_request(cbor.serialize(rv));
        check(back.term() == rv.term() && back.candidate_id() == rv.candidate_id() &&
                  back.last_log_index() == rv.last_log_index() &&
                  back.last_log_term() == rv.last_log_term(),
              "request_vote_request");
    }

    kythira::append_entries_request<> ae;
    ae._term = 7;
    ae._leader_id = 3;
    ae._prev_log_index = 41;
    ae._prev_log_term = 6;
    ae._leader_commit = 40;
    ae._entries.push_back(
        {/*term=*/7, /*index=*/42, make_command("set x = 1"), kythira::entry_type::normal});
    ae._entries.push_back(
        {/*term=*/7, /*index=*/43, make_command("set y = 2"), kythira::entry_type::normal});
    {
        auto back = cbor.deserialize_append_entries_request(cbor.serialize(ae));
        bool entries_ok = back.entries().size() == ae.entries().size();
        for (std::size_t i = 0; entries_ok && i < ae.entries().size(); ++i) {
            entries_ok = back.entries()[i].index() == ae.entries()[i].index() &&
                         back.entries()[i].command() == ae.entries()[i].command();
        }
        check(back.term() == ae.term() && back.leader_id() == ae.leader_id() && entries_ok,
              "append_entries_request (with log-entry commands)");
    }

    kythira::install_snapshot_request<> snap;
    snap._term = 7;
    snap._leader_id = 3;
    snap._last_included_index = 100;
    snap._last_included_term = 6;
    snap._offset = 0;
    snap._data = make_command("<binary snapshot chunk>");
    snap._done = true;
    {
        auto back = cbor.deserialize_install_snapshot_request(cbor.serialize(snap));
        check(back.data() == snap.data() && back.done() == snap.done(),
              "install_snapshot_request (with data chunk)");
    }

    kythira::cluster_join_response<> join;
    join.accepted = false;
    join.redirect = kythira::peer_info<std::uint64_t, std::string>{/*node_id=*/1,
                                                                   /*address=*/"10.0.0.1:9443"};
    {
        auto back = cbor.deserialize_cluster_join_response(cbor.serialize(join));
        check(back.accepted == join.accepted && back.redirect.has_value() &&
                  back.redirect->node_id == join.redirect->node_id &&
                  back.redirect->address == join.redirect->address,
              "cluster_join_response (with redirect)");
    }

    // (3) Wire-size comparison on the byte-carrying messages. CBOR byte strings
    // avoid JSON's ~33% base64 inflation of the same bytes.
    std::cout << "\nWire size (CBOR vs JSON):\n";
    auto report_size = [](std::string_view label, std::size_t cbor_bytes, std::size_t json_bytes) {
        std::cout << "  " << label << ": CBOR " << cbor_bytes << " B, JSON " << json_bytes
                  << " B  (" << (cbor_bytes <= json_bytes ? "CBOR <= JSON" : "CBOR > JSON")
                  << ")\n";
    };
    report_size("append_entries_request", cbor.serialize(ae).size(), json.serialize(ae).size());
    report_size("install_snapshot_request", cbor.serialize(snap).size(),
                json.serialize(snap).size());
    check(cbor.serialize(ae).size() <= json.serialize(ae).size() &&
              cbor.serialize(snap).size() <= json.serialize(snap).size(),
          "CBOR output is no larger than JSON for byte-carrying messages");

    // (4) name() resolves to the application/cbor media type both transports now
    // put on the wire. The HTTP and CoAP transports derive their content type
    // via coap_utils::get_content_format_for_serializer(serializer.name()); the
    // two lines below reproduce exactly that derivation.
    std::cout << "\nContent-type labeling:\n";
    std::cout << "  cbor.name() = \"" << cbor.name() << "\"\n";
    const auto media_type = kythira::coap_utils::content_format_to_string(
        kythira::coap_utils::get_content_format_for_serializer(cbor.name()));
    std::cout << "  derived media type = \"" << media_type << "\"\n";
    check(kythira::coap_utils::get_content_format_for_serializer(cbor.name()) ==
              kythira::coap_utils::coap_content_format::application_cbor,
          "name() resolves to application/cbor");

    std::cout << "\n" << (g_failures == 0 ? "All checks passed." : "Some checks FAILED.") << '\n';
    return g_failures == 0 ? 0 : 1;
}
