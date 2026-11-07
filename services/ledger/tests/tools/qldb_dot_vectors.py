#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
"""Print the dot/proof vectors ledger_merkle_test.cpp checks against.

The verification functions below are the ones in the AWS QLDB
verification tutorial (Python, verifier.py: compare_hash_values,
join_hash_pairwise, calculate_root_hash_from_internal_hashes), copied as
published apart from comments, so the C++ `dot` and `fold_proof` are tested
against AWS's own procedure rather than against a second reading of it.

The tree roots at the end pin the accumulator's shape (RFC 6962 with
join_hash_pairwise as the node function), written here independently of
merkle.hpp, so a change to the shape cannot pass unnoticed.

Run: python3 services/ledger/tests/tools/qldb_dot_vectors.py
"""
from array import array
from functools import reduce
from hashlib import sha256

HASH_LENGTH = 32


def compare_hash_values(hash1, hash2):
    assert len(hash1) == HASH_LENGTH
    assert len(hash2) == HASH_LENGTH
    hash_array1 = array('b', hash1)
    hash_array2 = array('b', hash2)
    for i in range(len(hash_array1) - 1, -1, -1):
        difference = hash_array1[i] - hash_array2[i]
        if difference != 0:
            return difference
    return 0


def join_hash_pairwise(hash1, hash2):
    if len(hash1) == 0:
        return hash2
    if len(hash2) == 0:
        return hash1
    concatenated = hash1 + hash2 if compare_hash_values(hash1, hash2) < 0 else hash2 + hash1
    new_hash_lib = sha256()
    new_hash_lib.update(concatenated)
    return new_hash_lib.digest()


def calculate_root_hash_from_internal_hashes(internal_hashes, leaf_hash):
    return reduce(join_hash_pairwise, internal_hashes, leaf_hash)


def h(label):
    return sha256(label.encode()).digest()


def with_tail(prefix_byte, tail):
    """32 bytes: prefix_byte repeated, then `tail` as the last bytes."""
    return bytes([prefix_byte] * (HASH_LENGTH - len(tail))) + bytes(tail)


def tree_root(leaves):
    n = len(leaves)
    if n == 1:
        return leaves[0]
    k = 1
    while k * 2 < n:
        k *= 2
    return join_hash_pairwise(tree_root(leaves[:k]), tree_root(leaves[k:]))


def main():
    pairs = [
        ("decided by the last byte", with_tail(0xff, [0x01]), with_tail(0x00, [0x02])),
        ("signed: 0x80 sorts before 0x7f", with_tail(0x00, [0x7f]), with_tail(0x00, [0x80])),
        ("tie on the last byte, decided earlier", with_tail(0x00, [0x05, 0x09]),
         with_tail(0x00, [0x04, 0x09])),
        ("equal hashes", with_tail(0x11, []), with_tail(0x11, [])),
        ("sha256 labels", h("a"), h("b")),
    ]
    for name, a, b in pairs:
        print(f"// {name}")
        print(f'{{"{a.hex()}", "{b.hex()}", "{join_hash_pairwise(a, b).hex()}"}},')

    leaf = h("revision")
    proof = [h("proof-0"), h("proof-1"), h("proof-2"), h("proof-3")]
    print("// tutorial fold: leaf sha256('revision'), proof sha256('proof-0'..'proof-3')")
    print(calculate_root_hash_from_internal_hashes(proof, leaf).hex())

    print("// tree roots over leaves sha256(str(i)), n = 1..9")
    leaves = [h(str(i)) for i in range(9)]
    for n in range(1, 10):
        print(f'"{tree_root(leaves[:n]).hex()}",')


if __name__ == "__main__":
    main()
