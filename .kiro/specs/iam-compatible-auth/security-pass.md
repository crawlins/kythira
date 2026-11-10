# Security pass over the IAM listeners

**Written**: November 9, 2026, for task 25 (Requirement 14.5): the
categories of `vulnerability-audit-2026-10-02.md` applied to every listener
and outbound fetch this spec added. Read against the code on the IAM PR's
branch; nothing ran against real cloud resources.

The surfaces:

1. `iam_node`'s HTTPS listener (IAM and STS Query APIs, admin API,
   `POST /v1/iam/credentials`), on `include/raft/beast_https_server.hpp`.
2. `iam_node`'s Raft RPC, and the embedded replica's RPC inside
   `redis_gateway_node`.
3. The JWKS and discovery fetches for external web identity issuers
   (`iam_oidc_jwks.hpp`).
4. The Redis gateway's `AUTH` with IAM tokens (`iam_redis_verifier.hpp`).
5. The CA's OIDC issuer endpoints were reviewed in their own task
   (task 3) and are not repeated here.

## By audit category

| Audit category (finding) | Result |
|---|---|
| Unbounded input buffers and pre-auth parsing cost (H3, M8, M20) | Sound. The HTTPS server caps headers at 16 KiB and bodies at 1 MiB, with 10 s header and handshake timeouts and 60 s idle and I/O timeouts. Query bodies are parsed once. JWKS responses are capped at 64 KiB. |
| Accept loops that die, one unbounded thread per connection (H4) | Sound. The accept loop logs and continues on any error but shutdown, sleeping 10 ms on resource errors; connections are thread-per-connection but gated at 256 (`max_connections`). |
| Plaintext by default, skipped verification (M3, M7) | Sound. HTTPS needs `--tls-cert`; plaintext needs `--insecure-plaintext`, which warns on every start. Raft RPC needs mutual TLS unless `--allow-plaintext-rpc` or a loopback-only bind, and warns either way. The replica in `redis_gateway_node` has no plaintext option at all: its three TLS variables are required. |
| Missing hostname check (M15) | Sound. JWKS and discovery fetches verify the server certificate and its host name (cpp-httplib's default once verification is on), never follow redirects, and pin the connection to the address that passed the public-address check. |
| Revocation (M13, M14) | Unchanged. The IAM RPC uses the same mutual TLS as the CA cluster, with the same lack of CRL checking (M14 is still open for core). A replica is removed by deregistering its id, which its certificate's reserved peer name is bound to. |
| Oracles (M4) | Sound. Unseal happens at start-up from a file; no request reveals whether a passphrase guess is right. DecodeAuthorizationMessage needs `sts:DecodeAuthorizationMessage` and only opens messages sealed for the caller's own account. |
| Pre-auth CPU DoS (M22) | Sound. Authentication is SigV4 (a handful of HMACs); no request triggers a password hash. The KEK is derived once at start-up. Failed signatures spend a per-key budget (100 per minute) and, on the Redis gateway, the caller's per-address budget too. An unknown `kid` refetches a JWKS at most once per 30 s per issuer. |
| Secrets in logs and memory (Requirement 14.2) | Sound. Secrets are sealed before they reach the log, returned once at creation, and cleansed (`OPENSSL_cleanse`) in the admin path, the authorizer, the Query API's key creation and the sealing code. The CLI writes the root key to a 0600 file, never to stdout. |
| Range checks in serializers (M21) | Sound. Session tokens and authorization messages are CBOR sealed with AES-256-GCM and length-checked before they are opened; a token that does not open is `InvalidClientTokenId`. |
| SSRF through a caller-chosen URL | Sound. Only registered providers' issuers are fetched; https only, optional host allowlist, non-public addresses refused, connection pinned to the checked address. |
| Containers as root, published ports (M9, M10, M11) | Test-only. The IAM images are test images (as their Dockerfiles say) and run as root like every other test image; the compose files publish nothing on the host. A production image is out of this spec. |

## Requirement 14.3 (amended)

Requirement 14.3 asks that an unknown access key and a bad signature take
the same code path and comparable time. They do not: an unknown key is
refused (`InvalidClientTokenId`) before any HMAC is computed, while a bad
signature costs the derivation and comparison (`SignatureDoesNotMatch`,
compared with `CRYPTO_memcmp`). This follows AWS, whose two error codes
already say which case applies, so equal timing would hide nothing the
response does not reveal. Access key ids are not secrets in AWS's model.
The per-key failure budget bounds the guessing either way.

Resolved 2026-11-10: Clark chose to keep AWS's behaviour, and Requirement
14.3 now reads "both refused, with AWS's error codes, and compared in
constant time". No code change.
