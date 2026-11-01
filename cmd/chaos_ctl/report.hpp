// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The reporter (.kiro/specs/cairn-chaos-testing/ Requirement 6, task 12):
// one finding from one schedule log, written out three ways.
//
//   tests/docker_chaos/findings/<id>.jsonl     the replay file, which
//                                              `chaos_ctl replay --log` runs
//   tests/docker_chaos/findings/<id>_test.cpp  a draft ChaosCluster test that
//                                              encodes the schedule and fails
//                                              if the invariant breaks again
//   doc/chaos-findings.md                      an entry: date, invariant,
//                                              schedule, log excerpt, Kythira
//                                              commit and Cairn version
//
// Nothing is committed (Requirement 6.2): the files land in the working
// tree for Clark to review.
//
// The finding is the log's first call that recorded a violation. The
// schedule written out is the log cut three ways, each of which leaves the
// cluster's history as the run saw it:
//
//   - calls after the finding's call are dropped: they cannot have caused it;
//   - read-only views (cluster.status, workload.history) are dropped, as
//     replay already skips them;
//   - calls the tool server refused before acting are dropped. A refused call
//     changed nothing, and replaying it is worse than useless: on a replay
//     the cluster's state can differ enough for the call to be accepted and
//     do something the run never did. "Refused" is matched on the error
//     text of the tool_error throws in tools.hpp that come before any side
//     effect; a call that failed part way ("command failed ...") is kept.
//
// The finding's own call is always kept, refused or not: its post-step Raft
// safety check is where the run saw the violation. This is not minimization
// (`chaos_ctl minimize` is, and judges every cut by replaying it); a report
// made from a minimized log says so.

#include "replay.hpp"
#include "schedule_log.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chaos_ctl::report {

/// The Cairn engine scripts/cairn/build-engine.sh pins.
inline constexpr std::string_view k_cairn_version = "v0.2.1 (e0ef2f85)";

/// A call the tool server refused before it acted: an unsupported tool, or
/// one of tools.hpp's argument and precondition errors, all of which are
/// thrown before the tool sends anything to the cluster.
[[nodiscard]] inline auto refused(const call_record& c) -> bool {
    if (c.status == "unsupported") {
        return true;
    }
    if (c.status != "error") {
        return false;
    }
    static const std::vector<std::regex> k_refusals = [] {
        std::vector<std::regex> out;
        for (const auto* p : {
                 R"(^argument '[^']*' must be )",
                 R"(^no node -?[0-9]+ in this run$)",
                 R"(^no node reports itself leader$)",
                 R"(^failpoint '.*' is not in docker/cairn/failpoints\.allowlist$)",
                 R"(^probability must be in )",
                 R"(^mode must be always, random, once or off$)",
                 R"(^groups must list at least two groups)",
                 R"(^each group must be a non-empty list)",
                 R"(^node ids must be integers$)",
                 R"(^node -?[0-9]+ is in two groups$)",
                 R"(^every node must be in exactly one group$)",
                 R"(^node -?[0-9]+ has no address \(is it running\?\)$)",
                 R"(^delay_ms must be in )",
                 R"(^a workload is already running; stop it first$)",
                 R"(^this run already has a workload history; one workload per run$)",
                 R"(^concurrency must be in )",
                 R"(^tail must be in )",
                 R"(^checks must be a list)",
                 R"(^k must be in )",
                 R"(^unknown check ')",
             }) {
            out.emplace_back(p);
        }
        return out;
    }();
    return std::ranges::any_of(k_refusals,
                               [&c](const std::regex& r) { return std::regex_search(c.error, r); });
}

/// The log's first call that recorded a violation, or nullptr.
[[nodiscard]] inline auto first_violation(const std::vector<call_record>& calls)
    -> const call_record* {
    const auto it =
        std::ranges::find_if(calls, [](const auto& c) { return !c.violations.empty(); });
    return it == calls.end() ? nullptr : &*it;
}

struct cut_options {
    bool keep_refused{false};
    bool keep_after_violation{false};
};

struct cut_result {
    std::vector<call_record> calls;
    std::size_t read_only{0};
    std::size_t refused{0};
    std::size_t after_violation{0};
};

/// The schedule a finding is written out with (see the file comment).
[[nodiscard]] inline auto cut(const std::vector<call_record>& all, std::int64_t violation_seq,
                              const cut_options& o = {}) -> cut_result {
    cut_result out;
    for (const auto& c : all) {
        if (c.seq == violation_seq) {
            out.calls.push_back(c);
            continue;
        }
        if (c.seq > violation_seq && !o.keep_after_violation) {
            ++out.after_violation;
        } else if (!replay::replayable(c)) {
            ++out.read_only;
        } else if (!o.keep_refused && refused(c)) {
            ++out.refused;
        } else {
            out.calls.push_back(c);
        }
    }
    return out;
}

/// What `chaos_ctl replay` printed, as far as a report needs it.
struct replay_summary {
    int attempts{0};
    int reproduced{0};
};

/// The replay result in `text`: chaos_ctl replay's output, alone or inside
/// a log of the run that produced it (the first line that is a JSON object
/// with "attempts" and "reproduced").
[[nodiscard]] inline auto parse_replay_result(const std::string& text)
    -> std::optional<replay_summary> {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() != '{') {
            continue;
        }
        boost::system::error_code ec;
        auto v = boost::json::parse(line, ec);
        if (ec || !v.is_object()) {
            continue;
        }
        const auto& o = v.as_object();
        const auto* a = o.if_contains("attempts");
        const auto* r = o.if_contains("reproduced");
        if (a != nullptr && r != nullptr && a->is_number() && r->is_number()) {
            return replay_summary{static_cast<int>(a->to_number<std::int64_t>()),
                                  static_cast<int>(r->to_number<std::int64_t>())};
        }
    }
    return std::nullopt;
}

/// Attempts a draft test makes by default: enough for a 90 % chance of
/// seeing the violation again at the measured reproduction rate, capped at
/// 10; 3 when no rate was measured.
[[nodiscard]] inline auto default_attempts(const std::optional<replay_summary>& r) -> int {
    if (!r || r->attempts <= 0) {
        return 3;
    }
    const double p = static_cast<double>(r->reproduced) / r->attempts;
    if (p >= 1.0) {
        return 1;
    }
    if (p <= 0.0) {
        return 10;
    }
    const auto n = static_cast<int>(std::ceil(std::log(0.1) / std::log(1.0 - p)));
    return std::clamp(n, 1, 10);
}

struct options {
    std::string source_log;
    std::string id;
    std::string image;
    std::string kythira_commit;
    std::string cairn_version{k_cairn_version};
    std::string note;
    bool minimized{false};
    std::optional<replay_summary> replay;
    cut_options cut;
};

struct finding {
    std::string id;
    std::string source_log;
    std::size_t total_calls{0};
    const call_record* violation{nullptr};
    std::set<std::string> invariants;
    std::size_t later_violating_calls{0};
    cut_result schedule;
    std::string image;
    std::string image_id;
    std::string kythira_commit;
    std::string cairn_version;
    std::uint64_t seed{0};
    std::string started_wall;
    std::string violation_wall;
    double minutes_to_violation{0};
    bool minimized{false};
    std::optional<replay_summary> replay;
    int attempts{3};
    std::string note;

    [[nodiscard]] auto span_ms() const -> std::int64_t {
        if (schedule.calls.empty()) {
            return 0;
        }
        return schedule.calls.back().started_mono_ms - schedule.calls.front().started_mono_ms;
    }
};

[[nodiscard]] inline auto string_at(const boost::json::object& o, std::string_view k)
    -> std::string {
    const auto* v = o.if_contains(k);
    return v != nullptr && v->is_string() ? std::string(v->as_string()) : std::string{};
}

/// `election_safety` -> `election-safety`, for ids.
[[nodiscard]] inline auto slug(std::string s) -> std::string {
    std::ranges::replace(s, '_', '-');
    return s;
}

/// A file name, target name and C++ identifier fragment: [a-z0-9-] only.
[[nodiscard]] inline auto valid_id(std::string_view id) -> bool {
    return !id.empty() && id.size() <= 80 && std::ranges::all_of(id, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    });
}

/// Builds the finding. `calls` must outlive it (it points at the violating
/// call). Throws std::invalid_argument if the log has no violation.
[[nodiscard]] inline auto build(const std::vector<call_record>& calls, const options& o)
    -> finding {
    const auto* v = first_violation(calls);
    if (v == nullptr) {
        throw std::invalid_argument(o.source_log + " records no violation; nothing to report");
    }
    finding f;
    f.source_log = o.source_log;
    f.total_calls = calls.size();
    f.violation = v;
    f.invariants = v->violated();
    f.later_violating_calls = static_cast<std::size_t>(std::ranges::count_if(
        calls, [v](const auto& c) { return c.seq > v->seq && !c.violations.empty(); }));
    f.schedule = cut(calls, v->seq, o.cut);
    const auto& first = calls.front().started_line;
    f.image_id = string_at(v->started_line, "image_id");
    f.kythira_commit =
        o.kythira_commit.empty() ? string_at(v->started_line, "kythira_commit") : o.kythira_commit;
    if (f.kythira_commit.empty()) {
        f.kythira_commit = "unknown";
    }
    if (const auto* s = v->started_line.if_contains("seed"); s != nullptr && s->is_number()) {
        f.seed = s->to_number<std::uint64_t>();
    }
    f.image = o.image;
    f.cairn_version = o.cairn_version;
    f.started_wall = string_at(first, "wall");
    f.violation_wall =
        v->done_line ? string_at(*v->done_line, "wall") : string_at(v->started_line, "wall");
    f.minutes_to_violation =
        static_cast<double>(v->started_mono_ms - calls.front().started_mono_ms) / 60000.0;
    f.minimized = o.minimized;
    f.replay = o.replay;
    f.attempts = default_attempts(o.replay);
    f.note = o.note;
    if (o.id.empty()) {
        const auto date =
            f.violation_wall.size() >= 10 ? f.violation_wall.substr(0, 10) : std::string("undated");
        f.id = date + "-" + slug(*f.invariants.begin()) + "-" + std::to_string(v->seq);
    } else {
        f.id = o.id;
    }
    if (!valid_id(f.id)) {
        throw std::invalid_argument("finding id '" + f.id + "' must be [a-z0-9-], at most 80");
    }
    return f;
}

// ── Rendering ────────────────────────────────────────────────────────────────

[[nodiscard]] inline auto fixed(double v, int digits) -> std::string {
    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%.*f", digits, v);
    return buf.data();
}

/// A C++ literal holding `s`: raw when that is safe, escaped otherwise.
[[nodiscard]] inline auto cpp_literal(const std::string& s) -> std::string {
    if (s.find(")json\"") == std::string::npos &&
        std::ranges::none_of(s, [](char ch) { return ch == '\n' || ch == '\r'; })) {
        return "R\"json(" + s + ")json\"";
    }
    std::string out = "\"";
    for (const char ch : s) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += ch;
        }
    }
    return out + "\"";
}

/// The violation records of the finding's call, joined for a one-line
/// summary: `2 nodes won term 43`.
[[nodiscard]] inline auto details(const finding& f) -> std::string {
    std::string out;
    for (const auto& v : f.violation->violations) {
        const auto d = string_at(v.as_object(), "detail");
        if (d.empty()) {
            continue;
        }
        out += (out.empty() ? "" : "; ") + d;
    }
    return out;
}

[[nodiscard]] inline auto invariants_text(const finding& f, std::string_view sep = ", ")
    -> std::string {
    std::string out;
    for (const auto& i : f.invariants) {
        out += (out.empty() ? "" : std::string(sep)) + i;
    }
    return out;
}

[[nodiscard]] inline auto short_id(const std::string& image_id) -> std::string {
    auto s = image_id;
    if (s.starts_with("sha256:")) {
        s = s.substr(7);
    }
    return s.substr(0, 12);
}

[[nodiscard]] inline auto test_ident(const finding& f) -> std::string {
    auto s = "finding_" + f.id;
    std::ranges::replace(s, '-', '_');
    return s;
}

[[nodiscard]] inline auto timeout_s(const finding& f) -> std::int64_t {
    // Each attempt: the schedule, plus starting the cluster, healing and the
    // checkers.
    return f.attempts * (f.span_ms() / 1000 + 300);
}

/// The draft test, as its own translation unit.
[[nodiscard]] inline auto render_test(const finding& f, std::string_view year) -> std::string {
    std::ostringstream o;
    const auto image = f.image.empty() ? std::string("the image ") + short_id(f.image_id) : f.image;
    o << "// Copyright (c) " << year << " Clark Rawlins\n"
      << "// SPDX-License-Identifier: Apache-2.0\n\n"
      << "// DRAFT written by `chaos_ctl report` (cairn-chaos-testing Requirement 6).\n"
      << "// Review it before committing; nothing was committed for you.\n"
      << "//\n"
      << "// Finding " << f.id << " (doc/chaos-findings.md):\n"
      << "//   " << invariants_text(f) << ", \"" << details(f) << "\"\n"
      << "//   at seq " << f.violation->seq << " of " << f.source_log << ",\n"
      << "//   " << fixed(f.minutes_to_violation, 1) << " min into a run on " << image << "\n"
      << "//   (image id " << short_id(f.image_id) << ", Kythira commit " << f.kythira_commit
      << ").\n"
      << "//\n"
      << "// The test replays the schedule below at its recorded offsets on a fresh\n"
      << "// three-node cluster, heals, runs every checker, and fails if the\n"
      << "// invariant is violated again in any of " << f.attempts << " attempt(s). It must\n"
      << "// fail on the image the finding was made on and pass once the bug is fixed\n"
      << "// (Requirement 6.3):\n"
      << "//\n"
      << "//   KYTHIRA_CHAOS_NODE_IMAGE=" << (f.image.empty() ? "<that image>" : f.image)
      << "  fails\n"
      << "//   KYTHIRA_CHAOS_NODE_IMAGE=kythira-chaos-node:dev  passes after the fix\n"
      << "//\n"
      << "// The docker-chaos-findings target runs every finding test;\n"
      << "// KYTHIRA_CHAOS_FINDING_REPEAT overrides the attempt count.\n\n"
      << "#define BOOST_TEST_MODULE " << test_ident(f) << "\n"
      << "#include <boost/test/unit_test.hpp>\n\n"
      << "#include \"chaos_finding.hpp\"\n\n"
      << "namespace {\n\n"
      << "// Offset from the first call (ms), seq in the run's log, tool, arguments.\n"
      << "// " << f.schedule.calls.size() << " of the log's " << f.total_calls
      << " calls; doc/chaos-findings.md says which were cut and why.\n"
      << "constexpr chaos_finding::step k_schedule[] = {\n";
    const auto base = f.schedule.calls.empty() ? 0 : f.schedule.calls.front().started_mono_ms;
    for (const auto& c : f.schedule.calls) {
        o << "    {" << (c.started_mono_ms - base) << ", " << c.seq << ", \"" << c.tool << "\", "
          << cpp_literal(boost::json::serialize(c.args)) << "},\n";
    }
    o << "};\n\n"
      << "const chaos_finding::finding k_finding{\n"
      << "    .id = \"" << f.id << "\",\n"
      << "    .invariants = {";
    bool first = true;
    for (const auto& i : f.invariants) {
        o << (first ? "" : ", ") << '"' << i << '"';
        first = false;
    }
    o << "},\n"
      << "    .attempts = " << f.attempts << ",\n"
      << "    .found_on_image = \"" << f.image << "\",\n"
      << "};\n\n"
      << "}  // namespace\n\n"
      << "BOOST_AUTO_TEST_CASE(" << test_ident(f) << ", *boost::unit_test::timeout(" << timeout_s(f)
      << ")) {\n"
      << "    chaos_finding::check(k_finding, k_schedule);\n"
      << "}\n";
    return o.str();
}

/// The replay file: the cut schedule's lines, which `chaos_ctl replay --log`
/// reads like any schedule log.
[[nodiscard]] inline auto render_replay_file(const finding& f) -> std::string {
    std::string out;
    for (const auto& line : lines_of(f.schedule.calls)) {
        out += boost::json::serialize(line);
        out += '\n';
    }
    return out;
}

/// The heading doc/chaos-findings.md opens with.
[[nodiscard]] inline auto doc_header() -> std::string {
    return R"(# Chaos findings

Findings from Cairn runs against Kythira (`.kiro/specs/cairn-chaos-testing/`,
Requirement 6). Each entry is written by `chaos_ctl report` from a run's
schedule log, together with a replay file and a draft `ChaosCluster` test in
`tests/docker_chaos/findings/`. Nothing is committed automatically: each
entry and draft is reviewed before it lands, and a draft test must fail on
the commit the finding was made against and pass after the fix before it
merges (Requirement 6.3).

A finding is a checker violation that reproduced from its schedule with no
model involved (Requirement 0.3). The replay rate is how many replays of the
run's log saw the same invariant broken again.
)";
}

[[nodiscard]] inline auto schedule_line(const call_record& c, std::int64_t base) -> std::string {
    auto args = boost::json::serialize(c.args);
    if (args.size() > 110) {
        args = args.substr(0, 107) + "...";
    }
    std::string tool = c.tool;
    tool.resize(std::max<std::size_t>(tool.size(), 20), ' ');
    std::string seq = "#" + std::to_string(c.seq);
    seq.resize(std::max<std::size_t>(seq.size(), 6), ' ');
    std::string t = "+" + fixed(static_cast<double>(c.started_mono_ms - base) / 1000.0, 1) + "s";
    t.insert(0, t.size() < 9 ? 9 - t.size() : 0, ' ');
    return t + "  " + seq + " " + tool + " " + args +
           (c.status == "ok" ? "" : "  [" + c.status + "]");
}

/// The doc/chaos-findings.md entry.
[[nodiscard]] inline auto render_entry(const finding& f, std::string_view replay_path,
                                       std::string_view test_path) -> std::string {
    std::ostringstream o;
    const auto date = f.violation_wall.size() >= 10 ? f.violation_wall.substr(0, 10) : "undated";
    const auto time = f.violation_wall.size() >= 19 ? f.violation_wall.substr(11, 8) : "";
    o << "\n## " << f.id << ": " << invariants_text(f) << "\n\n"
      << "| | |\n|---|---|\n"
      << "| Date | " << date << (time.empty() ? "" : " " + time + "Z") << ", "
      << fixed(f.minutes_to_violation, 1) << " min into the run |\n"
      << "| Invariant | `" << invariants_text(f, "`, `") << "`: " << details(f) << " |\n"
      << "| Kythira commit | "
      << (f.kythira_commit == "unknown" ? f.kythira_commit : "`" + f.kythira_commit + "`") << " |\n"
      << "| Image | " << (f.image.empty() ? std::string("") : "`" + f.image + "`, ") << "id `"
      << short_id(f.image_id) << "` |\n"
      << "| Cairn version | " << f.cairn_version << " |\n"
      << "| Run seed | " << f.seed << " |\n"
      << "| Replay | ";
    if (f.replay) {
        o << f.replay->reproduced << " of " << f.replay->attempts
          << " replays of the run's log reproduced it";
    } else {
        o << "not measured: run `chaos_ctl replay --repeat N` before this lands";
    }
    o << " |\n"
      << "| Replay file | `" << replay_path << "` |\n"
      << "| Draft test | `" << test_path << "`, " << f.attempts << " attempt(s) |\n"
      << "| Status | draft, awaiting review |\n";
    if (!f.note.empty()) {
        o << "\n" << f.note << "\n";
    }
    const auto& calls = f.schedule.calls;
    o << "\n**Schedule.** The run's log holds " << f.total_calls << " calls; the finding is seq "
      << f.violation->seq << ". The replay file keeps " << calls.size() << " of them, spanning "
      << fixed(static_cast<double>(f.span_ms()) / 60000.0, 1)
      << " min. Cut: " << f.schedule.after_violation << " after the finding, "
      << f.schedule.read_only << " read-only views, and " << f.schedule.refused
      << " refused by the tool server before acting. "
      << (f.minimized ? "The log was minimized by `chaos_ctl minimize` first."
                      : "Not minimized: `chaos_ctl minimize` has not judged these cuts, and "
                        "the replay rate above is for the uncut log.");
    if (f.later_violating_calls > 0) {
        o << " " << f.later_violating_calls
          << " later calls recorded violations too; they follow from this one or from the "
             "run's own continuation and are not separate findings.";
    }
    o << "\n\n```text\n";
    const auto base = calls.empty() ? 0 : calls.front().started_mono_ms;
    const std::size_t head = 12;
    const std::size_t tail = 12;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        if (calls.size() > head + tail + 1 && i == head) {
            o << "          ... " << calls.size() - head - tail << " more calls ...\n";
            i = calls.size() - tail - 1;
            continue;
        }
        o << schedule_line(calls[i], base) << '\n';
    }
    o << "```\n\n**Log excerpt.** The finding's outcome line, without its result:\n\n```json\n";
    auto excerpt = f.violation->done_line.value_or(f.violation->started_line);
    excerpt.erase("result");
    o << boost::json::serialize(excerpt) << "\n```\n";
    return o.str();
}

}  // namespace chaos_ctl::report
