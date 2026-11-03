# AWS SigV4 signing test suite (vendored)

The `v4/` directory is copied unmodified from
[awslabs/aws-c-auth](https://github.com/awslabs/aws-c-auth),
`tests/aws-signing-test-suite/v4`, at commit
`bdb7cef99d8bd1ad3c189b1400140a7787e47105` (2026-11-02). It is AWS's published SigV4
test suite, released under the Apache License 2.0; `LICENSE` and `NOTICE` are that
repository's own.

This is third-party content, so its files carry no Kythira copyright header
(see `CLAUDE.md`).

Each case directory holds `context.json` (credentials, region, service, timestamp
and whether the path is normalised), the unsigned `request.txt`, and for both the
header and the presigned (query) forms the signed request, canonical request,
string to sign and signature. `tests/sigv4_verifier_test.cpp` feeds every signed
request to `include/raft/sigv4_verifier.hpp` and checks all three intermediate
results byte for byte. `"normalize": false` cases run under the S3 path rule.

To refresh, copy the directory again from a newer commit and update the hash above.
