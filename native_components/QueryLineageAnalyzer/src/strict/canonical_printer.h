#pragma once
#include <cstddef>
#include <string>

#include "strict/strict_ast.h"

// Canonical query text, printed from the AST only (never from source tokens). In strict mode this is the only
// text ever executed, so the printer is fail-closed:
//   - it prints only batches that pass ValidateBatch;
//   - every identifier, parameter, number and string is re-lexed with the strict tokenizer before it is written
//     (an identifier must lex as exactly one Identifier token, a string as exactly one String token with the same
//     value, so no byte that the tokenizer did not validate is ever emitted);
//   - the finished text is parsed again with ParseBatchStrict and must give the same tree (DumpBatch with
//     derived result names) and print to the same text. Anything else is a refusal, never a best-effort text.
// Layout: one select item / source / clause per line; keywords in canonical Russian spelling; every select item
// of a query (and of the first part of an ОБЪЕДИНИТЬ) carries an explicit КАК <name> given by ComputeResultNames
// (explicit alias, derived Field name, else Поле<N>; unique per query head). Binary operators and predicates are
// parenthesised by precedence (equal precedence on the right, any operand of НЕ and unary minus that is not
// primary, and a МЕЖДУ under И / ИЛИ).

namespace lineage::strict {

// Hard cap on the printed text and on the combined expression / subquery recursion of the printer.
constexpr size_t kMaxCanonicalBytes = 16 * 1024 * 1024;
constexpr int kMaxPrintDepth = 300;

struct PrintResult {
    bool ok = false;
    std::string text;      // empty unless ok
    ParseError error;      // code Q_PRINT_* or the ValidateBatch code; the detail never echoes input text
};

PrintResult PrintCanonicalChecked(const Batch& batch);

// Convenience: the canonical text, or an empty string when the batch was refused (an empty text never parses,
// so it can not be executed by mistake).
std::string PrintCanonical(const Batch& batch);

}  // namespace lineage::strict
