#pragma once
#include <cstring>
#include <memory>
#include <string>

#include "strict/analyzer.h"
#include "strict/field_policy.h"
#include "bsl/bsl_parser.h"
#include "bsl/bsl_taint.h"
#include "bsl/bsl_types.h"
#include "strict/strict_parser.h"

// Helpers of the strict-checker suites. It also provides CheckCodeForTest, HasCodeReason and HasRisk.

namespace lineage::strict {

// Parses strictly, then analyzes. A parse error becomes a result carrying the Q_PARSE_* reason.
inline AnalysisResult AnalyzeForTest(const FieldPolicy& policy, const std::string& query) {
    const ParseResult parsed = ParseBatchStrict(query);
    if (!parsed.ok) {
        AnalysisResult r;
        r.reasons.push_back(Reason{parsed.error.code, parsed.error.pos});
        return r;
    }
    return AnalyzeQuery(parsed.batch, policy);
}

inline bool HasReason(const AnalysisResult& r, const char* code) {
    for (const Reason& reason : r.reasons) {
        if (reason.code == code) return true;
    }
    return false;
}

inline bool HasIssue(const AnalysisResult& r, const char* code) {
    for (const ResolveIssue& issue : r.resolution_issues) {
        if (issue.code == code) return true;
    }
    return false;
}

// The analysis together with the parsed body its Expr* keys (query_texts, bound_tokens, query_canonical) point into.
struct CodeCheck : bsl::CodeAnalysis {
    std::shared_ptr<const bsl::ParseResult> parsed;
};

// Parses client code and runs the taint pass (types, labels, CheckQuery of every query text with `policy`). A parse
// error becomes a result carrying the parser's reason code (C_EVAL, C_RESERVED, ...).
inline CodeCheck CheckCodeForTest(const FieldPolicy& policy, const std::string& code,
                                  const bsl::TokenTable& tokens = bsl::TokenTable()) {
    auto parsed = std::make_shared<bsl::ParseResult>(bsl::ParseBsl(code));
    CodeCheck r;
    if (!parsed->ok) {
        r.reasons.push_back(bsl::CodeReason{parsed->error.code, parsed->error.pos});
        r.risks = bsl::RefusedCodeRisks(code, parsed->error.code);   // no tree: the markers come from the text
    } else {
        static_cast<bsl::CodeAnalysis&>(r) = bsl::AnalyzeCode(parsed->body, policy, tokens);
    }
    r.parsed = std::move(parsed);
    return r;
}

inline bool HasCodeReason(const bsl::CodeAnalysis& r, const char* code) {
    for (const bsl::CodeReason& reason : r.reasons) {
        if (reason.code == code) return true;
    }
    return false;
}

inline bool HasRisk(const bsl::CodeAnalysis& r, const char* code) {
    for (const bsl::CodeReason& risk : r.risks) {
        if (risk.code == code) return true;
    }
    return false;
}

}  // namespace lineage::strict
