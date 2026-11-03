// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// OIDC issuer signing-key life cycle (`.kiro/specs/iam-compatible-auth/`
// task 1, Requirements 1.1-1.5): the pure rotation planner driven by an
// injected clock, the ca_state_machine commands that replicate its steps,
// snapshot determinism and the snapshot successor check, and the published
// discovery and JWKS documents.

#define BOOST_TEST_MODULE oidc_issuer_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/ca_state_machine.hpp>
#include <raft/jwt.hpp>
#include <raft/oidc_issuer.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using kythira::oidc::create_key;
using kythira::oidc::oidc_key_record;
using kythira::oidc::retire_key;
using kythira::oidc::rotation_config;
using raft::testing::ca_state_machine;

namespace {

constexpr std::int64_t hour = 3600;
constexpr std::int64_t day = 24 * hour;
constexpr std::int64_t t0 = 1'800'000'000;  // 2027-01-15, an arbitrary epoch

auto fake_key(std::string kid, std::int64_t publish_after, std::int64_t sign_after)
    -> oidc_key_record {
    oidc_key_record k;
    k.kid = std::move(kid);
    k.alg = kythira::jose::jws_alg::es256;
    k.public_jwk = boost::json::object{{"kty", "EC"}, {"crv", "P-256"}, {"x", "AA"}, {"y", "AA"}};
    k.sealed_private_key = "sealed:" + k.kid;
    k.publish_after = publish_after;
    k.sign_after = sign_after;
    return k;
}

auto is_ok(const std::vector<std::byte>& result) -> bool {
    return result.empty();
}

auto error_of(const std::vector<std::byte>& result) -> std::string {
    if (result.empty()) {
        return {};
    }
    std::string s(result.size(), '\0');
    for (std::size_t i = 0; i < result.size(); ++i) {
        s[i] = static_cast<char>(result[i]);
    }
    return std::string(boost::json::parse(s).as_object().at("error").as_string());
}

// Drives a ca_state_machine the way the leader's maintenance thread does:
// at each tick, plan and apply steps until the planner has nothing to do.
struct leader_sim {
    ca_state_machine sm;
    rotation_config cfg;
    std::uint64_t index = 0;
    int next_kid = 0;

    void tick(std::int64_t now) {
        for (int guard = 0; guard < 8; ++guard) {
            auto step = kythira::oidc::plan_oidc_rotation(sm.oidc_keys(), now, cfg);
            if (const auto* c = std::get_if<create_key>(&step)) {
                auto key =
                    fake_key("k" + std::to_string(next_kid++), c->publish_after, c->sign_after);
                BOOST_REQUIRE(is_ok(
                    sm.apply(raft::testing::encode_record_oidc_key_command(key, now), ++index)));
            } else if (const auto* r = std::get_if<retire_key>(&step)) {
                BOOST_REQUIRE(is_ok(sm.apply(
                    raft::testing::encode_retire_oidc_key_command(r->kid, r->retire_after, now),
                    ++index)));
            } else {
                return;
            }
        }
        BOOST_FAIL("planner did not converge at t=" << now);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Rotation timeline (injected clock)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rotation_timeline_publishes_early_and_retains_late) {
    leader_sim sim;
    struct seen {
        std::int64_t first_published = -1;
        std::int64_t first_signed = -1;
        std::int64_t last_signed = -1;
        std::int64_t last_published = -1;
    };
    std::map<std::string, seen> history;

    // A year of hourly ticks covers four rotations.
    for (std::int64_t now = t0; now <= t0 + 400 * day; now += hour) {
        sim.tick(now);
        const auto* signer = kythira::oidc::signing_key(sim.sm.oidc_keys(), now);
        BOOST_REQUIRE_MESSAGE(signer != nullptr, "no signing key at t=" << now);
        auto& s = history[signer->kid];
        if (s.first_signed < 0) {
            s.first_signed = now;
        }
        s.last_signed = now;
        const auto jwks = kythira::oidc::jwks_document(sim.sm.oidc_keys(), now);
        for (const auto& jwk : jwks.at("keys").as_array()) {
            auto& p = history[std::string(jwk.as_object().at("kid").as_string())];
            if (p.first_published < 0) {
                p.first_published = now;
            }
            p.last_published = now;
        }
    }

    BOOST_TEST(history.size() >= 5u);  // k0 plus four successors
    for (const auto& [kid, s] : history) {
        BOOST_TEST_CONTEXT("key " << kid) {
            BOOST_TEST(s.first_published >= 0);
            if (kid == "k0") {
                // The very first key signs immediately; nobody holds a token yet.
                BOOST_TEST(s.first_signed == t0);
            } else {
                // Requirement 1.5: in the JWKS at least 24 h before it signs.
                BOOST_TEST(s.first_signed - s.first_published >= 24 * hour);
            }
            if (s.last_signed < t0 + 390 * day) {  // retired within the run
                // Still verifiable for the maximum token lifetime after its
                // last signature.
                BOOST_TEST(s.last_published - s.last_signed >= 12 * hour);
                // ...and then dropped, not kept forever.
                BOOST_TEST(s.last_published - s.last_signed <= 12 * hour + 2 * hour);
                // Each key signs for about the rotation period.
                BOOST_TEST(s.last_signed - s.first_signed >= 89 * day - hour);
                BOOST_TEST(s.last_signed - s.first_signed <= 90 * day);
            }
        }
    }
    // At steady state the JWKS holds the signer plus at most one neighbour.
    BOOST_TEST(sim.sm.oidc_keys().size() <= 2u);
}

BOOST_AUTO_TEST_CASE(planner_first_key_and_idle_steps) {
    rotation_config cfg;
    auto step = kythira::oidc::plan_oidc_rotation({}, t0, cfg);
    const auto* c = std::get_if<create_key>(&step);
    BOOST_REQUIRE(c != nullptr);
    BOOST_TEST(c->publish_after == t0);
    BOOST_TEST(c->sign_after == t0);

    std::vector<oidc_key_record> keys{fake_key("a", t0, t0)};
    BOOST_TEST(std::holds_alternative<std::monostate>(
        kythira::oidc::plan_oidc_rotation(keys, t0 + 30 * day, cfg)));
    // Due at sign_after + 90 d - 24 h.
    BOOST_TEST(std::holds_alternative<std::monostate>(
        kythira::oidc::plan_oidc_rotation(keys, t0 + 89 * day - 1, cfg)));
    step = kythira::oidc::plan_oidc_rotation(keys, t0 + 89 * day, cfg);
    c = std::get_if<create_key>(&step);
    BOOST_REQUIRE(c != nullptr);
    BOOST_TEST(c->sign_after == t0 + 90 * day);

    // A pending successor and no usable signer (only retiring keys): wait.
    keys = {fake_key("a", t0, t0), fake_key("b", t0, t0 + day)};
    keys[0].retire_after = t0 + 2 * day;
    BOOST_TEST(std::holds_alternative<std::monostate>(
        kythira::oidc::plan_oidc_rotation(keys, t0 + hour, cfg)));
}

BOOST_AUTO_TEST_CASE(planner_retires_superseded_keys_and_signer_ties_are_stable) {
    rotation_config cfg;
    std::vector<oidc_key_record> keys{fake_key("a", t0, t0), fake_key("b", t0, t0 + day)};
    // Before b signs, a is the signer and nothing is retired.
    BOOST_TEST(kythira::oidc::signing_key(keys, t0 + hour)->kid == "a");
    // After, a is superseded.
    BOOST_TEST(kythira::oidc::signing_key(keys, t0 + day)->kid == "b");
    auto step = kythira::oidc::plan_oidc_rotation(keys, t0 + day + 5, cfg);
    const auto* r = std::get_if<retire_key>(&step);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->kid == "a");
    BOOST_TEST(r->retire_after == t0 + day + 5 + 12 * hour);

    // Equal sign_after: the larger kid signs on every member.
    keys = {fake_key("y", t0, t0), fake_key("x", t0, t0)};
    BOOST_TEST(kythira::oidc::signing_key(keys, t0)->kid == "y");
    // A retiring key never signs.
    keys[0].retire_after = t0 + day;
    BOOST_TEST(kythira::oidc::signing_key(keys, t0)->kid == "x");
}

// ---------------------------------------------------------------------------
// ca_state_machine commands
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(state_machine_records_retires_and_prunes_by_command_time) {
    ca_state_machine sm;
    auto a = fake_key("a", t0, t0);
    BOOST_TEST(is_ok(sm.apply(raft::testing::encode_record_oidc_key_command(a, t0), 1)));
    BOOST_TEST(error_of(sm.apply(raft::testing::encode_record_oidc_key_command(a, t0), 2)) ==
               "duplicate_kid");
    BOOST_TEST(error_of(sm.apply(raft::testing::encode_retire_oidc_key_command("zz", t0, t0), 3)) ==
               "kid_not_found");
    BOOST_TEST(is_ok(sm.apply(
        raft::testing::encode_retire_oidc_key_command("a", t0 + 12 * hour, t0 + hour), 4)));
    BOOST_TEST(error_of(sm.apply(
                   raft::testing::encode_retire_oidc_key_command("a", t0 + day, t0 + hour), 5)) ==
               "already_retiring");
    BOOST_REQUIRE(sm.oidc_keys().size() == 1u);
    BOOST_TEST(*sm.oidc_keys()[0].retire_after == t0 + 12 * hour);

    // The key is dropped only by a command stamped at or after retire_after.
    auto b = fake_key("b", t0 + 2 * hour, t0 + 2 * hour);
    BOOST_TEST(is_ok(sm.apply(raft::testing::encode_record_oidc_key_command(b, t0 + 2 * hour), 6)));
    BOOST_TEST(sm.oidc_keys().size() == 2u);
    auto c = fake_key("c", t0 + 12 * hour, t0 + 36 * hour);
    BOOST_TEST(
        is_ok(sm.apply(raft::testing::encode_record_oidc_key_command(c, t0 + 12 * hour), 7)));
    BOOST_TEST(sm.oidc_keys().size() == 2u);  // a pruned; b and c remain
    BOOST_TEST(sm.oidc_keys()[0].kid == "b");

    // A leader whose clock trails the previous one cannot move time back.
    BOOST_TEST(is_ok(sm.apply(
        raft::testing::encode_retire_oidc_key_command("b", t0 + 50 * hour, t0 + 5 * hour), 8)));
    BOOST_TEST(sm.oidc_clock() == t0 + 12 * hour);
}

BOOST_AUTO_TEST_CASE(state_machine_rejects_malformed_oidc_commands) {
    ca_state_machine sm;
    auto bad_alg = boost::json::object{{"type", 5}, {"now", t0}};
    auto key = kythira::oidc::key_record_to_json(fake_key("a", t0, t0));
    for (const auto& [k, v] : key) {
        bad_alg[k] = v;
    }
    bad_alg["alg"] = "HS256";
    BOOST_CHECK_THROW(
        (void)sm.apply(raft::testing::ca_state_machine_detail::bytes_from_json(bad_alg), 1),
        std::invalid_argument);
    auto signs_early = bad_alg;
    signs_early["alg"] = "ES256";
    signs_early["sign_after"] = t0 - 1;
    BOOST_CHECK_THROW(
        (void)sm.apply(raft::testing::ca_state_machine_detail::bytes_from_json(signs_early), 2),
        std::invalid_argument);
    auto pre_retired = bad_alg;
    pre_retired["alg"] = "ES256";
    pre_retired["retire_after"] = t0;
    BOOST_CHECK_THROW(
        (void)sm.apply(raft::testing::ca_state_machine_detail::bytes_from_json(pre_retired), 3),
        std::invalid_argument);
    BOOST_TEST(sm.oidc_keys().empty());
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(snapshot_restore_is_deterministic) {
    leader_sim sim;
    for (std::int64_t now = t0; now <= t0 + 200 * day; now += 6 * hour) {
        sim.tick(now);
    }
    const auto snapshot = sim.sm.get_state();

    // A follower restoring the snapshot, then both applying the same
    // further commands, stays byte-identical, and picks the same signer.
    ca_state_machine follower;
    follower.restore_from_snapshot(snapshot, sim.index);
    BOOST_TEST((follower.get_state() == snapshot));

    leader_sim replay;
    replay.sm = follower;
    replay.index = sim.index;
    replay.next_kid = sim.next_kid;
    for (std::int64_t now = t0 + 200 * day; now <= t0 + 300 * day; now += 6 * hour) {
        sim.tick(now);
        replay.tick(now);
        BOOST_REQUIRE((sim.sm.get_state() == replay.sm.get_state()));
        BOOST_TEST(kythira::oidc::signing_key(sim.sm.oidc_keys(), now)->kid ==
                   kythira::oidc::signing_key(replay.sm.oidc_keys(), now)->kid);
    }
}

BOOST_AUTO_TEST_CASE(snapshots_without_oidc_fields_are_unchanged_and_restore) {
    ca_state_machine sm;
    const auto before = sm.get_state();
    const auto text = std::string(reinterpret_cast<const char*>(before.data()), before.size());
    BOOST_TEST(text.find("oidc") == std::string::npos);

    // A snapshot written before this change (no oidc_* keys) restores with
    // no keys, and the planner then creates the first one.
    ca_state_machine restored;
    restored.restore_from_snapshot(before, 1);
    BOOST_TEST(restored.oidc_keys().empty());
    BOOST_TEST(std::holds_alternative<create_key>(
        kythira::oidc::plan_oidc_rotation(restored.oidc_keys(), t0, rotation_config{})));
}

namespace {

// A bootstrapped replica holding `keys` with OIDC clock `clock`.
auto bootstrapped_with(const std::vector<oidc_key_record>& keys, std::int64_t clock)
    -> ca_state_machine {
    ca_state_machine sm;
    std::uint64_t i = 0;
    BOOST_REQUIRE(
        is_ok(sm.apply(raft::testing::encode_bootstrap_ca_command("CERT", "SEALED"), ++i)));
    for (const auto& k : keys) {
        auto rec = k;
        rec.retire_after.reset();
        BOOST_REQUIRE(is_ok(sm.apply(raft::testing::encode_record_oidc_key_command(rec, t0), ++i)));
        if (k.retire_after) {
            BOOST_REQUIRE(is_ok(sm.apply(
                raft::testing::encode_retire_oidc_key_command(k.kid, *k.retire_after, t0), ++i)));
        }
    }
    // Refused (kid_not_found), but its "now" still advances the OIDC clock.
    BOOST_REQUIRE(error_of(sm.apply(
                      raft::testing::encode_retire_oidc_key_command("missing", 0, clock), ++i)) ==
                  "kid_not_found");
    BOOST_REQUIRE(sm.oidc_clock() == clock);
    return sm;
}

auto snapshot_with(const ca_state_machine& base, const std::vector<oidc_key_record>& keys,
                   std::int64_t clock) -> std::vector<std::byte> {
    auto obj =
        raft::testing::ca_state_machine_detail::json_from_bytes(base.get_state()).as_object();
    boost::json::array arr;
    for (const auto& k : keys) {
        arr.emplace_back(kythira::oidc::key_record_to_json(k));
    }
    obj["oidc_keys"] = std::move(arr);
    obj["oidc_clock"] = clock;
    return raft::testing::ca_state_machine_detail::bytes_from_json(obj);
}

}  // namespace

BOOST_AUTO_TEST_CASE(snapshot_successor_check_guards_signing_keys) {
    auto a = fake_key("a", t0, t0);
    auto b = fake_key("b", t0, t0 + day);
    auto sm = bootstrapped_with({a, b}, t0 + hour);

    auto must_reject = [&](const std::vector<std::byte>& snap, const char* why) {
        auto copy = sm;
        BOOST_CHECK_THROW(copy.restore_from_snapshot(snap, 99), std::invalid_argument);
        BOOST_TEST_MESSAGE("rejected: " << why);
        BOOST_TEST((copy.get_state() == sm.get_state()));
    };

    // Swapping the public key under the same kid.
    auto swapped = a;
    swapped.public_jwk["x"] = "BB";
    must_reject(snapshot_with(sm, {swapped, b}, t0 + hour), "altered key");
    // Dropping a key that was never retired.
    must_reject(snapshot_with(sm, {b}, t0 + hour), "dropped live key");
    // Moving the clock backwards.
    must_reject(snapshot_with(sm, {a, b}, t0), "clock backwards");

    // Legitimate successors: a retires, then is pruned once the clock passes.
    auto a_retiring = a;
    a_retiring.retire_after = t0 + 2 * day;
    auto ok1 = sm;
    ok1.restore_from_snapshot(snapshot_with(sm, {a_retiring, b}, t0 + day), 99);
    BOOST_TEST(ok1.oidc_keys().size() == 2u);
    auto ok2 = ok1;
    ok2.restore_from_snapshot(snapshot_with(ok1, {b}, t0 + 2 * day), 100);
    BOOST_TEST(ok2.oidc_keys().size() == 1u);

    // ...but not before its retire_after, and the retirement time is fixed.
    auto early = ok1;
    BOOST_CHECK_THROW(early.restore_from_snapshot(snapshot_with(ok1, {b}, t0 + day + hour), 100),
                      std::invalid_argument);
    auto moved = a_retiring;
    moved.retire_after = t0 + 30 * day;
    BOOST_CHECK_THROW(early.restore_from_snapshot(snapshot_with(ok1, {moved, b}, t0 + day), 100),
                      std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Real keys: sealing and published documents
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(sealed_key_round_trips_and_signs) {
    using namespace kythira::jose;
    auto key = generate_signing_key(jws_alg::rs256);
    oidc_key_record rec;
    rec.public_jwk = public_jwk(key.get());
    rec.kid = jwk_thumbprint(rec.public_jwk);
    rec.alg = jws_alg::rs256;
    rec.sealed_private_key =
        raft::testing::encrypt_ca_private_key(private_key_to_pem(key.get()), "unseal-passphrase");
    rec.publish_after = rec.sign_after = t0;
    BOOST_TEST(rec.sealed_private_key.find("PRIVATE KEY") == std::string::npos);

    ca_state_machine sm;
    BOOST_TEST(is_ok(sm.apply(raft::testing::encode_record_oidc_key_command(rec, t0), 1)));
    const auto& stored = sm.oidc_keys().at(0);
    auto unsealed = private_key_from_pem(
        raft::testing::decrypt_ca_private_key(stored.sealed_private_key, "unseal-passphrase"));
    const auto token =
        sign_compact({{"kid", stored.kid}}, R"({"sub":"node:1"})", unsealed.get(), stored.alg);

    // A relying party verifies it using only the published JWKS.
    auto jwks = kythira::oidc::jwks_document(sm.oidc_keys(), t0);
    const auto& jwk = jwks.at("keys").as_array().at(0).as_object();
    BOOST_TEST(jwk.at("kid").as_string() == rec.kid);
    BOOST_TEST(jwk.at("use").as_string() == "sig");
    BOOST_TEST(jwk.at("alg").as_string() == "RS256");
    BOOST_TEST(!jwk.contains("d"));
    BOOST_TEST(jwk_thumbprint(jwk) == rec.kid);
    auto pub = public_key_from_jwk(jwk);
    BOOST_TEST(verify_compact(token, pub.get(), jws_alg::rs256).has_value());
}

BOOST_AUTO_TEST_CASE(jwks_hides_unpublished_keys_and_is_sorted) {
    std::vector<oidc_key_record> keys{fake_key("z", t0, t0), fake_key("m", t0 + day, t0 + 2 * day),
                                      fake_key("b", t0, t0)};
    auto at0 = kythira::oidc::jwks_document(keys, t0).at("keys").as_array();
    BOOST_REQUIRE(at0.size() == 2u);
    BOOST_TEST(at0[0].as_object().at("kid").as_string() == "b");
    BOOST_TEST(at0[1].as_object().at("kid").as_string() == "z");
    BOOST_TEST(kythira::oidc::jwks_document(keys, t0 + day).at("keys").as_array().size() == 3u);
    // A retiring key is published until its retire_after, not after.
    keys[0].retire_after = t0 + 2 * day;
    BOOST_TEST(kythira::oidc::jwks_document(keys, t0 + 2 * day - 1).at("keys").as_array().size() ==
               3u);
    BOOST_TEST(kythira::oidc::jwks_document(keys, t0 + 2 * day).at("keys").as_array().size() == 2u);
}

BOOST_AUTO_TEST_CASE(discovery_document_has_the_required_members) {
    auto doc = kythira::oidc::discovery_document("https://issuer.example",
                                                 "https://issuer.example/.well-known/jwks.json",
                                                 kythira::jose::jws_alg::rs256);
    BOOST_TEST(doc.at("issuer").as_string() == "https://issuer.example");
    BOOST_TEST(doc.at("jwks_uri").as_string() == "https://issuer.example/.well-known/jwks.json");
    BOOST_TEST(doc.at("response_types_supported").as_array().at(0).as_string() == "id_token");
    BOOST_TEST(doc.at("subject_types_supported").as_array().at(0).as_string() == "public");
    BOOST_TEST(doc.at("id_token_signing_alg_values_supported").as_array().at(0).as_string() ==
               "RS256");
    BOOST_TEST(doc.at("claims_supported").as_array().size() == 10u);
}

BOOST_AUTO_TEST_CASE(issuer_url_validation) {
    using kythira::oidc::issuer_url_error;
    BOOST_TEST(!issuer_url_error("https://ca.example.com").has_value());
    BOOST_TEST(!issuer_url_error("https://ca.example.com:8443/tenants/a").has_value());
    BOOST_TEST(issuer_url_error("http://ca.example.com").has_value());
    BOOST_TEST(issuer_url_error("https://").has_value());
    BOOST_TEST(issuer_url_error("https:///path").has_value());
    BOOST_TEST(issuer_url_error("https://ca.example.com/").has_value());
    BOOST_TEST(issuer_url_error("https://ca.example.com?x=1").has_value());
    BOOST_TEST(issuer_url_error("https://ca.example.com#f").has_value());
    BOOST_TEST(issuer_url_error("https://ca.example .com").has_value());
}
