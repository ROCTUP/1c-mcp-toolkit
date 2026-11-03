#pragma once
#include <string>
#include <vector>

#include "strict/strict_ast.h"

namespace lineage::strict {

// Deterministic S-expression dump of an expression tree, used by tests and by the round-trip property.
// Recursive: call it only on trees that passed the parser's depth limit / ValidateExpr.
// Subquery bodies inside expressions are dumped in full as "(subquery <select>)".
// `fill_result_names` shows the aliases the canonical printer derives for unaliased select items
// (see DerivedResultAlias), so a tree can be compared with the re-parse of its canonical text.
std::string DumpExpr(const Expr& e, bool fill_result_names = false);

// Deterministic S-expression dump of a whole batch (statements, clauses, subquery bodies). Same recursion caveat.
std::string DumpBatch(const Batch& b, bool fill_result_names = false);

// The name an unaliased Field item gets (last segment for 1-2 parts, concatenation of the segments after the
// first for 3+ parts). Empty for every other expression, and when the name cannot follow КАК: a keyword other than
// the four path keywords (Ссылка, Представление, ПредставлениеСсылки, Значение are kept), or a Latin/Cyrillic mix.
std::string DerivedResultAlias(const Expr& e);

// Final result column names of a query head, in output order (the names the canonical text carries after КАК):
// an explicit alias is kept; an unaliased Field gets DerivedResultAlias; any other item (and a Field whose name
// is not printable) gets Поле<N>, N = 1-based item position. A derived name equal to an earlier one
// (case-insensitive, ё = е) gets the smallest integer suffix that makes it unique. A wildcard item has an empty
// name. Returns false (names cleared) when two explicit aliases collide. Names of an ОБЪЕДИНИТЬ come from its
// first part, i.e. from the head Select.
bool ComputeResultNames(const Select& head, std::vector<std::string>& names);

// ComputeResultNames for every statement of a batch (a Drop statement yields an empty list).
bool StatementResultNames(const Batch& b, std::vector<std::vector<std::string>>& out);

}  // namespace lineage::strict
