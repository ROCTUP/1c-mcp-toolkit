#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "bsl/check_code.h"
#include "strict/check_query.h"
#include "strict/field_policy.h"
#include "test_framework.h"

// Data-driven corpus runner. A corpus is a directory of *.case files plus an optional policy.json:
//   // kind: query | code
//   // expect: safe | confirm | incomplete
//   // reason: <code>        (optional, repeatable; each must be among the reasons)
//   // risk: <code>          (optional, repeatable; code cases)
//   // label: <path> = <lbl> (optional, repeatable; code cases: result_labels[path] must equal lbl)
//   // class: <n>            (optional, research.md attack class; informational)
//   <text>                   (everything after the header lines)
// A header line is "//", a known key and a colon; the first other line starts the text. A malformed case file is a
// failure, never a skip. A case expecting safe that is not safe is a usability failure; a case expecting
// confirm / incomplete that gets safe is a SECURITY failure; the two are reported apart.

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct Case {
    std::string file;
    std::string kind;
    std::string expect;
    std::vector<std::string> reasons;
    std::vector<std::string> risks;
    std::vector<std::pair<std::string, std::string>> labels;
    std::string text;
};

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return std::string();
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

bool ReadFile(const fs::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream os;
    os << in.rdbuf();
    *out = os.str();
    if (out->compare(0, 3, "\xEF\xBB\xBF") == 0) out->erase(0, 3);
    out->erase(std::remove(out->begin(), out->end(), '\r'), out->end());
    return true;
}

// Returns an empty string on success, otherwise the reason the file is malformed.
std::string ParseCase(const std::string& raw, Case* c) {
    static const char* const kKeys[] = {"kind", "expect", "reason", "risk", "class", "label"};
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        const std::string line = raw.substr(pos, eol - pos);
        if (line.compare(0, 2, "//") != 0) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) break;
        const std::string key = Trim(line.substr(2, colon - 2));
        bool known = false;
        for (const char* k : kKeys) known = known || key == k;
        if (!known) break;
        const std::string value = Trim(line.substr(colon + 1));
        if (value.empty()) return "empty value of '" + key + "'";
        if (key == "kind") {
            if (!c->kind.empty()) return "duplicate 'kind'";
            if (value != "query" && value != "code") return "kind must be query or code";
            c->kind = value;
        } else if (key == "expect") {
            if (!c->expect.empty()) return "duplicate 'expect'";
            if (value != "safe" && value != "confirm" && value != "incomplete") {
                return "expect must be safe, confirm or incomplete";
            }
            c->expect = value;
        } else if (key == "reason") {
            c->reasons.push_back(value);
        } else if (key == "risk") {
            c->risks.push_back(value);
        } else if (key == "label") {
            const size_t eq = value.rfind('=');
            if (eq == std::string::npos) return "label must be: path = label";
            const std::string path = Trim(value.substr(0, eq));
            const std::string label = Trim(value.substr(eq + 1));
            if (path.empty() || label.empty()) return "label must be: path = label";
            c->labels.emplace_back(path, label);
        }
        pos = eol < raw.size() ? eol + 1 : eol;
    }
    if (c->kind.empty()) return "missing 'kind'";
    if (c->expect.empty()) return "missing 'expect'";
    if (c->kind == "query" && (!c->risks.empty() || !c->labels.empty())) return "'risk' / 'label' apply to code cases";
    c->text = raw.substr(pos);
    return std::string();
}

bool Contains(const Json& arr, const std::string& code) {
    if (!arr.is_array()) return false;
    for (const Json& item : arr) {
        if (item.is_object() && item.contains("code") && item["code"].get<std::string>() == code) return true;
    }
    return false;
}

// Empty string = the case holds, otherwise a one-line description of the mismatch.
std::string Mismatch(const Case& c, const std::string& policy_json, bool* security) {
    Json j;
    try {
        j = Json::parse(c.kind == "query" ? lineage::strict::CheckQueryImpl(c.text, policy_json)
                                          : lineage::bsl::CheckCodeImpl(c.text, policy_json));
    } catch (const std::exception& e) {
        return std::string("checker output is not JSON: ") + e.what();
    }
    std::ostringstream out;
    const std::string verdict = j.value("verdict", std::string("<none>"));
    if (verdict != c.expect) {
        out << "verdict " << verdict << ", expected " << c.expect << "; ";
        if (verdict == "safe") *security = true;
    }
    for (const std::string& code : c.reasons) {
        if (!Contains(j.value("reasons", Json::array()), code)) out << "missing reason " << code << "; ";
    }
    for (const std::string& code : c.risks) {
        if (!Contains(j.value("risks", Json::array()), code)) out << "missing risk " << code << "; ";
    }
    for (const auto& want : c.labels) {
        const Json labels = j.value("result_labels", Json::object());
        const auto it = labels.find(want.first);
        const std::string got = it == labels.end() ? std::string("<none>") : it->get<std::string>();
        if (got != want.second) out << "label " << want.first << " is " << got << ", expected " << want.second << "; ";
    }
    if (!out.str().empty()) {
        out << "[got reasons:";
        for (const Json& r : j.value("reasons", Json::array())) out << " " << r.value("code", std::string("?"));
        out << "]";
    }
    return out.str();
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string s;
    for (const std::string& l : lines) s += "\n  " + l;
    return s;
}

void RunCorpusDir(const fs::path& dir) {
    std::vector<std::string> malformed, security, usability, other;
    std::string policy_json;
    if (!ReadFile(dir / "policy.json", &policy_json) &&
        !ReadFile(fs::path(QLA_CORPUS_DIR) / "policy.json", &policy_json)) {
        qla_test::Fail("no policy.json in " + dir.string() + " or in the repo corpus", __FILE__, __LINE__);
    }
    const auto parsed = lineage::strict::FieldPolicy::FromJson(policy_json);
    if (!parsed.ok) qla_test::Fail("policy.json is malformed: " + parsed.error, __FILE__, __LINE__);

    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".case") continue;
        // legit_pending holds legitimate shapes the checker still stops on; it runs only on request.
        if (fs::relative(entry.path(), dir).begin()->string() == "legit_pending") continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) qla_test::Fail("no cases in " + dir.string(), __FILE__, __LINE__);

    for (const fs::path& file : files) {
        const std::string name = fs::relative(file, dir).generic_string();
        std::string raw;
        Case c;
        c.file = name;
        if (!ReadFile(file, &raw)) {
            malformed.push_back(name + ": unreadable");
            continue;
        }
        const std::string err = ParseCase(raw, &c);
        if (!err.empty()) {
            malformed.push_back(name + ": " + err);
            continue;
        }
        bool is_security = false;
        const std::string diff = Mismatch(c, policy_json, &is_security);
        if (diff.empty()) continue;
        if (is_security) security.push_back(name + ": " + diff);
        else if (c.expect == "safe") usability.push_back(name + ": " + diff);
        else other.push_back(name + ": " + diff);
    }
    std::cout << "corpus " << dir.string() << ": " << files.size() << " cases\n";
    if (malformed.empty() && security.empty() && usability.empty() && other.empty()) return;

    std::ostringstream msg;
    if (!security.empty()) msg << "\nSECURITY (expected a stop, got safe):" << JoinLines(security);
    if (!malformed.empty()) msg << "\nMALFORMED case files:" << JoinLines(malformed);
    if (!usability.empty()) msg << "\nUSABILITY (expected safe, got a stop):" << JoinLines(usability);
    if (!other.empty()) msg << "\nMISMATCH:" << JoinLines(other);
    qla_test::Fail(msg.str(), __FILE__, __LINE__);
}

}  // namespace

TEST(Corpus, RepoCorpus) { RunCorpusDir(fs::path(QLA_CORPUS_DIR)); }

// Legitimate cases the checker does not auto-pass yet (each is a usability gap with a reason code in its header).
// Not part of the default run; --pending or QLA_PENDING_CORPUS=1 runs it and lists every case still stopped.
TEST(Corpus, PendingLegitIfRequested) {
    bool requested = qla_test::ArgValue("--pending") == "1";
    if (const char* e = std::getenv("QLA_PENDING_CORPUS")) requested = requested || std::string(e) == "1";
    if (!requested) return;
    RunCorpusDir(fs::path(QLA_CORPUS_DIR) / "legit_pending");
}

// An alternative corpus (recipe-derived cases kept outside the repository): --corpus <dir> or QLA_LOCAL_CORPUS.
// A missing directory is a skip; a present one that is empty or malformed is a failure.
TEST(Corpus, LocalCorpusIfGiven) {
    std::string dir = qla_test::ArgValue("--corpus");
    if (dir.empty()) {
        if (const char* e = std::getenv("QLA_LOCAL_CORPUS")) dir = e;
    }
    if (dir.empty()) return;
    if (!fs::is_directory(fs::path(dir))) {
        std::cout << "corpus " << dir << ": not present, skipped\n";
        return;
    }
    RunCorpusDir(fs::path(dir));
}
