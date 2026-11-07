# Ion hash test vectors

`ion_hash_tests.ion` is the test suite of the Amazon Ion hash specification,
copied unchanged from
[amazon-ion/ion-hash-test](https://github.com/amazon-ion/ion-hash-test)
at commit `c2e3655e58da679f3dd9a533754d70c79b80fda6` (2020-03-05). It is
Amazon's work under the Apache License 2.0; `LICENSE` and `NOTICE` here are
that repository's. Its header comment describes the format.

`ion_hash_test.cpp` runs every case that has an `identity` or `md5`
expectation against `kythira::ledger::ion_hasher`. To update, copy the file
from a newer commit and change the commit above.
