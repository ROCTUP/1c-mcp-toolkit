#pragma once
#include <string>
#include <vector>

#include "bsl/bsl_ast.h"
#include "bsl/bsl_types.h"
#include "strict/field_policy.h"

// Taint pass of the strict BSL checker: drives the type and label walk of bsl_types.h to a fixpoint
// and checks every query text of the client code with CheckQuery under the same policy.
//
//   - The flow-insensitive state (collection contents per allocation site, query parameters per Запрос site) and
//     the column labels depend on each other (a parameter may come from another query's column). Each pass fixes
//     the parameters, checks the queries with them and repeats the walk from a fresh state until the collection
//     state is stable; passes repeat until the parameters the code binds equal the ones the pass assumed. A
//     budget overrun is C_LIMIT.
//   - Every text recorded for a Запрос site is checked; a column takes the worst label across the texts, and a
//     site whose texts disagree on the column names has no known columns. The texts' reasons are added as Q_* at
//     the code position of the text; the canonical query text of each is in CodeAnalysis::query_canonical.
//   - A query in client code starts with NO parameters: only УстановитьПараметр on that site binds one, with the
//     join of every value bound to it (Open / Client -> client, ProtectedDirect -> token_protected, anything else
//     -> token_unknown). A parameter the code never sets is missing, so the query is not safe. The host's
//     "params" table of the policy is not used here.
//   - Token-bound values: a string literal whose whole value is a token from the policy's "tokens" table is that
//     token's origin and goes to CodeAnalysis::bound_tokens (the canonical printer writes МСПЗначенияТокенов[n]).
//     A token inside a longer literal is C_TOKEN_IN_STRING; a token-format literal missing from the table is
//     C_TOKEN_UNKNOWN; a token as a name is C_TOKEN_AS_NAME; a token in a query text is C_TOKEN_IN_QUERY.
//
// Hand-offs:
//   - The keys of bound_tokens / query_canonical / query_texts point into `body`: it must outlive the result.
//   - Result description: a protected_derived / mixed / unknown value in Результат must not auto-pass (equal derived values
//     would show equal tokens); result_label is the label of the whole Результат, the per-path labels still have
//     to be built from the same walk.
//   - A token literal written into a query text would need an inserted З.УстановитьПараметр("МСПТокен<n>",
//     МСПЗначенияТокенов[n-1]) statement; the canonical printer replaces expressions only, so it is refused
//     (C_TOKEN_IN_QUERY) and the client binds the token as a parameter value instead. The refusal is code-only:
//     CheckQuery (execute_query) prints such a literal as &МСПТокен<n> itself (check_query.h).
//   - CheckQuery reports no value types: a column without reference targets may still hold a reference (ЕСТЬNULL,
//     ОБЪЕДИНИТЬ, parameters), so a string conversion of such a column needs confirmation.

namespace lineage::bsl {

// Token format of the host (ЭтоВалидныйТокенФормат): "[" + Latin letters + "-" + at least five digits + "]".
bool IsTokenFormat(const std::string& s);

enum class TokenUse { None, Bound, InString, Unknown };

// Classifies a string literal: the whole value equal to an issued token (Bound, `binding` set), a whole
// token-format value that is not issued (Unknown), a token or token-format part inside the value (InString).
// Tokens match case-insensitively in their Latin letters, as the host compares them.
TokenUse ClassifyTokenLiteral(const std::string& value, const TokenTable* tokens, TokenBinding* binding);

// Reads the policy's "tokens" object: {"<token>": {"index": <int >= 0>, "origin": "protected" | "unknown"}}.
// No "tokens" key is an empty table. A malformed table, a key that is not in the token format, a negative or
// repeated index, or two keys equal up to case is false (the caller refuses the check).
bool ParseTokenTable(const std::string& policy_json, TokenTable& out);

// Risk classes: R_PROTECTION, R_DEFERRED, R_WRITE, R_CREDENTIALS (bsl_allowlist.h, kRiskRules). AnalyzeCode
// fills CodeAnalysis::risks from the tree (every name: variables, calls, "Новый" types, loop variables, members and
// methods after a dot) and a risk forces Confirm. ScanRisksInSource is the same scan over the tokens of the source
// text, for code the parser refused (no tree): the gate still shows the markers. If the tokenizer refuses part of the
// text (an Unknown token, oversize) the whole normalised text is searched with word boundaries. The CheckCode entry point merges the
// two for the CheckCode answer (MergeRisks); the host appends R_RELAUNCH / R_CLIENT_CONTEXT itself.
std::vector<CodeReason> ScanRisksInSource(const std::string& utf8);
// Risks of code the parser refused: ScanRisksInSource plus R_UNKNOWN_EFFECT (no tree, nothing proven); for C_EVAL and
// C_DYNAMIC_NEW also R_PROTECTION and a raw scan of the string literals (alone and joined).
std::vector<CodeReason> RefusedCodeRisks(const std::string& utf8, const std::string& parse_error_code);
void MergeRisks(std::vector<CodeReason>& into, const std::vector<CodeReason>& more);

// R_UNKNOWN_EFFECT (BSL side: protection class): a call the allowlist does not know (module function, unknown global,
// method on an Unknown receiver or not allowed on a known type, unknown "Новый" type) can do anything, writes included.
// Property reads without a call stay a reason only.

// The taint pass over a body. `tokens` may be empty. Never throws: an internal failure is C_INTERNAL / confirm.
CodeAnalysis AnalyzeCode(const Body& body, const strict::FieldPolicy& policy, const TokenTable& tokens);

}  // namespace lineage::bsl
