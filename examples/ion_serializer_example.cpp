/**
 * Example: Amazon Ion RPC Serializer
 *
 * Demonstrates `kythira::ion_serializer` (`ion_rpc_serializer<std::vector<
 * std::byte>>`), the Amazon Ion (via `ion-c`) implementation of the
 * `rpc_serializer` concept, as a drop-in alternative to
 * `kythira::json_serializer`. It shows:
 *
 * 1. Naming `ion_serializer` as a `Types::serializer_type` (compile-time
 *    concept satisfaction) — the only change needed to switch a node to Ion.
 * 2. Round-tripping every family of Raft RPC message over Ion, in both the
 *    binary and text encodings.
 * 3. Encoding-agnostic deserialize: bytes written as text Ion decode through
 *    the same call that decodes binary Ion (Ion's binary version marker makes
 *    the two self-describing).
 * 4. Ion's native-blob wire-size advantage over JSON's base64 on byte-carrying
 *    messages (log-entry commands and snapshot chunks ride as Ion `blob`s).
 * 5. `name()` ("ion-binary"/"ion-text") resolving to the `application/ion`
 *    media type both the CoAP and HTTP transports put on the wire.
 *
 * Built only with the opt-in `ion` vcpkg feature (ion-c present); registered as
 * a CTest example that prints a short report and exits non-zero on any failure.
 */

#include <raft/ion_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/coap_utils.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using data_type = std::vector<std::byte>;

// (1) A Types bundle adopts Ion by naming ion_serializer as its
// serializer_type — nothing else changes, because every transport treats the
// serializer as an opaque byte-in/byte-out component. Ion, CBOR, and JSON all
// satisfy the same rpc_serializer concept, so they are interchangeable here.
struct ion_types {
    using serialized_data_type = data_type;
    using serializer_type = kythira::ion_serializer;
};
static_assert(kythira::rpc_serializer<ion_types::serializer_type, ion_types::serialized_data_type>,
              "ion_serializer must satisfy the rpc_serializer concept");

auto make_command(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return bytes;
}

int g_failures = 0;

auto check(bool ok, std::string_view what) -> void {
    std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] " << what << '\n';
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

auto main() -> int {
    kythira::ion_serializer ion;  // binary by default
    kythira::ion_serializer ion_text{kythira::ion_encoding::text};
    kythira::json_serializer json;

    std::cout << "=== Amazon Ion RPC Serializer example ===\n\n";

    // (2) Round-trip a representative message from each family (binary Ion).
    std::cout << "Round-trip over binary Ion:\n";

    kythira::request_vote_request<> rv{/*term=*/7, /*candidate_id=*/3,
                                       /*last_log_index=*/42, /*last_log_term=*/6};
    {
        auto back = ion.deserialize_request_vote_request(ion.serialize(rv));
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
        auto back = ion.deserialize_append_entries_request(ion.serialize(ae));
        bool entries_ok = back.entries().size() == ae.entries().size();
        for (std::size_t i = 0; entries_ok && i < ae.entries().size(); ++i) {
            entries_ok = back.entries()[i].index() == ae.entries()[i].index() &&
                         back.entries()[i].command() == ae.entries()[i].command() &&
                         back.entries()[i].type() == ae.entries()[i].type();
        }
        check(back.term() == ae.term() && back.leader_id() == ae.leader_id() && entries_ok,
              "append_entries_request (with log-entry commands as Ion blobs)");
    }

    kythira::install_snapshot_request<> snap;
    snap._term = 7;
    snap._leader_id = 3;
    snap._last_included_index = 100;
    snap._last_included_term = 6;
    snap._offset = 0;
    // Deliberately include high-bit bytes that have no text-safe interpretation,
    // to show the native-blob path carries arbitrary binary faithfully.
    snap._data = std::vector<std::byte>{std::byte{0x00}, std::byte{0xFF}, std::byte{0x80},
                                        std::byte{0x7F}, std::byte{0xC3}, std::byte{0x28}};
    snap._done = true;
    {
        auto back = ion.deserialize_install_snapshot_request(ion.serialize(snap));
        check(back.data() == snap.data() && back.done() == snap.done(),
              "install_snapshot_request (arbitrary binary data as an Ion blob)");
    }

    kythira::cluster_join_response<> join;
    join.accepted = false;
    join.redirect = kythira::peer_info<std::uint64_t, std::string>{/*node_id=*/1,
                                                                   /*address=*/"10.0.0.1:9443"};
    {
        auto back = ion.deserialize_cluster_join_response(ion.serialize(join));
        check(back.accepted == join.accepted && back.redirect.has_value() &&
                  back.redirect->node_id == join.redirect->node_id &&
                  back.redirect->address == join.redirect->address,
              "cluster_join_response (with redirect)");
    }

    // (3) Encoding-agnostic deserialize: serialize with the TEXT writer, decode
    // with the BINARY-configured instance's same deserialize_* call. Ion's
    // binary version marker makes the read path pick the right decoder itself.
    std::cout << "\nBinary/text interoperability:\n";
    {
        auto text_bytes = ion_text.serialize(ae);
        check(!text_bytes.empty() && static_cast<unsigned>(text_bytes[0]) != 0xE0u,
              "text encoding does not start with the binary version marker");
        auto back = ion.deserialize_append_entries_request(text_bytes);
        bool entries_ok = back.entries().size() == ae.entries().size();
        for (std::size_t i = 0; entries_ok && i < ae.entries().size(); ++i) {
            entries_ok = back.entries()[i].command() == ae.entries()[i].command();
        }
        check(back.term() == ae.term() && entries_ok,
              "binary-configured reader decodes text-Ion bytes");
    }
    {
        auto binary_bytes = ion.serialize(ae);
        check(binary_bytes.size() >= 4 && static_cast<unsigned>(binary_bytes[0]) == 0xE0u &&
                  static_cast<unsigned>(binary_bytes[1]) == 0x01u &&
                  static_cast<unsigned>(binary_bytes[2]) == 0x00u &&
                  static_cast<unsigned>(binary_bytes[3]) == 0xEAu,
              "binary encoding starts with the Ion binary version marker E0 01 00 EA");
        auto back = ion_text.deserialize_append_entries_request(binary_bytes);
        check(back.term() == ae.term(), "text-configured reader decodes binary-Ion bytes");
    }

    // (4) Wire-size comparison on the byte-carrying messages. Ion blobs avoid
    // JSON's ~33% base64 inflation of the same bytes.
    std::cout << "\nWire size (binary Ion vs JSON):\n";
    auto report_size = [](std::string_view label, std::size_t ion_bytes, std::size_t json_bytes) {
        std::cout << "  " << label << ": Ion " << ion_bytes << " B, JSON " << json_bytes << " B  ("
                  << (ion_bytes <= json_bytes ? "Ion <= JSON" : "Ion > JSON") << ")\n";
    };
    report_size("append_entries_request", ion.serialize(ae).size(), json.serialize(ae).size());
    report_size("install_snapshot_request", ion.serialize(snap).size(),
                json.serialize(snap).size());

    // (5) name() resolves to the application/ion media type both transports put
    // on the wire, for both encodings.
    std::cout << "\nContent-type labeling:\n";
    std::cout << "  ion.name()      = \"" << ion.name() << "\"\n";
    std::cout << "  ion_text.name() = \"" << ion_text.name() << "\"\n";
    const auto media_type = kythira::coap_utils::content_format_to_string(
        kythira::coap_utils::get_content_format_for_serializer(ion.name()));
    std::cout << "  derived media type = \"" << media_type << "\"\n";
    check(kythira::coap_utils::get_content_format_for_serializer(ion.name()) ==
                  kythira::coap_utils::coap_content_format::application_ion &&
              kythira::coap_utils::get_content_format_for_serializer(ion_text.name()) ==
                  kythira::coap_utils::coap_content_format::application_ion,
          "both ion-binary and ion-text resolve to application/ion");

    std::cout << "\n" << (g_failures == 0 ? "All checks passed." : "Some checks FAILED.") << '\n';
    return g_failures == 0 ? 0 : 1;
}
