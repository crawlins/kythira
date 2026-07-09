/*
 * Hand-written C header for lakers-kythira-ffi (vcpkg-overlays/lakers/ffi).
 * Keep in sync with ffi/src/lib.rs by hand — there is no cbindgen build
 * step for this crate (see vcpkg-overlays/lakers/README.md for why).
 */
#ifndef LAKERS_KYTHIRA_H
#define LAKERS_KYTHIRA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by every function below unless noted otherwise. */
#define LAKERS_OK 0
#define LAKERS_ERR_NULL_ARG (-1)
#define LAKERS_ERR_INVALID_STATE (-2)
#define LAKERS_ERR_BUFFER_TOO_SMALL (-3)
#define LAKERS_ERR_BAD_CREDENTIAL (-4)
#define LAKERS_ERR_BAD_KEY_LENGTH (-5)
/* EDHOC protocol errors (peer auth failure, malformed message, etc.) are
 * reported as values <= -100; see edhoc_error_code() in ffi/src/lib.rs for
 * the exact mapping. Callers that don't need to distinguish EDHOC error
 * subtypes can just treat any negative return as failure. */

typedef struct LakersInitiator LakersInitiator;
typedef struct LakersResponder LakersResponder;

/* ── Initiator (coap_client role) ────────────────────────────────────── */

LakersInitiator *lakers_initiator_new(void);

/* id_priv_key: 32-byte raw P-256 private key. cred/cred_len: this device's
 * own CBOR CCS credential. Must be called before verify_message_2(). */
int lakers_initiator_set_identity(LakersInitiator *handle, const uint8_t *id_priv_key,
                                  const uint8_t *cred, size_t cred_len);

/* Writes message_1 into (out_msg1, out_cap) and its true length into
 * *out_len (even when it doesn't fit, so the caller can retry with a
 * larger buffer). */
int lakers_initiator_prepare_message_1(LakersInitiator *handle, uint8_t *out_msg1, size_t out_cap,
                                       size_t *out_len);

int lakers_initiator_parse_message_2(LakersInitiator *handle, const uint8_t *message_2,
                                     size_t message_2_len);

/* peer_cred/peer_cred_len: the peer's expected CBOR CCS credential, checked
 * byte-for-byte (no trust-on-first-use). */
int lakers_initiator_verify_message_2(LakersInitiator *handle, const uint8_t *peer_cred,
                                     size_t peer_cred_len);

int lakers_initiator_prepare_message_3(LakersInitiator *handle, uint8_t *out_msg3, size_t out_cap,
                                       size_t *out_len);

/* Completes the exchange without message_4 (RFC 9528 permits either side to
 * skip it once message_3 has been sent/verified) — the only completion
 * mode this binding implements. */
int lakers_initiator_complete(LakersInitiator *handle);

/* Derives exported key material (label 0 = OSCORE Master Secret, label 1 =
 * OSCORE Master Salt, per RFC 8613/9528 convention) into (out, out_len)
 * bytes. Only valid after lakers_initiator_complete() returns LAKERS_OK. */
int lakers_initiator_exporter(LakersInitiator *handle, uint8_t label, uint8_t *out,
                              size_t out_len);

void lakers_initiator_free(LakersInitiator *handle);

/* ── Responder (coap_server role) ────────────────────────────────────── */

/* id_priv_key: 32-byte raw P-256 private key. cred/cred_len: this device's
 * own CBOR CCS credential. Returns NULL on a bad credential/key. */
LakersResponder *lakers_responder_new(const uint8_t *id_priv_key, const uint8_t *cred,
                                      size_t cred_len);

int lakers_responder_process_message_1(LakersResponder *handle, const uint8_t *message_1,
                                       size_t message_1_len);

int lakers_responder_prepare_message_2(LakersResponder *handle, uint8_t *out_msg2, size_t out_cap,
                                       size_t *out_len);

int lakers_responder_parse_message_3(LakersResponder *handle, const uint8_t *message_3,
                                     size_t message_3_len);

/* peer_cred/peer_cred_len: the initiator's expected CBOR CCS credential.
 * On success the responder is immediately Done (no message_4 is sent). */
int lakers_responder_verify_message_3(LakersResponder *handle, const uint8_t *peer_cred,
                                      size_t peer_cred_len);

int lakers_responder_exporter(LakersResponder *handle, uint8_t label, uint8_t *out,
                              size_t out_len);

void lakers_responder_free(LakersResponder *handle);

#ifdef __cplusplus
}
#endif

#endif /* LAKERS_KYTHIRA_H */
