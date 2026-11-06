// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file redis_golden_bytes.hpp
/// @brief Golden-file comparison for the Redis gateway's wire, log-entry and
///        snapshot bytes (.kiro/specs/redis-gateway-module/ task 1).
///
/// The files under `services/redis/tests/golden/redis/` were generated from `main` before
/// the gateway moved into `services/redis/`, and are checked in. A test
/// renders what it produces as text (`hex_block` for bytes, `escape` for RESP
/// replies) and hands it to `check_golden`, which compares it with the file
/// line by line. A golden file that changes in a pull request is a behaviour
/// change, and that pull request has to say so.
///
/// To regenerate after a deliberate change, run the test with
/// `KYTHIRA_UPDATE_GOLDENS=1`: it rewrites the files instead of comparing,
/// and the diff then shows exactly what changed.
///
/// `KYTHIRA_REDIS_GOLDEN_DIR` is set by the test's CMake target to the
/// directory holding the files.

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef KYTHIRA_REDIS_GOLDEN_DIR
#error "KYTHIRA_REDIS_GOLDEN_DIR must name the golden-file directory"
#endif

namespace kythira::testing::golden {

[[nodiscard]] inline auto golden_dir() -> std::filesystem::path {
    return std::filesystem::path{KYTHIRA_REDIS_GOLDEN_DIR};
}

[[nodiscard]] inline auto updating() -> bool {
    const char* v = std::getenv("KYTHIRA_UPDATE_GOLDENS");
    return v != nullptr && *v != '\0' && std::string_view{v} != "0";
}

/// Lowercase hex, 32 bytes (64 digits) per line, each line prefixed with
/// `indent`. An empty input renders as `<indent>(empty)`.
[[nodiscard]] inline auto hex_block(const std::vector<std::byte>& bytes,
                                    std::string_view indent = "  ") -> std::string {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    if (bytes.empty()) {
        out.append(indent).append("(empty)\n");
        return out;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i % 32 == 0) {
            out.append(indent);
        }
        const auto b = std::to_integer<unsigned>(bytes[i]);
        out.push_back(digits[b >> 4U]);
        out.push_back(digits[b & 0xFU]);
        if (i % 32 == 31 || i + 1 == bytes.size()) {
            out.push_back('\n');
        }
    }
    return out;
}

/// Printable ASCII as itself; `\` as `\\`; CR, LF and TAB as `\r`, `\n`,
/// `\t`; every other byte as `\xHH`. One RESP reply renders on one line.
[[nodiscard]] inline auto escape(std::string_view s) -> std::string {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c >= 0x20 && c < 0x7f) {
            out.push_back(ch);
        } else {
            out += "\\x";
            out.push_back(digits[c >> 4U]);
            out.push_back(digits[c & 0xFU]);
        }
    }
    return out;
}

namespace detail {
[[nodiscard]] inline auto split_lines(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
}
}  // namespace detail

/// Compare `body`, under the copyright notice, with `golden_dir() / name`,
/// or rewrite the file when `KYTHIRA_UPDATE_GOLDENS` is set. On a mismatch
/// the first differing line is reported with its number, and the whole
/// actual text is written to the working directory as `<name>.actual`.
inline auto check_golden(const std::string& name, const std::string& body) -> void {
    // The files are checked in, so they carry the project's notice; it is
    // part of what is compared, so a regenerated file keeps it.
    const std::string actual =
        "# Copyright (c) 2026 Clark Rawlins\n# SPDX-License-Identifier: Apache-2.0\n\n" + body;
    const auto path = golden_dir() / name;
    if (updating()) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << actual;
        BOOST_REQUIRE_MESSAGE(out.good(), "could not write " << path);
        BOOST_TEST_MESSAGE("golden: rewrote " << path);
        return;
    }
    std::ifstream in(path, std::ios::binary);
    BOOST_REQUIRE_MESSAGE(in.good(), "golden file " << path
                                                    << " is missing; run with "
                                                       "KYTHIRA_UPDATE_GOLDENS=1 to create it");
    std::stringstream buf;
    buf << in.rdbuf();
    const auto expected = buf.str();
    if (expected == actual) {
        return;
    }
    const auto want = detail::split_lines(expected);
    const auto got = detail::split_lines(actual);
    std::size_t line = 0;
    while (line < want.size() && line < got.size() && want[line] == got[line]) {
        ++line;
    }
    const auto dump = std::filesystem::path{name}.filename().string() + ".actual";
    std::ofstream(dump, std::ios::binary | std::ios::trunc) << actual;
    BOOST_ERROR("golden " << name << " differs at line " << (line + 1) << "\n  expected: "
                          << (line < want.size() ? want[line] : std::string{"<end of file>"})
                          << "\n  actual:   "
                          << (line < got.size() ? got[line] : std::string{"<end of output>"})
                          << "\n  full output written to " << dump
                          << "; a deliberate change is regenerated with KYTHIRA_UPDATE_GOLDENS=1");
}

}  // namespace kythira::testing::golden
