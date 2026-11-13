#include "bsl/check_code.h"

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bsl/bsl_canonical.h"
#include "bsl/bsl_parser.h"
#include "bsl/bsl_taint.h"
#include "bsl/bsl_tokenizer.h"
#include "bsl/bsl_types.h"
#include "strict/check_query.h"
#include "utf_utils.h"
#include "mixed_script.h"
#include "strict/field_policy.h"

namespace lineage::bsl {

namespace {

using Json = nlohmann::json;

const char* const kInternalFallback =
    "{\"api\":1,\"verdict\":\"confirm\",\"reasons\":[{\"code\":\"C_INTERNAL\",\"line\":1,\"col\":1,\"offset\":0}],"
    "\"risks\":[{\"code\":\"R_UNKNOWN_EFFECT\",\"line\":1,\"col\":1}],\"canonical\":\"\",\"result_labels\":{},"
    "\"missing_tables\":[]}";

struct Outcome {
    std::string verdict = "confirm";
    std::vector<CodeReason> reasons;
    std::vector<CodeReason> risks;
    std::string canonical;
    std::vector<ResultPath> labels;
    std::set<std::string> missing_tables;
};

std::string Dump(const Json& json) {
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string ToJson(const Outcome& o) {
    Json out = Json::object();
    out["api"] = 1;
    out["verdict"] = o.verdict;
    out["reasons"] = Json::array();
    for (const CodeReason& r : o.reasons) {
        Json reason = Json::object();
        reason["code"] = r.code;
        reason["line"] = r.pos.line;
        reason["col"] = r.pos.col;
        reason["offset"] = r.pos.offset;
        out["reasons"].push_back(reason);
    }
    out["risks"] = Json::array();
    for (const CodeReason& r : o.risks) {
        Json risk = Json::object();
        risk["code"] = r.code;
        risk["line"] = r.pos.line;
        risk["col"] = r.pos.col;
        out["risks"].push_back(risk);
    }
    out["canonical"] = o.canonical;
    out["result_labels"] = Json::object();
    for (const ResultPath& p : o.labels) out["result_labels"][p.path] = p.label;
    out["missing_tables"] = Json::array();
    for (const std::string& t : o.missing_tables) out["missing_tables"].push_back(t);
    return Dump(out);
}

// Refusal without an analysis: the reasons and the markers from the text only.
Outcome Refuse(const std::string& code, const std::string& reason_code, const Pos& pos) {
    Outcome o;
    o.reasons.push_back(CodeReason{reason_code, pos});
    o.risks = RefusedCodeRisks(code, reason_code);
    return o;
}

// Policy tables the code refers to, for a parse failure on a mixed-script name the policy does not list yet (see
// strict::QueryTableNames): "<Менеджеры>.<Имя>" pairs with the plural mapped to the policy's singular kind, and the
// table names of every string literal read as a query text.
std::vector<std::string> CodeTableNames(const std::vector<Token>& tokens) {
    static const std::pair<const char*, const char*> kPlural[] = {
        {"Справочники", "Справочник"}, {"Документы", "Документ"}, {"Перечисления", "Перечисление"},
        {"ПланыОбмена", "ПланОбмена"}, {"ПланыВидовХарактеристик", "ПланВидовХарактеристик"},
        {"ПланыСчетов", "ПланСчетов"}, {"РегистрыСведений", "РегистрСведений"},
        {"РегистрыНакопления", "РегистрНакопления"}, {"РегистрыБухгалтерии", "РегистрБухгалтерии"},
        {"БизнесПроцессы", "БизнесПроцесс"}, {"Задачи", "Задача"}, {"Константы", "Константа"}};
    std::vector<std::string> out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind == Bt::String) {
            for (const std::string& t : strict::QueryTableNames(tokens[i].value)) out.push_back(t);
            continue;
        }
        if (i + 2 >= tokens.size() || tokens[i].kind != Bt::Identifier || tokens[i + 1].kind != Bt::Dot ||
            tokens[i + 2].kind != Bt::Identifier) {
            continue;
        }
        const std::string key = NormalizeIdentifierKey(tokens[i].text);
        for (const auto& p : kPlural) {
            if (key == NormalizeIdentifierKey(p.first)) out.push_back(std::string(p.second) + "." + tokens[i + 2].text);
        }
    }
    return out;
}

bool HasRefusedMixedName(const std::vector<Token>& tokens) {
    for (const Token& t : tokens) {
        if (t.kind == Bt::Identifier && t.mixed_script && IsRefusedMixedScript(t.text)) return true;
    }
    return false;
}

bool Failed(const CodeAnalysis& a) {
    for (const CodeReason& r : a.reasons) {
        if (r.code == "C_LIMIT" || r.code == "C_INTERNAL" || r.code == "Q_INTERNAL") return true;
    }
    return false;
}

// Every query text of the code has a canonical query text.
bool AllQueriesPrinted(const CodeAnalysis& a) {
    for (const QueryTextUse& use : a.query_texts) {
        if (!a.query_canonical.count(use.text_expr)) return false;
    }
    return true;
}

Outcome Check(const std::string& code, const std::string& policy_json) {
    const strict::FieldPolicy::Parsed policy = strict::FieldPolicy::FromJson(policy_json);
    TokenTable tokens;
    if (!policy.ok || !ParseTokenTable(policy_json, tokens)) return Refuse(code, "C_POLICY_INVALID", Pos{});

    const std::set<std::string> mixed = policy.policy.MixedScriptNames();
    const KnownMixedNamesScope scope(&mixed);   // also covers the query texts checked inside AnalyzeCode

    const ParseResult parsed = ParseBsl(code);   // the Body lives until the end of this function
    if (!parsed.ok) {
        Outcome o = Refuse(code, parsed.error.code.empty() ? "C_PARSE" : parsed.error.code, parsed.error.pos);
        const std::vector<Token> tokens = Tokenize(code);
        if (parsed.error.code == "C_PARSE" && HasRefusedMixedName(tokens)) {
            for (const std::string& t : CodeTableNames(tokens)) {
                if (!policy.policy.HasTable(t)) o.missing_tables.insert(t);
            }
            if (!o.missing_tables.empty()) o.verdict = "incomplete";
        }
        return o;
    }

    const CodeAnalysis a = AnalyzeCode(parsed.body, policy.policy, tokens);
    Outcome o;
    o.reasons = a.reasons;
    for (const CodeReason& r : a.result_reasons) o.reasons.push_back(r);
    o.risks = a.risks;
    MergeRisks(o.risks, ScanRisksInSource(code));
    o.labels = a.result_labels;
    o.missing_tables = a.missing_tables;

    bool clean = o.reasons.empty() && o.risks.empty() && o.missing_tables.empty();
    if (o.missing_tables.empty() && !Failed(a) && AllQueriesPrinted(a)) {
        PrintOptions options;
        options.query_replacements = a.query_canonical;
        options.bound_tokens = a.bound_tokens;
        const PrintResult printed = PrintBslCanonicalChecked(parsed.body, options);
        if (printed.ok) {
            o.canonical = printed.text;
        } else {
            clean = false;
            o.reasons.push_back(CodeReason{printed.error.code.empty() ? "C_PRINT_FAILED" : printed.error.code,
                                           printed.error.pos});
        }
    } else {
        clean = false;
    }
    if (clean && a.verdict == CodeVerdict::Safe) {
        o.verdict = "safe";
    } else if (!o.missing_tables.empty()) {
        o.verdict = "incomplete";
    } else {
        o.verdict = "confirm";
    }
    return o;
}

}  // namespace

std::string CheckCodeImpl(const std::string& code, const std::string& policy_json) {
    try {
        return ToJson(Check(code, policy_json));
    } catch (...) {
        return kInternalFallback;
    }
}

}  // namespace lineage::bsl
