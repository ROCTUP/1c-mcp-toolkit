#pragma once
#include <string>

#include "bsl/bsl_ast.h"

// Allowlist parser for the BSL subset accepted by the strict checker. It is a closed grammar: a
// construct that is not listed is a ParseError with a reason code, never a pass-through node.
//
// Reason codes: C_EVAL (Выполнить, Вычислить), C_HANDLER (ДобавитьОбработчик / УдалитьОбработчик),
// C_RAISE (ВызватьИсключение with an argument), C_DYNAMIC_NEW (Новый("...")), C_LIMIT (size / depth / statement
// budgets), C_RESERVED (a name of bsl_reserved.h used by client code), C_PARSE (everything else: unknown tokens, labels, Перейти, Процедура / Функция / Перем / Возврат,
// Асинх / Ждать, mixed Latin/Cyrillic identifiers, malformed syntax), C_INTERNAL.
//
// A keyword after '.' is a member name (Запрос.Выполнить()) and never changes statement structure.

// Hand-offs for the printer and the allowlist:
// - the printer must escape control, bidi and zero-width characters in string literals;
// - default-deny in the allowlist: Объект.Вычислить / Объект.Выполнить, ЭтотОбъект.*, string-named callbacks
//   (ОписаниеОповещения and the like): the parser accepts them only as names;
// - allowlist keys must treat ё and е as equal;
// - identifiers mixing Latin and Cyrillic (HTTPЗапрос, COMОбъект) are rejected here; acceptable, protection class.
// Variables, global callees, "Новый" type names and loop variables must be plain names (no keyword, no eval
// name); only member names after a dot may be keywords.

namespace lineage::bsl {

constexpr int kMaxParseDepth = 64;          // nested parentheses / chained unary operators / blocks in one expression
constexpr int kMaxBlockDepth = 32;          // nested compound statements (Если/Для/Пока/Попытка); leaf statements don't
                                            // count: 32 nested Если are accepted, the 33rd is C_LIMIT
constexpr int kMaxExprDepth = 256;          // depth of a built expression tree (postfix and binary chains included)
constexpr size_t kMaxStatements = 2000;     // statements in one body, nested ones included
constexpr size_t kMaxCallArgs = 64;
constexpr size_t kMaxNodes = 200000;        // node-visit budget of ValidateBody

struct ParseResult { bool ok = false; Body body; ParseError error; };

// allow_reserved is for the canonical printer's re-parse only: it admits the names of bsl_reserved.h.
ParseResult ParseBsl(const std::string& utf8, bool allow_reserved = false);

// Structural validator: concrete kinds only (Unset is rejected), the arity and payload each kind requires
// (identifier charset without Latin/Cyrillic mixing, number / date text, flags only on the kinds that use
// them), no null children, Прервать / Продолжить only inside a loop, depth and node budgets. Iterative, so it
// is safe on arbitrary caller-built trees. ParseBsl runs it on its own result.
bool ValidateBody(const Body& body, ParseError& err, bool allow_reserved = false);

// "Новый Запрос(...)" / "Новый Query(...)" with parentheses: the only "Новый" a member chain may follow
// (Новый Запрос(Текст).Выполнить().Выгрузить()). The object is never bound to a name, so it cannot escape; the parser
// accepts a '.' right after it and the printer writes it as a postfix base without parentheses.
bool IsChainableNew(const Expr& e);

// Deterministic S-expression dump (tests, round-trip property). Recursive: call it only on validated trees.
std::string DumpBsl(const Body& body);
std::string DumpBslExpr(const Expr& e);

}  // namespace lineage::bsl
