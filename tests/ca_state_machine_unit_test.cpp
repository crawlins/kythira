#define BOOST_TEST_MODULE ca_state_machine_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/ca_state_machine.hpp>

#include <chrono>
#include <limits>
#include <string>
#include <vector>

using namespace raft::testing;

namespace {

auto make_ledger_entry(std::uint64_t serial, const std::string& subject) -> ca_ledger_entry {
    ca_ledger_entry e;
    e.serial = serial;
    e.subject = subject;
    e.dns_names = {subject + ".example.com"};
    e.certificate_pem =
        "-----BEGIN CERTIFICATE-----\nMOCK-" + subject + "\n-----END CERTIFICATE-----\n";
    e.not_before = std::chrono::system_clock::now();
    e.not_after = e.not_before + std::chrono::hours(24 * 30);
    return e;
}

}  // namespace

// ── AES-256-GCM / PBKDF2 round-trip ─────────────────────────────────────────

BOOST_AUTO_TEST_CASE(encrypt_decrypt_round_trip) {
    std::string key_pem =
        "-----BEGIN PRIVATE KEY-----\nMOCK-KEY-MATERIAL\n-----END PRIVATE KEY-----\n";
    auto encrypted = encrypt_ca_private_key(key_pem, "correct horse battery staple");
    BOOST_TEST(encrypted.find("BEGIN PRIVATE KEY") ==
               std::string::npos);  // never plaintext in the ciphertext blob

    auto decrypted = decrypt_ca_private_key(encrypted, "correct horse battery staple");
    BOOST_TEST(decrypted == key_pem);
}

BOOST_AUTO_TEST_CASE(decrypt_wrong_passphrase_throws) {
    std::string key_pem =
        "-----BEGIN PRIVATE KEY-----\nMOCK-KEY-MATERIAL\n-----END PRIVATE KEY-----\n";
    auto encrypted = encrypt_ca_private_key(key_pem, "correct passphrase");
    BOOST_CHECK_THROW(decrypt_ca_private_key(encrypted, "wrong passphrase"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(encrypt_produces_distinct_ciphertext_each_call) {
    // Fresh salt/nonce per call — same plaintext must not produce identical
    // ciphertext (a static nonce would be a catastrophic GCM misuse).
    std::string key_pem = "-----BEGIN PRIVATE KEY-----\nMOCK\n-----END PRIVATE KEY-----\n";
    auto a = encrypt_ca_private_key(key_pem, "passphrase");
    auto b = encrypt_ca_private_key(key_pem, "passphrase");
    BOOST_TEST(a != b);
    BOOST_TEST(decrypt_ca_private_key(a, "passphrase") == key_pem);
    BOOST_TEST(decrypt_ca_private_key(b, "passphrase") == key_pem);
}

// ── apply() dispatch ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(bootstrap_ca_sets_root_material) {
    ca_state_machine sm;
    BOOST_TEST(!sm.has_root_material());

    auto encrypted_key = encrypt_ca_private_key("mock-key-pem", "unseal-passphrase");
    auto cmd = encode_bootstrap_ca_command("mock-root-cert-pem", encrypted_key);
    auto result = sm.apply(cmd, 1);

    BOOST_TEST(result.empty());
    BOOST_TEST(sm.has_root_material());
    BOOST_TEST(sm.root_certificate_pem() == "mock-root-cert-pem");
    BOOST_TEST(sm.encrypted_bootstrap_material() == encrypted_key);
}

BOOST_AUTO_TEST_CASE(duplicate_bootstrap_ca_rejected_deterministically) {
    ca_state_machine sm;
    auto cmd1 = encode_bootstrap_ca_command("root-1", encrypt_ca_private_key("key-1", "pass"));
    auto result1 = sm.apply(cmd1, 1);
    BOOST_TEST(result1.empty());

    auto cmd2 = encode_bootstrap_ca_command("root-2", encrypt_ca_private_key("key-2", "pass"));
    auto result2 = sm.apply(cmd2, 2);

    // Structured error, not a thrown exception — and the original material is untouched.
    BOOST_TEST(!result2.empty());
    std::string result2_str(reinterpret_cast<const char*>(result2.data()), result2.size());
    BOOST_TEST(result2_str.find("already_bootstrapped") != std::string::npos);
    BOOST_TEST(sm.root_certificate_pem() == "root-1");
}

BOOST_AUTO_TEST_CASE(record_issuance_appends_to_ledger) {
    ca_state_machine sm;
    auto entry = make_ledger_entry(42, "client-a");
    auto cmd = encode_record_issuance_command(entry);
    auto result = sm.apply(cmd, 1);

    BOOST_TEST(result.empty());
    BOOST_REQUIRE(sm.ledger().size() == 1);
    BOOST_TEST(sm.ledger()[0].serial == 42);
    BOOST_TEST(sm.ledger()[0].subject == "client-a");
    BOOST_TEST(sm.ledger()[0].certificate_pem == entry.certificate_pem);
    BOOST_TEST(!sm.ledger()[0].revoked_at.has_value());
}

// Regression: certificate serial numbers are std::uint64_t and legitimately
// use the full width (RFC 5280 wants high-entropy, non-sequential serials —
// certificate_authority generates them accordingly), so roughly half of all
// real serials have bit 63 set and don't fit in an int64_t. json_to_ledger_entry()
// previously decoded via to_number<std::int64_t>() instead of
// to_number<std::uint64_t>(), which boost::json rejects with a "not exact"
// error for such values — surfacing only probabilistically (whichever random
// serial a test happened to generate), which is exactly how this escaped
// coverage the first time: every other test here uses small, hand-picked
// serials that never exercise the high bit.
BOOST_AUTO_TEST_CASE(record_issuance_survives_serial_exceeding_int64_max) {
    ca_state_machine sm;
    std::uint64_t large_serial = std::numeric_limits<std::uint64_t>::max();
    auto entry = make_ledger_entry(large_serial, "client-large-serial");
    auto result = sm.apply(encode_record_issuance_command(entry), 1);

    BOOST_TEST(result.empty());
    BOOST_REQUIRE(sm.ledger().size() == 1);
    BOOST_TEST(sm.ledger()[0].serial == large_serial);

    // Also exercise revocation of that same over-int64_t-range serial.
    auto revoked_at = std::chrono::system_clock::now();
    auto revoke_result = sm.apply(encode_record_revocation_command(large_serial, revoked_at), 2);
    BOOST_TEST(revoke_result.empty());
    BOOST_REQUIRE(sm.ledger()[0].revoked_at.has_value());

    // And the snapshot round-trip, which serializes/deserializes the whole
    // ledger through the same JSON encoding.
    auto snapshot = sm.get_state();
    ca_state_machine restored;
    restored.restore_from_snapshot(snapshot, 2);
    BOOST_REQUIRE(restored.ledger().size() == 1);
    BOOST_TEST(restored.ledger()[0].serial == large_serial);
}

BOOST_AUTO_TEST_CASE(record_revocation_marks_matching_serial) {
    ca_state_machine sm;
    sm.apply(encode_record_issuance_command(make_ledger_entry(7, "client-b")), 1);

    auto revoked_at = std::chrono::system_clock::now();
    auto result = sm.apply(encode_record_revocation_command(7, revoked_at), 2);

    BOOST_TEST(result.empty());
    BOOST_REQUIRE(sm.ledger().size() == 1);
    BOOST_REQUIRE(sm.ledger()[0].revoked_at.has_value());
    // Round-tripped through epoch-seconds encoding — compare at second granularity.
    auto expected = std::chrono::duration_cast<std::chrono::seconds>(revoked_at.time_since_epoch());
    auto actual = std::chrono::duration_cast<std::chrono::seconds>(
        sm.ledger()[0].revoked_at->time_since_epoch());
    BOOST_TEST(expected.count() == actual.count());
}

BOOST_AUTO_TEST_CASE(record_revocation_unknown_serial_rejected_deterministically) {
    ca_state_machine sm;
    sm.apply(encode_record_issuance_command(make_ledger_entry(1, "client-c")), 1);

    auto result =
        sm.apply(encode_record_revocation_command(999, std::chrono::system_clock::now()), 2);

    BOOST_TEST(!result.empty());
    std::string result_str(reinterpret_cast<const char*>(result.data()), result.size());
    BOOST_TEST(result_str.find("serial_not_found") != std::string::npos);
    // Untouched — the ledger's one entry is still unrevoked.
    BOOST_TEST(!sm.ledger()[0].revoked_at.has_value());
}

BOOST_AUTO_TEST_CASE(noop_command_leaves_state_unchanged) {
    ca_state_machine sm;
    sm.apply(encode_bootstrap_ca_command("root", encrypt_ca_private_key("key", "pass")), 1);
    sm.apply(encode_record_issuance_command(make_ledger_entry(1, "alice")), 2);
    auto state_before = sm.get_state();

    auto result = sm.apply(encode_noop_command(), 3);

    BOOST_TEST(result.empty());
    BOOST_TEST(sm.get_state() == state_before);
    BOOST_TEST(sm.last_applied_index() == 3);
}

// ── Property 15: determinism across replicas ────────────────────────────────

BOOST_AUTO_TEST_CASE(property_apply_is_deterministic_across_replicas) {
    ca_state_machine replica_a;
    ca_state_machine replica_b;

    auto encrypted_key = encrypt_ca_private_key("shared-key-pem", "shared-passphrase");
    std::vector<std::vector<std::byte>> commands = {
        encode_bootstrap_ca_command("shared-root-pem", encrypted_key),
        encode_record_issuance_command(make_ledger_entry(1, "alice")),
        encode_record_issuance_command(make_ledger_entry(2, "bob")),
        encode_record_revocation_command(1, std::chrono::system_clock::now()),
        // A duplicate bootstrap and an unknown-serial revocation exercise the
        // deterministic-rejection paths identically on both replicas too.
        encode_bootstrap_ca_command("attempted-second-root", encrypt_ca_private_key("x", "y")),
        encode_record_revocation_command(999, std::chrono::system_clock::now()),
    };

    std::uint64_t index = 1;
    for (const auto& cmd : commands) {
        auto result_a = replica_a.apply(cmd, index);
        auto result_b = replica_b.apply(cmd, index);
        BOOST_TEST(result_a == result_b);
        ++index;
    }

    BOOST_TEST(replica_a.get_state() == replica_b.get_state());
}

// ── restore_from_snapshot(get_state()) round-trip ───────────────────────────

BOOST_AUTO_TEST_CASE(snapshot_round_trip_preserves_state) {
    ca_state_machine original;
    auto encrypted_key = encrypt_ca_private_key("round-trip-key", "passphrase");
    original.apply(encode_bootstrap_ca_command("round-trip-root", encrypted_key), 1);
    original.apply(encode_record_issuance_command(make_ledger_entry(1, "alice")), 2);
    original.apply(encode_record_issuance_command(make_ledger_entry(2, "bob")), 3);
    original.apply(encode_record_revocation_command(2, std::chrono::system_clock::now()), 4);

    auto snapshot = original.get_state();

    ca_state_machine restored;
    restored.restore_from_snapshot(snapshot, 4);

    BOOST_TEST(restored.has_root_material());
    BOOST_TEST(restored.root_certificate_pem() == "round-trip-root");
    BOOST_TEST(restored.encrypted_bootstrap_material() == encrypted_key);
    BOOST_REQUIRE(restored.ledger().size() == 2);
    BOOST_TEST(restored.ledger()[0].serial == 1);
    BOOST_TEST(!restored.ledger()[0].revoked_at.has_value());
    BOOST_TEST(restored.ledger()[1].serial == 2);
    BOOST_TEST(restored.ledger()[1].revoked_at.has_value());
    BOOST_TEST(restored.last_applied_index() == 4);

    // Byte-identical re-serialization confirms a full round-trip, not just a
    // field-by-field coincidence.
    BOOST_TEST(restored.get_state() == snapshot);

    // Decrypting the round-tripped material with the original passphrase
    // still recovers the original key — the encrypted blob itself round-trips
    // through JSON/base64 unmodified.
    BOOST_TEST(decrypt_ca_private_key(restored.encrypted_bootstrap_material(), "passphrase") ==
               "round-trip-key");
}

BOOST_AUTO_TEST_CASE(empty_snapshot_resets_to_fresh_state) {
    ca_state_machine sm;
    sm.apply(encode_bootstrap_ca_command("root", encrypt_ca_private_key("key", "pass")), 1);
    BOOST_TEST(sm.has_root_material());

    sm.restore_from_snapshot({}, 0);
    BOOST_TEST(!sm.has_root_material());
    BOOST_TEST(sm.ledger().empty());
}
