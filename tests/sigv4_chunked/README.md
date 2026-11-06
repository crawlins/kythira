# Streaming SigV4 vectors

`vectors.json` holds requests signed by botocore at a fixed time with fake
credentials, for `tests/sigv4_chunk_decoder_test.cpp`
(`.kiro/specs/iam-compatible-auth/` task 7). `gen_vectors.py` writes it, using
the botocore version pinned in `requirements.txt`. Don't edit the JSON by hand.
To regenerate:

```sh
python3 -m venv /tmp/sigv4-venv
/tmp/sigv4-venv/bin/pip install -r tests/sigv4_chunked/requirements.txt
cd tests/sigv4_chunked && /tmp/sigv4-venv/bin/python gen_vectors.py > vectors.json
```

Rerunning with the pinned botocore reproduces the file byte for byte.

| Mode | Seed signature | Body |
|---|---|---|
| `streaming_signed`, `streaming_signed_trailer` | botocore `S3SigV4Auth` | chunk and trailer signatures computed by the script from botocore's signing key, as AWS's S3 pages describe (botocore never signs chunks) |
| `streaming_unsigned_trailer` | botocore `S3SigV4Auth`, which picks the STREAMING value itself | botocore's own `AwsChunkedWrapper` output, as boto3 sends flexible checksums |
| `presigned` | botocore `S3SigV4QueryAuth` | none |

The credentials are fake and named so. Never use AWS's documentation example
keys here, because secret scanners flag them.
