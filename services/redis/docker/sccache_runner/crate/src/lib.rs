// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

//! Enough code that a compile is not free, so a cache hit is visibly a hit.

/// Folded into the checksum so that changing it changes the crate's cache
/// key. sccache 0.10 caches only the library compile (a `bin` crate type is
/// `CannotCache`), so this is the one knob a scenario has to make a build
/// miss on purpose; `run.sh` rewrites the value from `KYTHIRA_SALT`.
pub const SALT: u64 = 0; // KYTHIRA_SALT

/// Sum of the first `n` values of a simple linear congruential sequence.
pub fn checksum(n: u64) -> u64 {
    let mut x: u64 = 0x9E37_79B9_7F4A_7C15;
    let mut acc: u64 = SALT;
    for _ in 0..n {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        acc = acc.wrapping_add(x >> 33);
    }
    acc
}

#[cfg(test)]
mod tests {
    #[test]
    fn checksum_is_deterministic() {
        assert_eq!(super::checksum(10), super::checksum(10));
    }
}
