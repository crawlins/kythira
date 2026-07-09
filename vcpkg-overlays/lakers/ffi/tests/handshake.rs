//! End-to-end smoke test for the FFI surface, using the same test vectors as
//! upstream lakers' own `test_handshake` (lib/src/lib.rs). Exercises the
//! bindings exactly as the C++ side will: two independent handles, byte
//! buffers passed between them, no shared Rust state.

use lakers_kythira_ffi::*;
use std::os::raw::c_int;

fn cred_i() -> Vec<u8> {
    hex(
        "A2027734322D35302D33312D46462D45462D33372D33322D333908A101A5010202412B2001215820AC75E9ECE3\
         E50BFC8ED60399889522405C47BF16DF96660A41298CB4307F7EB62258206E5DE611388A4B8A8211334AC7D37EC\
         B52A387D257E6DB3C2A93DF21FF3AFFC8",
    )
}
fn i_key() -> Vec<u8> {
    hex("fb13adeb6518cee5f88417660841142e830a81fe334380a953406a1305e8706b")
}
fn r_key() -> Vec<u8> {
    hex("72cc4761dbd4c78f758931aa589d348d1ef874a7e303ede2f140dcf3e6aa4aac")
}
fn cred_r() -> Vec<u8> {
    hex(
        "A2026008A101A5010202410A2001215820BBC34960526EA4D32E940CAD2A234148DDC21791A12AFBCBAC9362204\
         6DD44F02258204519E257236B2A0CE2023F0931F1F386CA7AFDA64FCDE0108C224C51EABF6072",
    )
}

// Tiny local hex decoder (avoids adding a dev-dependency just for this test).
const fn hex_val(c: u8) -> u8 {
    match c {
        b'0'..=b'9' => c - b'0',
        b'A'..=b'F' => c - b'A' + 10,
        b'a'..=b'f' => c - b'a' + 10,
        _ => panic!("invalid hex digit"),
    }
}

fn hex(s: &str) -> Vec<u8> {
    let bytes = s.as_bytes();
    assert_eq!(bytes.len() % 2, 0);
    bytes
        .chunks(2)
        .map(|pair| (hex_val(pair[0]) << 4) | hex_val(pair[1]))
        .collect()
}

fn check(rc: c_int, what: &str) {
    assert_eq!(rc, LAKERS_OK, "{what} failed with code {rc}");
}

#[test]
fn full_handshake_and_matching_oscore_export() {
    let cred_i = cred_i();
    let i_key = i_key();
    let cred_r = cred_r();
    let r_key = r_key();

    unsafe {
        let initiator = lakers_initiator_new();
        assert!(!initiator.is_null());
        let responder = lakers_responder_new(r_key.as_ptr(), cred_r.as_ptr(), cred_r.len());
        assert!(!responder.is_null());

        check(
            lakers_initiator_set_identity(initiator, i_key.as_ptr(), cred_i.as_ptr(), cred_i.len()),
            "set_identity",
        );

        let mut msg1 = [0u8; 256];
        let mut msg1_len = 0usize;
        check(
            lakers_initiator_prepare_message_1(initiator, msg1.as_mut_ptr(), msg1.len(), &mut msg1_len),
            "prepare_message_1",
        );

        check(
            lakers_responder_process_message_1(responder, msg1.as_ptr(), msg1_len),
            "process_message_1",
        );

        let mut msg2 = [0u8; 256];
        let mut msg2_len = 0usize;
        check(
            lakers_responder_prepare_message_2(responder, msg2.as_mut_ptr(), msg2.len(), &mut msg2_len),
            "prepare_message_2",
        );

        check(
            lakers_initiator_parse_message_2(initiator, msg2.as_ptr(), msg2_len),
            "parse_message_2",
        );
        check(
            lakers_initiator_verify_message_2(initiator, cred_r.as_ptr(), cred_r.len()),
            "verify_message_2",
        );

        let mut msg3 = [0u8; 256];
        let mut msg3_len = 0usize;
        check(
            lakers_initiator_prepare_message_3(initiator, msg3.as_mut_ptr(), msg3.len(), &mut msg3_len),
            "prepare_message_3",
        );
        check(lakers_initiator_complete(initiator), "initiator_complete");

        check(
            lakers_responder_parse_message_3(responder, msg3.as_ptr(), msg3_len),
            "parse_message_3",
        );
        check(
            lakers_responder_verify_message_3(responder, cred_i.as_ptr(), cred_i.len()),
            "verify_message_3",
        );

        let mut i_secret = [0u8; 16];
        let mut r_secret = [0u8; 16];
        let mut i_salt = [0u8; 8];
        let mut r_salt = [0u8; 8];
        check(
            lakers_initiator_exporter(initiator, 0, i_secret.as_mut_ptr(), i_secret.len()),
            "initiator exporter secret",
        );
        check(
            lakers_initiator_exporter(initiator, 1, i_salt.as_mut_ptr(), i_salt.len()),
            "initiator exporter salt",
        );
        check(
            lakers_responder_exporter(responder, 0, r_secret.as_mut_ptr(), r_secret.len()),
            "responder exporter secret",
        );
        check(
            lakers_responder_exporter(responder, 1, r_salt.as_mut_ptr(), r_salt.len()),
            "responder exporter salt",
        );

        assert_eq!(i_secret, r_secret, "OSCORE master secret must match between initiator/responder");
        assert_eq!(i_salt, r_salt, "OSCORE master salt must match between initiator/responder");

        lakers_initiator_free(initiator);
        lakers_responder_free(responder);
    }
}

#[test]
fn mismatched_peer_credential_is_rejected() {
    let cred_i = cred_i();
    let i_key = i_key();
    let cred_r = cred_r();
    let r_key = r_key();
    // A credential that doesn't match what the responder actually holds.
    let wrong_cred_r = cred_i.clone();

    unsafe {
        let initiator = lakers_initiator_new();
        let responder = lakers_responder_new(r_key.as_ptr(), cred_r.as_ptr(), cred_r.len());

        check(
            lakers_initiator_set_identity(initiator, i_key.as_ptr(), cred_i.as_ptr(), cred_i.len()),
            "set_identity",
        );
        let mut msg1 = [0u8; 256];
        let mut msg1_len = 0usize;
        check(
            lakers_initiator_prepare_message_1(initiator, msg1.as_mut_ptr(), msg1.len(), &mut msg1_len),
            "prepare_message_1",
        );
        check(
            lakers_responder_process_message_1(responder, msg1.as_ptr(), msg1_len),
            "process_message_1",
        );
        let mut msg2 = [0u8; 256];
        let mut msg2_len = 0usize;
        check(
            lakers_responder_prepare_message_2(responder, msg2.as_mut_ptr(), msg2.len(), &mut msg2_len),
            "prepare_message_2",
        );
        check(
            lakers_initiator_parse_message_2(initiator, msg2.as_ptr(), msg2_len),
            "parse_message_2",
        );

        // Verifying against the wrong peer credential must fail the MAC
        // check rather than silently succeeding.
        let rc = lakers_initiator_verify_message_2(initiator, wrong_cred_r.as_ptr(), wrong_cred_r.len());
        assert!(rc != LAKERS_OK, "verify_message_2 with wrong peer credential must fail");

        lakers_initiator_free(initiator);
        lakers_responder_free(responder);
    }
}
