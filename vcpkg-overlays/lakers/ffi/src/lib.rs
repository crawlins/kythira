//! Minimal C FFI over the `lakers` EDHOC crate (RFC 9528), covering exactly
//! what Kythira's coap-transport-security OSCORE bootstrap needs:
//! Initiator (coap_client) and Responder (coap_server) roles for the
//! 3-message EDHOC exchange (no message_4 — both sides call
//! completed_without_message_4(), a standard RFC 9528 completion mode), plus
//! edhoc_exporter() to derive OSCORE sender_id/recipient_id/master_secret/
//! master_salt.
//!
//! This intentionally does not wrap the upstream `lakers-c` crate: that
//! crate's C bindings only expose the Initiator role (see
//! vcpkg-overlays/lakers/README.md), and its #![no_std] / #[repr(C)]
//! state-struct-copying style is aimed at microcontrollers without an
//! allocator. Kythira's target (x86_64 Linux, `std` available) has no such
//! constraint, so this crate uses ordinary `Box`-owned opaque handles whose
//! typestate transitions are all held as regular (non-thread-local) struct
//! fields, so a handle can safely be driven from any thread.
//!
//! Credentials are pre-shared out of band (Kythira's `edhoc_params::
//! identity_credential` / `peer_credential`), not fetched via
//! credential_check_or_fetch()/TOFU, so id_cred_r / id_cred_i / EAD items
//! from parse_message_2 / parse_message_3 are intentionally discarded: the
//! caller already knows which credential it expects from the peer.

use lakers::{
    Credential, CredentialTransfer, EDHOCError, EDHOCMethod, EDHOCSuite, EdhocInitiator,
    EdhocInitiatorProcessedM2, EdhocInitiatorProcessingM2, EdhocInitiatorWaitM2,
    EdhocInitiatorWaitM4, EdhocMessageBuffer, EdhocResponder, EdhocResponderDone,
    EdhocResponderProcessedM1, EdhocResponderProcessingM3, EdhocResponderWaitM3,
};
use lakers_crypto::{default_crypto, Crypto as CryptoImpl};

use std::os::raw::c_int;
use std::slice;

// ── status codes ───────────────────────────────────────────────────────────

pub const LAKERS_OK: c_int = 0;
pub const LAKERS_ERR_NULL_ARG: c_int = -1;
pub const LAKERS_ERR_INVALID_STATE: c_int = -2;
pub const LAKERS_ERR_BUFFER_TOO_SMALL: c_int = -3;
pub const LAKERS_ERR_BAD_CREDENTIAL: c_int = -4;
pub const LAKERS_ERR_BAD_KEY_LENGTH: c_int = -5;
// EDHOC protocol errors are reported as -(100 + n), n starting at 0, in the
// EDHOCError enum's declaration order (shared/src/lib.rs).
const EDHOC_ERR_BASE: c_int = -100;

fn edhoc_error_code(e: EDHOCError) -> c_int {
    let n = match e {
        EDHOCError::UnexpectedCredential => 0,
        EDHOCError::MissingIdentity => 1,
        EDHOCError::IdentityAlreadySet => 2,
        EDHOCError::MacVerificationFailed => 3,
        EDHOCError::UnsupportedMethod => 4,
        EDHOCError::UnsupportedCipherSuite => 5,
        EDHOCError::ParsingError => 6,
        EDHOCError::EncodingError => 7,
        EDHOCError::CredentialTooLongError => 8,
        EDHOCError::EadLabelTooLongError => 9,
        EDHOCError::EadTooLongError => 10,
        EDHOCError::EADUnprocessable => 11,
        EDHOCError::AccessDenied => 12,
        _ => 99,
    };
    EDHOC_ERR_BASE - n
}

fn parse_credential(bytes: &[u8]) -> Result<Credential, c_int> {
    Credential::parse_ccs(bytes).map_err(|_| LAKERS_ERR_BAD_CREDENTIAL)
}

fn parse_p256_key(bytes: &[u8]) -> Result<[u8; 32], c_int> {
    bytes.try_into().map_err(|_| LAKERS_ERR_BAD_KEY_LENGTH)
}

unsafe fn out_slice<'a>(ptr: *mut u8, cap: usize) -> Option<&'a mut [u8]> {
    if ptr.is_null() {
        None
    } else {
        Some(slice::from_raw_parts_mut(ptr, cap))
    }
}

unsafe fn in_slice<'a>(ptr: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        Some(&[])
    } else if ptr.is_null() {
        None
    } else {
        Some(slice::from_raw_parts(ptr, len))
    }
}

/// Writes `msg`'s bytes into `(out, out_cap)`, and `msg`'s length into
/// `*out_len`, regardless of whether it fits (so the caller can retry with a
/// larger buffer). Returns LAKERS_OK, or LAKERS_ERR_BUFFER_TOO_SMALL if it
/// didn't fit (in which case nothing was written to `out`).
unsafe fn write_message(
    msg: &EdhocMessageBuffer,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> c_int {
    if out_len.is_null() {
        return LAKERS_ERR_NULL_ARG;
    }
    let bytes = msg.as_slice();
    *out_len = bytes.len();
    if bytes.len() > out_cap {
        return LAKERS_ERR_BUFFER_TOO_SMALL;
    }
    match out_slice(out, out_cap) {
        Some(dst) => {
            dst[..bytes.len()].copy_from_slice(bytes);
            LAKERS_OK
        }
        None => LAKERS_ERR_NULL_ARG,
    }
}

// ── Initiator (coap_client role) ───────────────────────────────────────────

enum InitiatorStage {
    Start(EdhocInitiator<CryptoImpl>),
    WaitM2(EdhocInitiatorWaitM2<CryptoImpl>),
    ProcessingM2(EdhocInitiatorProcessingM2<CryptoImpl>),
    ProcessedM2(EdhocInitiatorProcessedM2<CryptoImpl>),
    WaitM4(EdhocInitiatorWaitM4<CryptoImpl>),
    // Note: EdhocInitiatorWaitM4::completed_without_message_4() actually
    // returns EdhocResponderDone<Crypto>, not EdhocInitiatorDone<Crypto> —
    // this is upstream lakers' own return type (lib.rs: both roles' Done
    // structs have identical {state, crypto} fields and edhoc_exporter
    // behavior; process_message_4(), which this crate doesn't use, is the
    // only path that produces a real EdhocInitiatorDone). Storing the type
    // upstream actually hands back avoids an unsound transmute between the
    // two structurally-identical-but-distinct types.
    Done(EdhocResponderDone<CryptoImpl>),
    Invalid,
}

pub struct LakersInitiator {
    stage: InitiatorStage,
    cred_i: Option<Credential>,
    priv_key: Option<[u8; 32]>,
}

#[no_mangle]
pub extern "C" fn lakers_initiator_new() -> *mut LakersInitiator {
    let initiator = EdhocInitiator::new(default_crypto(), EDHOCMethod::StatStat,
                                        EDHOCSuite::CipherSuite2);
    Box::into_raw(Box::new(LakersInitiator {
        stage: InitiatorStage::Start(initiator),
        cred_i: None,
        priv_key: None,
    }))
}

/// # Safety
/// `handle` must be a live pointer from lakers_initiator_new(), not
/// previously passed to lakers_initiator_free(). `id_priv_key` must point to
/// 32 bytes (or be null, which is rejected). `cred` must point to `cred_len`
/// bytes of a CBOR CCS credential (Credential::parse_ccs's input format).
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_set_identity(
    handle: *mut LakersInitiator,
    id_priv_key: *const u8,
    cred: *const u8,
    cred_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    if id_priv_key.is_null() {
        return LAKERS_ERR_NULL_ARG;
    }
    let Some(cred_bytes) = in_slice(cred, cred_len) else { return LAKERS_ERR_NULL_ARG };
    let priv_key = match parse_p256_key(slice::from_raw_parts(id_priv_key, 32)) {
        Ok(k) => k,
        Err(e) => return e,
    };
    let credential = match parse_credential(cred_bytes) {
        Ok(c) => c,
        Err(e) => return e,
    };
    // Actually applying this identity happens in verify_message_2 (via
    // set_identity on the ProcessingM2 typestate) — stash it here so
    // lakers_initiator_verify_message_2 has it, matching lakers' own
    // "expose own identity only after validating cred_r" ordering.
    handle.cred_i = Some(credential);
    handle.priv_key = Some(priv_key);
    LAKERS_OK
}

/// # Safety
/// `handle` must be live. `out_msg1`/`out_len` behave as documented on
/// write_message(). c_i and ead_1 are not exposed by this API (always
/// generated/omitted).
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_prepare_message_1(
    handle: *mut LakersInitiator,
    out_msg1: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let stage = std::mem::replace(&mut handle.stage, InitiatorStage::Invalid);
    let InitiatorStage::Start(initiator) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match initiator.prepare_message_1(None, &None) {
        Ok((next, msg1)) => {
            let rc = write_message(&msg1, out_msg1, out_cap, out_len);
            handle.stage = InitiatorStage::WaitM2(next);
            rc
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// `handle` must be live; `message_2` must point to `message_2_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_parse_message_2(
    handle: *mut LakersInitiator,
    message_2: *const u8,
    message_2_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let Some(msg_bytes) = in_slice(message_2, message_2_len) else { return LAKERS_ERR_NULL_ARG };
    let msg2 = match EdhocMessageBuffer::new_from_slice(msg_bytes) {
        Ok(m) => m,
        Err(_) => return LAKERS_ERR_BAD_CREDENTIAL,
    };
    let stage = std::mem::replace(&mut handle.stage, InitiatorStage::Invalid);
    let InitiatorStage::WaitM2(initiator) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match initiator.parse_message_2(&msg2) {
        Ok((next, _c_r, _id_cred_r, _ead_2)) => {
            handle.stage = InitiatorStage::ProcessingM2(next);
            LAKERS_OK
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// `handle` must be live, have completed parse_message_2, and have had
/// lakers_initiator_set_identity() called on it already. `peer_cred` must
/// point to `peer_cred_len` bytes of the peer's expected CCS credential
/// (verified byte-for-byte, no TOFU).
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_verify_message_2(
    handle: *mut LakersInitiator,
    peer_cred: *const u8,
    peer_cred_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let Some(peer_bytes) = in_slice(peer_cred, peer_cred_len) else { return LAKERS_ERR_NULL_ARG };
    let valid_cred_r = match parse_credential(peer_bytes) {
        Ok(c) => c,
        Err(e) => return e,
    };
    let Some(priv_key) = handle.priv_key.take() else { return LAKERS_ERR_INVALID_STATE };
    let Some(cred_i) = handle.cred_i.take() else { return LAKERS_ERR_INVALID_STATE };
    let stage = std::mem::replace(&mut handle.stage, InitiatorStage::Invalid);
    let InitiatorStage::ProcessingM2(mut initiator) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    if initiator.set_identity(priv_key, cred_i).is_err() {
        return LAKERS_ERR_INVALID_STATE;
    }
    match initiator.verify_message_2(valid_cred_r) {
        Ok(next) => {
            handle.stage = InitiatorStage::ProcessedM2(next);
            LAKERS_OK
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// Same buffer-writing contract as prepare_message_1.
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_prepare_message_3(
    handle: *mut LakersInitiator,
    out_msg3: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let stage = std::mem::replace(&mut handle.stage, InitiatorStage::Invalid);
    let InitiatorStage::ProcessedM2(initiator) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match initiator.prepare_message_3(CredentialTransfer::ByReference, &None) {
        Ok((next, msg3, _prk_out)) => {
            let rc = write_message(&msg3, out_msg3, out_cap, out_len);
            handle.stage = InitiatorStage::WaitM4(next);
            rc
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// Completes the exchange without message_4 (RFC 9528 permits either side
/// to skip it once message_3 has been sent/verified); the only completion
/// mode this crate implements.
///
/// # Safety
/// `handle` must be live and have completed prepare_message_3.
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_complete(handle: *mut LakersInitiator) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let stage = std::mem::replace(&mut handle.stage, InitiatorStage::Invalid);
    let InitiatorStage::WaitM4(initiator) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match initiator.completed_without_message_4() {
        Ok(done) => {
            handle.stage = InitiatorStage::Done(done);
            LAKERS_OK
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// `handle` must be live and Done (lakers_initiator_complete() returned
/// LAKERS_OK). `out` must point to at least `out_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_exporter(
    handle: *mut LakersInitiator,
    label: u8,
    out: *mut u8,
    out_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let InitiatorStage::Done(ref mut done) = handle.stage else { return LAKERS_ERR_INVALID_STATE };
    let Some(dst) = out_slice(out, out_len) else { return LAKERS_ERR_NULL_ARG };
    let exported = done.edhoc_exporter(label, &[], out_len);
    dst.copy_from_slice(&exported[..out_len]);
    LAKERS_OK
}

/// # Safety
/// `handle` must either be null or a live pointer previously returned by
/// lakers_initiator_new(), not already freed.
#[no_mangle]
pub unsafe extern "C" fn lakers_initiator_free(handle: *mut LakersInitiator) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

// ── Responder (coap_server role) ───────────────────────────────────────────

enum ResponderStage {
    Start(EdhocResponder<CryptoImpl>),
    ProcessedM1(EdhocResponderProcessedM1<CryptoImpl>),
    WaitM3(EdhocResponderWaitM3<CryptoImpl>),
    ProcessingM3(EdhocResponderProcessingM3<CryptoImpl>),
    Done(EdhocResponderDone<CryptoImpl>),
    Invalid,
}

pub struct LakersResponder {
    stage: ResponderStage,
}

/// # Safety
/// `id_priv_key` must point to 32 bytes; `cred` to `cred_len` bytes of this
/// responder's own CBOR CCS credential. Returns null on a bad credential or
/// key length.
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_new(
    id_priv_key: *const u8,
    cred: *const u8,
    cred_len: usize,
) -> *mut LakersResponder {
    if id_priv_key.is_null() {
        return std::ptr::null_mut();
    }
    let Some(cred_bytes) = in_slice(cred, cred_len) else { return std::ptr::null_mut() };
    let Ok(priv_key) = parse_p256_key(slice::from_raw_parts(id_priv_key, 32)) else {
        return std::ptr::null_mut();
    };
    let Ok(cred_r) = parse_credential(cred_bytes) else { return std::ptr::null_mut() };
    let responder = EdhocResponder::new(default_crypto(), EDHOCMethod::StatStat, priv_key, cred_r);
    Box::into_raw(Box::new(LakersResponder { stage: ResponderStage::Start(responder) }))
}

/// # Safety
/// `handle` must be live and freshly created (not yet advanced).
/// `message_1` must point to `message_1_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_process_message_1(
    handle: *mut LakersResponder,
    message_1: *const u8,
    message_1_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let Some(msg_bytes) = in_slice(message_1, message_1_len) else { return LAKERS_ERR_NULL_ARG };
    let msg1 = match EdhocMessageBuffer::new_from_slice(msg_bytes) {
        Ok(m) => m,
        Err(_) => return LAKERS_ERR_BAD_CREDENTIAL,
    };
    let stage = std::mem::replace(&mut handle.stage, ResponderStage::Invalid);
    let ResponderStage::Start(responder) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match responder.process_message_1(&msg1) {
        Ok((next, _c_i, _ead_1)) => {
            handle.stage = ResponderStage::ProcessedM1(next);
            LAKERS_OK
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// Same buffer-writing contract as lakers_initiator_prepare_message_1.
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_prepare_message_2(
    handle: *mut LakersResponder,
    out_msg2: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let stage = std::mem::replace(&mut handle.stage, ResponderStage::Invalid);
    let ResponderStage::ProcessedM1(responder) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match responder.prepare_message_2(CredentialTransfer::ByReference, None, &None) {
        Ok((next, msg2)) => {
            let rc = write_message(&msg2, out_msg2, out_cap, out_len);
            handle.stage = ResponderStage::WaitM3(next);
            rc
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// `message_3` must point to `message_3_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_parse_message_3(
    handle: *mut LakersResponder,
    message_3: *const u8,
    message_3_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let Some(msg_bytes) = in_slice(message_3, message_3_len) else { return LAKERS_ERR_NULL_ARG };
    let msg3 = match EdhocMessageBuffer::new_from_slice(msg_bytes) {
        Ok(m) => m,
        Err(_) => return LAKERS_ERR_BAD_CREDENTIAL,
    };
    let stage = std::mem::replace(&mut handle.stage, ResponderStage::Invalid);
    let ResponderStage::WaitM3(responder) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match responder.parse_message_3(&msg3) {
        Ok((next, _id_cred_i, _ead_3)) => {
            handle.stage = ResponderStage::ProcessingM3(next);
            LAKERS_OK
        }
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// `peer_cred` must point to `peer_cred_len` bytes of the initiator's
/// expected CCS credential (verified byte-for-byte, no TOFU). On success,
/// the responder is immediately Done (this crate never sends message_4).
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_verify_message_3(
    handle: *mut LakersResponder,
    peer_cred: *const u8,
    peer_cred_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let Some(peer_bytes) = in_slice(peer_cred, peer_cred_len) else { return LAKERS_ERR_NULL_ARG };
    let valid_cred_i = match parse_credential(peer_bytes) {
        Ok(c) => c,
        Err(e) => return e,
    };
    let stage = std::mem::replace(&mut handle.stage, ResponderStage::Invalid);
    let ResponderStage::ProcessingM3(responder) = stage else {
        handle.stage = stage;
        return LAKERS_ERR_INVALID_STATE;
    };
    match responder.verify_message_3(valid_cred_i) {
        Ok((next, _prk_out)) => match next.completed_without_message_4() {
            Ok(done) => {
                handle.stage = ResponderStage::Done(done);
                LAKERS_OK
            }
            Err(e) => edhoc_error_code(e),
        },
        Err(e) => edhoc_error_code(e),
    }
}

/// # Safety
/// Same contract as lakers_initiator_exporter.
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_exporter(
    handle: *mut LakersResponder,
    label: u8,
    out: *mut u8,
    out_len: usize,
) -> c_int {
    let Some(handle) = handle.as_mut() else { return LAKERS_ERR_NULL_ARG };
    let ResponderStage::Done(ref mut done) = handle.stage else { return LAKERS_ERR_INVALID_STATE };
    let Some(dst) = out_slice(out, out_len) else { return LAKERS_ERR_NULL_ARG };
    let exported = done.edhoc_exporter(label, &[], out_len);
    dst.copy_from_slice(&exported[..out_len]);
    LAKERS_OK
}

/// # Safety
/// `handle` must either be null or a live pointer previously returned by
/// lakers_responder_new(), not already freed.
#[no_mangle]
pub unsafe extern "C" fn lakers_responder_free(handle: *mut LakersResponder) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}
