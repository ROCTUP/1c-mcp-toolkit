#pragma once
#include <map>
#include <string>

#include "bsl/bsl_ast.h"
#include "bsl/bsl_tokenizer.h"

// Canonical BSL text, printed from the AST only (never from source tokens). In strict mode this text is the only
// code ever executed, so the printer is fail-closed and mirrors the strict query printer:
//   - it prints only trees that pass ValidateBody (after replacements are applied, too);
//   - every identifier, number, date and string is re-lexed with the BSL tokenizer before it is written;
//   - a string value holding a control character (TAB and the line break excepted), a C1 control, a bidi /
//     zero-width / line-separator / format character, invalid UTF-8 or a bare CR is refused: BSL has no safe escape;
//   - a line break inside a plain string literal is written as a "|" continuation line, exactly as the tokenizer
//     reads it back;
//   - operands of binary / unary operators are parenthesised unless they are atoms; the only exception is the
//     left operand of a left-associative chain of the same level (a + b + c), which would otherwise nest the
//     parentheses once per term; comparisons are never chained;
//   - a keyword used as a member name is written in its stored spelling, after a dot only;
//   - the finished text is parsed again with ParseBsl and must dump-equal the (replaced) tree and print to itself.
// Anything else is a refusal with no usable text.
//
// Replacements (applied before printing, each target must occur in the tree exactly as a node pointer):
//   query_replacements: the node becomes the given text, built from single-line string literals joined with
//     "+ Символы.ПС +" at every line break, so no "|" appears in the output;
//   bound_tokens: the node becomes МСПЗначенияТокенов[<n>].
// A replacement on a node of an assignment-target chain is a refusal (C_PRINT_REPLACEMENT). A reserved name
// (bsl_reserved.h) in the original tree is a refusal (C_PRINT_RESERVED); only the printer itself creates them.
// A replacement whose target does not occur in the tree, or that is listed in both maps, is a refusal.

namespace lineage::bsl {

// Equal to the tokenizer's source limit: a longer text could not be parsed back anyway.
constexpr size_t kMaxCanonicalBytes = kMaxSourceBytes;
constexpr int kMaxPrintDepth = 300;

struct PrintOptions {
    std::map<const Expr*, std::string> query_replacements;
    std::map<const Expr*, int> bound_tokens;
};

struct PrintResult {
    bool ok = false;
    std::string text;      // empty unless ok
    ParseError error;      // code C_PRINT_* or the ValidateBody code; the detail never echoes input text
};

PrintResult PrintBslCanonicalChecked(const Body& body, const PrintOptions& options = PrintOptions());

// The canonical text, or an empty string when refused (an empty text is a valid empty program, so callers that
// execute the result must use the Checked form and test `ok`).
std::string PrintBslCanonical(const Body& body, const PrintOptions& options = PrintOptions());

}  // namespace lineage::bsl
