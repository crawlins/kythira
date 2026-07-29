#pragma once

// clang++/aarch64 workaround for libstdc++'s <ext/opt_random.h>: see the
// CMakeLists.txt comment above the `-include` of this file for the full
// history. Summary: <ext/random>'s aarch64 NEON fast path uses GCC's
// `__builtin_shuffle`, which clang does not implement, so clang cannot
// compile that header at all under `#if __ARM_NEON`. Undefining
// __ARM_NEON for the whole translation unit (the first fix attempted)
// avoided the compile error but silently changed the computed layout of
// unrelated types gated on `#ifdef __ARM_NEON` elsewhere in folly/proxygen
// (e.g. proxygen::HTTPMessage: 608 bytes under that workaround vs. 576
// bytes under both GCC and a plain clang build) -- a real, measured ABI
// mismatch against the GCC-compiled vcpkg proxygen/folly static libraries,
// and the actual root cause of a proxygen HTTPHeaders segfault that only
// reproduced on clang+aarch64. Root-caused via a `sizeof(HTTPMessage)`
// probe compiled with both compilers on real ARM64 hardware.
//
// Fix: force-include this header first (see CMakeLists.txt), which
// pre-processes <ext/random> with __ARM_NEON locally undefined only for
// that one system header. <ext/random>'s own include guard then makes
// every later #include <ext/random> (transitively, via folly/Random.h) a
// no-op, so __ARM_NEON stays defined -- matching GCC -- for every other
// header in the translation unit. Confirmed on real ARM64 hardware to
// restore sizeof(HTTPMessage) to 576 (matching GCC) while still compiling
// cleanly under clang.
#if defined(__clang__) && (defined(__aarch64__) || defined(__arm64__))
#pragma push_macro("__ARM_NEON")
#undef __ARM_NEON
#include <ext/random>
#pragma pop_macro("__ARM_NEON")
#endif
