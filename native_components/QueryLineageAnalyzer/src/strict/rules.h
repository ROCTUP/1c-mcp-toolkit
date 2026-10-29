#pragma once
#include <map>
#include <set>
#include <vector>

#include "strict/analyzer.h"
#include "strict/field_policy.h"
#include "strict/scope.h"
#include "strict/strict_ast.h"

// Rule engine of the strict checker: which operations over protected values need confirmation.
// It runs after name resolution and labelling, over the labels the analyzer recorded for every expression node.
// Reason codes are fixed and public, carry no values, and point into the client's own text.
//
// Implicit channels: a select whose row set is filtered, joined or grouped by protected data
// ("rows_protected") must not feed an aggregate (Q_AGGREGATE_ROWSET: a COUNT / SUM over open fields would expose
// the cardinality of the protected condition), and a subquery with such a row set is tainted as a whole when it
// is used as a value. Virtual-table parameters are labelled by the analyzer against the base table of the
// register and checked with the condition rules; a protected slot that is not condition-shaped is Q_VT_PARAM.

namespace lineage::strict {

// Labels recorded by the analyzer, keyed by AST node (the batch outlives the table).
struct LabelTable {
    std::map<const Expr*, FieldLabel> expr;                       // label of every walked expression
    std::map<const Select*, std::vector<Column>> select_columns;  // result columns of a select (after ОБЪЕДИНИТЬ)
    std::map<const Select*, bool> rows_protected;    // the select's own row set depends on a protected condition
    std::map<const Select*, bool> result_protected;  // ... or that of any ОБЪЕДИНИТЬ part (the whole result)
    std::set<const Expr*> outer_refs;   // field paths of a subquery that do not resolve in its own sources
};

// True when a grouping item is also selected as is (same field path): the group key is then returned as a
// protected value, and the group counts describe that value only. Any other protected grouping hides the key.
inline bool GroupItemIsSelected(const Select& s, const Expr& g) {
    if (g.kind != Expr::K::Field) return false;
    for (const SelectItem& item : s.items) {
        if (item.wildcard || !item.expr || item.expr->kind != Expr::K::Field) continue;
        const Expr& x = *item.expr;
        if (x.parts.size() != g.parts.size()) continue;
        bool same = true;
        for (size_t i = 0; same && i < x.parts.size(); ++i) same = FoldedKey(x.parts[i]) == FoldedKey(g.parts[i]);
        if (same) return true;
    }
    return false;
}

// Appends rule and resolution reasons to `result` and sets the verdict.
void ApplyRules(const Batch& batch, const FieldPolicy& policy, const LabelTable& labels, AnalysisResult& result);

}  // namespace lineage::strict
