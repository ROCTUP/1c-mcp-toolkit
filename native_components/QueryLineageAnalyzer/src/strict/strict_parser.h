#pragma once
#include <string>

#include "strict/strict_ast.h"
#include "strict/strict_tokenizer.h"

// Any dotted name followed by '(' in FROM becomes a Source::K::VirtualTable; whether it is a real virtual table (and
// whether a plain Table is a metadata table or a temporary table) is validated by the checker.
// Function names are NOT allowlisted here: any identifier followed by '(' parses as a Call. The allowlist
// of callable functions lives in the checker. A keyword after '.' is a ParseError (Q_PARSE_KEYWORD_SEGMENT)
// except ССЫЛКА, ПРЕДСТАВЛЕНИЕ, ПРЕДСТАВЛЕНИЕССЫЛКИ, ЗНАЧЕНИЕ, which are accepted as non-first path
// segments and stored with their source spelling as ordinary Field parts.

namespace lineage::strict {

// Maximum nesting of parenthesised expressions / chained unary operators (Q_PARSE_LIMIT beyond it).
constexpr int kMaxParseDepth = 64;
// Maximum depth of a built expression tree, chains of left-associative operators included
// (Q_PARSE_LIMIT beyond it). Keeps every later recursive walk and the destructor inside a small stack.
constexpr int kMaxExprDepth = 256;
// Node-visit budget of ValidateExpr (equals the tokenizer token limit); stops shared-child DAG blowup.
constexpr size_t kMaxExprNodes = 200000;

// Maximum statements in one batch (Q_PARSE_LIMIT beyond it), union parts per query, and virtual-table parameter slots.
constexpr size_t kMaxBatchStatements = 64;
constexpr size_t kMaxUnionParts = 256;
constexpr size_t kMaxVirtualTableParams = 32;

struct ParseResult { bool ok = false; Batch batch; ParseError error; };
struct ExprResult { bool ok = false; ExprP expr; ParseError error; };

ParseResult ParseBatchStrict(const std::string& utf8);
ExprResult ParseExpressionForTest(const std::string& utf8);

// Structural validator for an expression tree: every node must have a concrete kind (Expr::K::Unset is
// rejected), the arity and payload its kind requires, no null children, and Row only as the left operand
// of a subquery-form В / В ИЕРАРХИИ. Iterative, so it is safe on arbitrary caller-built trees.
// Returns false and fills `err` (code Q_PARSE_INVALID_NODE, or Q_PARSE_LIMIT when deeper than kMaxExprDepth).
// Subquery bodies are walked too (every clause of every nested Select, same checks and the same node budget);
// subquery nesting deeper than kMaxParseDepth is Q_PARSE_LIMIT.
bool ValidateExpr(const ExprP& root, ParseError& err);

// Structural validator for a whole batch: 1..kMaxBatchStatements statements, each a well-formed Drop or Select;
// every clause of every Select (items, sources, joins, virtual-table parameters, unions, order, totals) is
// checked and every embedded expression goes through the ValidateExpr rules. Iterative; fills `err` like
// ValidateExpr. ParseBatchStrict runs it on its own result, so a returned batch is always valid.
bool ValidateBatch(const Batch& batch, ParseError& err);

}  // namespace lineage::strict
