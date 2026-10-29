#pragma once
#include <set>
#include <string>
#include <vector>

#include "strict/analyzer.h"
#include "strict/field_policy.h"

// Strict query check, the entry point behind the CheckQuery component method. Returns a JSON object:
//   {"api":1,"verdict":"safe|confirm|incomplete","reasons":[{"code","line","col","offset"}],
//    "canonical":"...","columns":[{"name","origin","direct"}],"missing_tables":[...]}
// "canonical" is empty unless the query parsed and printed; "incomplete" means the policy lacks tables
// (listed in "missing_tables") and the host should retry with them added. Reasons carry fixed codes and
// positions only; the only client text in the output is the canonical text, column names and table names.
// "incomplete" is never safe: retry with the missing tables, the retry's verdict decides.
// The BSL caller must execute the canonical text with Запрос.Выполнить() (not ВыполнитьПакет): only the last
// statement's result is labelled, so a non-final select without ПОМЕСТИТЬ, or a final УНИЧТОЖИТЬ, is Q_BATCH_RESULT.
// A malformed policy is Q_POLICY_INVALID / confirm; so is a "params" name outside the alphabet of a parameter after
// '&' (IsIdentifierName), since the host could bind it to another parameter after trimming or folding. Never throws: an internal failure is Q_INTERNAL / confirm.
// Token literals: a string literal whose whole value is a token of the policy's "tokens" table (the format of
// bsl::ParseTokenTable, matched as bsl::ClassifyTokenLiteral does) is checked and printed as the parameter
// &МСПТокен<index+1>. The host binds the token's value to that parameter and executes the canonical text, so every
// parameter МСПТокен<index+1> of the table, bound or written by hand, has the token's origin (protected ->
// token_protected, unknown -> token_unknown); a "params" entry of that name (matched as parameter names are) must
// carry the same origin, and one that differs is Q_POLICY_INVALID / confirm. A token parameter whose index the table
// does not list keeps its "params" origin. A token inside a longer literal and a token-format literal missing from
// the table stay literals; a malformed "tokens" table is Q_POLICY_INVALID / confirm. CheckQueryWithPolicy never
// binds: a query text in client code keeps its literals.
namespace lineage::strict {

std::string CheckQueryImpl(const std::string& query_utf8, const std::string& policy_json);

// The same check over an already parsed policy, as data (CheckQueryImpl serializes exactly this). The BSL checker
// uses it for query texts in client code: it needs the reference targets of the columns, which the JSON omits.
struct QueryReason {
    std::string code;
    bool has_pos = false;
    Pos pos;
};

struct QueryCheckResult {
    std::string verdict = "confirm";             // "safe" | "confirm" | "incomplete"
    std::vector<QueryReason> reasons;
    std::string canonical;                       // empty unless the query parsed and printed
    bool parsed = false;                         // false: no columns are known
    std::vector<ColumnLabel> columns;            // names as the platform will give them; refs as analysed
    std::set<std::string> missing_tables;
};

// Never throws: an internal failure is Q_INTERNAL / confirm.
QueryCheckResult CheckQueryWithPolicy(const std::string& query_utf8, const FieldPolicy& policy);

}  // namespace lineage::strict
