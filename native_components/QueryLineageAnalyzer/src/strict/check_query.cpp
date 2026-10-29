#include "strict/check_query.h"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bsl/bsl_taint.h"
#include "strict/analyzer.h"
#include "strict/ast_dump.h"
#include "strict/canonical_printer.h"
#include "strict/field_policy.h"
#include "strict/origin.h"
#include "strict/strict_parser.h"

namespace lineage::strict {

namespace {

using Json = nlohmann::json;

const char* const kInternalFallback = "{\"api\":1,\"verdict\":\"confirm\",\"reasons\":[{\"code\":\"Q_INTERNAL\"}]}";

Json MakeReason(const std::string& code, const Pos* pos) {
    Json reason = Json::object();
    reason["code"] = code;
    if (pos) {
        reason["line"] = pos->line;
        reason["col"] = pos->col;
        reason["offset"] = pos->offset;
    }
    return reason;
}

std::string Dump(const Json& json) {
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json Skeleton() {
    Json out = Json::object();
    out["api"] = 1;
    out["verdict"] = "confirm";
    out["reasons"] = Json::array();
    out["canonical"] = "";
    out["columns"] = Json::array();
    out["missing_tables"] = Json::array();
    return out;
}

std::string Reject(const std::string& code, const Pos* pos) {
    Json out = Skeleton();
    out["reasons"].push_back(MakeReason(code, pos));
    return Dump(out);
}

// Names the platform will give the columns of the last statement; the analyzer's own names are the fallback.
std::vector<std::string> FinalColumnNames(const Batch& batch, const AnalysisResult& analysis) {
    std::vector<std::vector<std::string>> per_statement;
    if (StatementResultNames(batch, per_statement) && !per_statement.empty() &&
        per_statement.back().size() == analysis.columns.size()) {
        return per_statement.back();
    }
    std::vector<std::string> names;
    for (const ColumnLabel& column : analysis.columns) names.push_back(column.name);
    return names;
}

// The result of a batch is the last statement's; any other select without ПОМЕСТИТЬ (or a final УНИЧТОЖИТЬ)
// would return a result the labels do not describe.
bool HasUnlabelledResult(const Batch& batch) {
    const size_t count = batch.statements.size();
    for (size_t i = 0; i < count; ++i) {
        const Statement& st = batch.statements[i];
        const bool is_last = i + 1 == count;
        if (st.kind == Statement::K::Drop) {
            if (is_last) return true;
        } else if (!is_last && (!st.select || st.select->into.empty())) {
            return true;
        }
    }
    return false;
}

QueryReason MakeQueryReason(const std::string& code, const Pos* pos) {
    QueryReason r;
    r.code = code;
    if (pos) {
        r.has_pos = true;
        r.pos = *pos;
    }
    return r;
}

// The parameter that holds the value of the token at `index`, and its origin.
std::string TokenParamName(int index) { return "МСПТокен" + std::to_string(index + 1); }

ParamOrigin TokenParamOrigin(const bsl::TokenBinding& binding) {
    return binding.origin == Origin::ProtectedDirect ? ParamOrigin::TokenProtected : ParamOrigin::TokenUnknown;
}

// Gives every parameter МСПТокен<index+1> of the table the origin of its token (see check_query.h). False if
// "params" names one of them with another origin: the host binds the token's value there, and a different label
// would relabel that value (a client label would return a protected value unmasked).
bool AgreeTokenParams(const bsl::TokenTable& tokens, FieldPolicy& policy) {
    for (const auto& kv : tokens) {
        if (!policy.AgreeParam(TokenParamName(kv.second.index), TokenParamOrigin(kv.second))) return false;
    }
    return true;
}

// Turns every string literal whose whole value is an issued token into the parameter МСПТокен<index+1> (see
// check_query.h). A token inside a longer literal, or a token-format literal the table does not list, stays a
// literal.
class TokenLiteralBinder {
public:
    explicit TokenLiteralBinder(const bsl::TokenTable& tokens) : tokens_(tokens) {}

    void Run(Batch& batch) {
        for (Statement& st : batch.statements) VisitSelect(st.select);
    }

private:
    void VisitExpr(const ExprP& e) {
        if (!e) return;
        if (e->kind == Expr::K::StringLit) {
            bsl::TokenBinding binding;
            if (bsl::ClassifyTokenLiteral(e->text, &tokens_, &binding) == bsl::TokenUse::Bound) {
                e->kind = Expr::K::Param;
                e->text = TokenParamName(binding.index);
            }
            return;
        }
        for (const ExprP& arg : e->args) VisitExpr(arg);
        VisitSelect(e->subquery);
    }

    void VisitList(const std::vector<ExprP>& list) {
        for (const ExprP& e : list) VisitExpr(e);
    }

    void VisitSource(const Source& src) {
        VisitList(src.vt_params);
        VisitSelect(src.subquery);
    }

    void VisitSelect(const std::shared_ptr<Select>& s) {
        if (!s) return;
        for (const SelectItem& item : s->items) VisitExpr(item.expr);
        for (const Source& src : s->from) VisitSource(src);
        for (const std::vector<Join>& joins : s->joins) {
            for (const Join& j : joins) {
                VisitSource(j.source);
                VisitExpr(j.on);
            }
        }
        VisitExpr(s->where);
        VisitList(s->group_by);
        VisitExpr(s->having);
        VisitList(s->index_by);
        for (const Select::UnionPart& part : s->unions) VisitSelect(part.select);
        for (const OrderItem& o : s->order_by) VisitExpr(o.expr);
        VisitList(s->totals_aggregates);
        for (const TotalsItem& t : s->totals_by) {
            VisitExpr(t.expr);
            VisitList(t.periods_args);
        }
    }

    const bsl::TokenTable& tokens_;
};

// `tokens` is null on the path of query texts in client code: there a token literal stays a literal. Otherwise
// `policy` already gives the token parameters their origins (AgreeTokenParams).
QueryCheckResult Analyze(const std::string& query_utf8, const FieldPolicy& policy,
                         const bsl::TokenTable* tokens = nullptr) {
    QueryCheckResult out;
    ParseResult parsed = ParseBatchStrict(query_utf8);
    if (!parsed.ok) {
        out.reasons.push_back(
            MakeQueryReason(parsed.error.code.empty() ? "Q_PARSE_ERROR" : parsed.error.code, &parsed.error.pos));
        return out;
    }

    if (tokens && !tokens->empty()) TokenLiteralBinder(*tokens).Run(parsed.batch);

    const AnalysisResult analysis = AnalyzeQuery(parsed.batch, policy);
    for (const Reason& reason : analysis.reasons) out.reasons.push_back(MakeQueryReason(reason.code, &reason.pos));
    out.missing_tables = analysis.missing_tables;

    bool safe = analysis.verdict == Verdict::Safe && analysis.reasons.empty() && analysis.missing_tables.empty() &&
                analysis.resolution_issues.empty();

    if (HasUnlabelledResult(parsed.batch)) {
        safe = false;
        out.reasons.push_back(MakeQueryReason("Q_BATCH_RESULT", nullptr));
    }

    const PrintResult printed = PrintCanonicalChecked(parsed.batch);
    if (printed.ok) {
        out.canonical = printed.text;
    } else {
        safe = false;
        out.reasons.push_back(
            MakeQueryReason(printed.error.code.empty() ? "Q_PRINT_FAILED" : printed.error.code, &printed.error.pos));
    }

    out.parsed = true;
    const std::vector<std::string> names = FinalColumnNames(parsed.batch, analysis);
    for (size_t i = 0; i < analysis.columns.size(); ++i) {
        ColumnLabel column = analysis.columns[i];
        if (i < names.size()) column.name = names[i];
        out.columns.push_back(std::move(column));
    }

    if (safe) {
        out.verdict = "safe";
    } else if (!analysis.missing_tables.empty()) {
        out.verdict = "incomplete";
    } else {
        out.verdict = "confirm";
    }
    return out;
}

std::string ToJson(const QueryCheckResult& r) {
    Json out = Skeleton();
    for (const QueryReason& reason : r.reasons) {
        out["reasons"].push_back(MakeReason(reason.code, reason.has_pos ? &reason.pos : nullptr));
    }
    for (const std::string& table : r.missing_tables) out["missing_tables"].push_back(table);
    out["canonical"] = r.canonical;
    for (const ColumnLabel& label : r.columns) {
        Json column = Json::object();
        column["name"] = label.name;
        column["origin"] = OriginName(label.origin);
        column["direct"] = label.origin == Origin::ProtectedDirect;
        out["columns"].push_back(column);
    }
    out["verdict"] = r.verdict;
    return Dump(out);
}

std::string Check(const std::string& query_utf8, const std::string& policy_json) {
    FieldPolicy::Parsed policy = FieldPolicy::FromJson(policy_json);
    if (!policy.ok) return Reject("Q_POLICY_INVALID", nullptr);
    // A malformed token table, or a token parameter that "params" labels otherwise than its token, is a malformed
    // policy, as in CheckCode.
    bsl::TokenTable tokens;
    if (!bsl::ParseTokenTable(policy_json, tokens) || !AgreeTokenParams(tokens, policy.policy)) {
        return Reject("Q_POLICY_INVALID", nullptr);
    }
    return ToJson(Analyze(query_utf8, policy.policy, &tokens));
}

}  // namespace

QueryCheckResult CheckQueryWithPolicy(const std::string& query_utf8, const FieldPolicy& policy) {
    try {
        return Analyze(query_utf8, policy);
    } catch (...) {
        QueryCheckResult r;
        r.reasons.push_back(MakeQueryReason("Q_INTERNAL", nullptr));
        return r;
    }
}

std::string CheckQueryImpl(const std::string& query_utf8, const std::string& policy_json) {
    try {
        return Check(query_utf8, policy_json);
    } catch (...) {
        return kInternalFallback;
    }
}

}  // namespace lineage::strict
