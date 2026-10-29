#pragma once
#include <string>

// Strict code check, the entry point behind the CheckCode component method. Returns a JSON object:
//   {"api":1,"verdict":"safe|confirm|incomplete",
//    "reasons":[{"code","line","col","offset"}], "risks":[{"code","line","col"}],
//    "canonical":"...", "result_labels":{"<path>":"<label>"}, "missing_tables":[...]}
// The envelope mirrors CheckQueryImpl (strict/check_query.h) except that there are no "columns".
//
// Verdict: "safe" only when there is no reason, no risk, no missing table and the canonical text printed; "incomplete"
// when the policy lacks tables (listed in "missing_tables", the host retries with them added; never safe); otherwise
// "confirm". A parse refusal is "confirm" with the parser's code as the only reason and RefusedCodeRisks as the risks
// (it always carries R_UNKNOWN_EFFECT). A malformed policy (or token table) is C_POLICY_INVALID / confirm. Never
// throws: an internal failure is C_INTERNAL / confirm with R_UNKNOWN_EFFECT.
//
// "reasons": the codes of bsl_types.h and bsl_taint.h (C_*, Q_*) plus the Результат codes below. A reason without a
// position of its own (the Результат ones, C_LIMIT) reports line 1, col 1, offset 0. "risks": R_PROTECTION, R_DEFERRED,
// R_WRITE, R_CREDENTIALS, R_UNKNOWN_EFFECT; the host adds R_RELAUNCH / R_CLIENT_CONTEXT itself. Any risk forces confirm.
//
// "canonical" is the only code the host may execute. It is empty unless the code parsed, the walk did not fail
// internally (C_INTERNAL / C_LIMIT / Q_INTERNAL), every query text has a canonical query and the printer succeeded;
// it is never produced for "incomplete". The host runs it with МСПЗначенияТокенов[n] bound to the value of the
// token of the policy's "tokens" table whose 0-based "index" is n. A confirm verdict keeps the canonical text, so
// the user confirms exactly what runs.
//
// "result_labels" (binding contract with the masker): path -> label over the SERIALIZED Результат, i.e.
// the value БезопасноеЗначениеДляПередачи (ObjectModule) produces, not the BSL types:
//   $                the root (the host wraps it under "data" and prefixes the paths accordingly)
//   .Key             a structure key (a literal of the code; an identifier: no dot, brackets or blanks)
//   [*]              any element of an array, a table row or a map entry
//   [*].key/.value   key and value of a map entry (a Соответствие is an array of {key, value})
//   [*].Column       a column of a table (ТаблицаЗначений / Выгрузить() is an array of row objects)
//   a table row, a map entry and the root of any collection are labelled "container"
// Keys keep their written spelling; the masker compares paths upper-cased. Structure keys that differ only in case or
// in ё / е are one key (their labels are joined). A value deeper than the serializer's limit of 5 levels is the string
// "<max_depth>"; the checker cannot see what it hides, so its path is "unknown" (C_UNLABELED_RESULT, confirm). Any
// other cut-off (description budget) is "unknown" too, never "open". A Результат never assigned is the host's initial Неопределено: "$" is
// "open". The paths echo key and column names chosen by the client (like the column names of CheckQuery).
// Labels:
//   open               platform data that is not protected
//   client             a value the client wrote (a literal and its concatenations)
//   protected          a value read straight from a protected field; the masker tokenizes it
//   protected_derived  a value computed from protected data      -> reason C_DERIVED_RESULT      -> confirm
//   mixed              a position filled from protected and non-protected values
//                                                                -> reason C_MIXED_RESULT        -> confirm
//   unknown            nothing is proven (also a type the description does not model, a value under a key that is
//                      not a literal, a join of different shapes, an over-budget description)
//                                                                -> reason C_UNLABELED_RESULT    -> confirm
//   container          an array / structure / map / table / row (the masker looks at the children)
//   ref                an untainted reference value: the serializer turns it into an object description and the
//                      masker applies the reference rule to it (its Представление is always tokenized). The checker
//                      writes "ref" where it knows reference targets (a query column with targets, a stored
//                      reference); a column without targets stays "open", the masker's rule is not limited to "ref".
// A container whose size or key set depends on tainted data is also C_DERIVED_RESULT. A derived, mixed or unknown
// label therefore never auto-passes. These reasons are added by this layer only: bsl_taint.h's AnalyzeCode verdict is
// unchanged (CodeAnalysis::result_labels / result_reasons carry them).
//
// Lifetime: the parsed Body lives inside the call until the JSON is built, so the node pointers of the analysis
// (query_canonical, bound_tokens) never dangle.
namespace lineage::bsl {

std::string CheckCodeImpl(const std::string& code, const std::string& policy_json);

}  // namespace lineage::bsl
