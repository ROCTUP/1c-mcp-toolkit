#pragma once
#include <set>
#include <string>
#include <vector>

#include "strict/field_policy.h"
#include "strict/origin.h"
#include "strict/scope.h"
#include "strict/strict_ast.h"

// Strict analysis of a parsed query batch: name resolution (scopes, aliases, temporary tables, subqueries) and
// the origin label of every output column of the batch's last statement. Later passes add expression labels and
// the rule engine on top of the same walk.

// Contract for the rule engine: ANY column or expression labelled Origin::Unknown must lead to Confirm,
// whether or not a resolution issue was recorded. Resolution assumes the policy lists every field of every table;
// a name that an outer query resolves while an inner policy table does not list it is reported as ambiguous.

// Notes for Tasks 17/18 (not yet enforced by labels):
//  (a) a subquery's result must carry taint from its ГДЕ / JOIN ON / СГРУППИРОВАТЬ / ИМЕЮЩИЕ conditions;
//      SubqueryOrigin here folds in only the returned columns, so the rule engine must add the conditions.
//  (b) КОЛИЧЕСТВО / СУММА over open fields are Open, but with a protected filter, join or grouping they leak
//      through the row set: they must not auto-pass.
//  (c) the host must classify EVERY parameter in the policy: a client-named parameter could still carry a token.
//      An unlisted parameter is Unknown (resolution issue + Confirm). A token_protected parameter is
//      ProtectedDirect and is restricted by the rules (rules.h): it may only be compared for equality with a
//      protected value (field or another token), appear in В (...) against such values, or be selected as is.
//      Origin classes of protected fields are not modelled, so "protected = protected" means any two protected
//      values; the host's token space must make a foreign-origin token unresolvable.
//
// Notes for labels and rules:
//  (d) virtual-table parameters are NOT labelled by the analyzer; rules.cpp CheckVtParam is syntactic only.
//      They must be labelled in a scope over the register's base table and checked with the Condition rules.
//  (e) LabelTable needs a per-select "row set filtered / joined / grouped by protected" flag, consumed by (b).
// Deliberate decisions:
//  (f) protected = protected equality that returns rows (self-joins, cross-field А.Имя = А.Код) stays Safe: the
//      main token map is keyed by value, so equal values already show equal tokens. Revisit if per-field
//      token spaces are introduced.
//  (g) filters equivalent to a token equality (НЕ x = tok, x В (tok), OR with open predicates, LEFT JOIN on a
//      token) stay Safe: the client holds the token. <> is Confirm (conservative, inconsistent but harmless).
//  (h) grouping by protected data, or РАЗЛИЧНЫЕ over a protected column, is exempt only in the top-level result
//      (the key is returned as is). A select that is consumed (temp table, derived table, union part,
//      subquery) hands its multiplicities on: its columns become derived protected and its row set protected.
//  (i) virtual-table slot meaning per kind is not derived: only the LAST slot may hold protected data, as a
//      condition; a tainted earlier slot is Q_VT_PARAM.
// Notes for Tasks 46/50: protected equality is allowed between ANY two protected values, e.g. a token of field X
//  against field Y, or А.Имя = А.Код. Closing it needs a field-origin id on protected labels and token records.
//
// Verdict: Safe only when there are no reasons, no missing tables and no resolution issues. ANY Unknown
// label anywhere yields a reason, and every resolution issue is mirrored as a Q_RESOLVE_* reason. Reasons carry
// a fixed code and a position in the client's own text, never values.

namespace lineage::strict {

struct Reason {
    std::string code;
    Pos pos;
};

struct ColumnLabel {
    std::string name;                   // final result name ("" for a wildcard item)
    Origin origin = Origin::Unknown;
    Pos pos;
    std::vector<std::string> refs;      // target tables when the column holds a reference
    bool refs_known = false;            // `refs` is complete (FieldLabel::refs_known in scope.h)
};

enum class Verdict { Safe, Confirm };

struct AnalysisResult {
    Verdict verdict = Verdict::Confirm;          // fail closed: Safe is set only by AnalyzeQuery on a clean pass
    std::vector<Reason> reasons;                 // public reason codes (parse failure, rule engine, resolution)
    std::vector<ColumnLabel> columns;            // output columns of the last statement
    std::set<std::string> missing_tables;        // tables/targets used but absent from the policy
    std::vector<ResolveIssue> resolution_issues; // everything name resolution could not prove
};

AnalysisResult AnalyzeQuery(const Batch& batch, const FieldPolicy& policy);

}  // namespace lineage::strict
