#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bsl/bsl_allowlist.h"
#include "bsl/bsl_ast.h"
#include "strict/field_policy.h"
#include "strict/origin.h"

// Symbol and type tracking of the strict BSL checker. AnalyzeCodeWalk walks a validated AST and
// proves, for every identifier, global function, "Новый" type, member, method and index, that it is on the
// allowlist (bsl_allowlist.h) and that the receiver's type is KNOWN. Anything else is a reason code (the list is
// in the comment of CodeAnalysis) and the verdict is Confirm. The same walk carries the origin labels of the taint
// pass: bsl_taint.h drives it to a fixpoint and hands the query texts to CheckQuery. Risk classes and the
// Результат labels are Tasks 26-27.
//
// Soundness rules (a type that cannot be proven is Unknown, and nothing is allowed on Unknown):
//   - the walk is flow-sensitive; branches (Если, ?(), И / ИЛИ, Попытка) join their environments, and two
//     different types join to Unknown;
//   - a variable assigned on only one path is "maybe unassigned"; reading it is C_UNKNOWN_IDENT, because on the
//     other path the name could resolve to a module variable of the host;
//   - a loop is iterated to a fixpoint: at the loop head and after the loop, every variable assigned anywhere in the
//     loop (including the loop variable and out-parameters) holds the join of its value before the loop and of
//     EVERY value assigned to it in the loop, wherever the assignment is (so Прервать / Продолжить need no
//     separate states); a variable the loop assigns but that was unassigned before it is maybe unassigned there;
//     the Исключение block of Попытка starts from the same kind of join over the Попытка body;
//   - a collection element and the result of an unknown call are Unknown;
//   - a Запрос object may only be used as the receiver of a call or of a Текст assignment: copying it into
//     another variable, a collection or an argument is C_QUERY_ESCAPES, so a text change can never reach an object
//     the checker did not associate with its allocation site;
//   - names are matched case-insensitively with ё = е; two variable spellings that differ only in ё / е are
//     C_AMBIGUOUS_IDENT, so the result does not depend on whether the platform treats them as one name.
//
// Lost queries: a Запрос site is modelled only through Выполнить, УстановитьПараметр with the allowed arity and a
// literal, non-reserved name, and Текст assignments of a constant-folded text. Any other use of a Запрос value is a
// reason, and the results of that site are no longer labelled from its texts, so a confirmed run cannot return a
// value under a weaker label than the one it really has:
//   - parameters the walk cannot follow (C_DYNAMIC_PARAM, C_RESERVED, C_BAD_ARGS on a Запрос method, a member,
//     method, index or iteration the allowlist does not have on Запрос, Параметры included): the columns keep their
//     names, every label is Unknown and no reference target is known (LabelState::unlabeled_queries);
//   - a text the walk cannot follow (C_DYNAMIC_QUERY, C_QUERY_ESCAPES: the copy may be changed by code the walk does
//     not see, C_BAD_ARGS of Новый Запрос): no column is known (LabelState::untracked_queries).
//
// Label rules (the lattice is lineage::strict::Origin, the same as the query checker's):
//   - literals are Client; a whole string literal equal to an issued token is that token's origin (ProtectedDirect
//     or Unknown) and is printed as МСПЗначенияТокенов[n]; a query column has its CheckQuery label; a dereferenced
//     attribute (Ссылка.Имя) has its policy class; anything the walk cannot label is Unknown;
//   - a value moved (assigned, stored in a collection, read back) keeps its label; any other operation over a
//     tainted input is C_FUNC_OVER_PROTECTED and its result is Transform(Join(inputs));
//   - a reference read as a string (Строка, Формат, Лев, ..., "x" + Ссылка) is labelled with the presentation class
//     of its targets; a value of unknown type may be a reference with unknown targets, so its presentation is
//     Unknown;
//   - pc: under a branch, loop, ?() or right-hand side of И / ИЛИ conditioned on a tainted value (and in the
//     Исключение block after a tainted operation), every assignment, collection mutation and parameter binding is
//     joined with the condition's label, and the construct is C_IMPLICIT_FLOW; a Прервать / Продолжить under such a
//     pc taints the rest of the loop and the values the loop leaves;
//   - .Количество(), Свойство() and the iteration of a collection read its shape label (the pc of its mutations),
//     not its contents; the row count of a query result is open (an accepted residual leak of the query path).

namespace lineage::bsl {

using strict::Origin;

// A structure key / column name that may appear in a result path: ASCII letters, digits and '_' (not first), and
// non-ASCII bytes. A key with '.', '[', ']' or a blank would make two paths ambiguous.
bool IsPathKey(const std::string& key);

// Allowlist key: upper case, ё folded to е. The same normalisation as NormalizeIdentifierKey plus the ё fold that
// IsReservedName applies.
std::string AllowlistKey(const std::string& name);

struct ColumnInfo {
    std::string name;                // the column name as the platform gives it (first spelling); not compared
    bool name_collision = false;     // another column differs only in ё / е; not compared
    std::vector<std::string> refs;   // target tables when the column holds a reference (non-empty => Ty::Ref)
    Origin origin = Origin::Unknown; // label of the column's values (CheckQuery)
    bool refs_known = false;         // `refs` is complete; false: may be a reference whose targets were lost
    bool operator==(const ColumnInfo& o) const {
        return refs == o.refs && origin == o.origin && refs_known == o.refs_known;
    }
};

// Columns of a query result / value table / row, keyed by AllowlistKey. Immutable once built.
struct ColumnSet {
    std::map<std::string, ColumnInfo> columns;
    bool operator==(const ColumnSet& o) const { return columns == o.columns; }
};

// A token the host issued, as the policy's "tokens" table lists it (bsl_taint.h parses it). The table holds tokens
// and origins only, never the real values; the canonical code reads the value as МСПЗначенияТокенов[index].
struct TokenBinding {
    int index = -1;                  // 0-based position in the bound-value array
    Origin origin = Origin::Unknown; // ProtectedDirect ("protected") or Unknown ("unknown")
    bool operator==(const TokenBinding& o) const { return index == o.index && origin == o.origin; }
};
using TokenTable = std::map<std::string, TokenBinding>;   // key: the token text with ASCII letters in upper case

struct TypeCheckOptions {
    // Column labels of the query allocated at `site` (the "Новый Запрос" node), called at Запрос.Выполнить().
    // The taint driver supplies it from CheckQuery. Without it (or when it returns null) the columns are unknown
    // and a column read is C_UNKNOWN_MEMBER.
    std::function<std::shared_ptr<const ColumnSet>(const Expr* site)> columns_for_query;
    // Presentation classes and dereferenced fields of references. Null: presentations are Unknown and a
    // dereference is C_UNKNOWN_MEMBER.
    const strict::FieldPolicy* policy = nullptr;
    // Issued tokens. Null: no literal is bound, and a token-format literal is C_TOKEN_UNKNOWN.
    const TokenTable* tokens = nullptr;
    // Work counter shared by every walk of one analysis (statements plus environment entries copied); over
    // kMaxCodeWork the walk stops with C_LIMIT. Null: each walk counts on its own.
    size_t* work = nullptr;
};

constexpr size_t kMaxCodeWork = 500000;   // well under a second; ordinary client code uses a few percent

// What one position of a collection holds, for the Результат description: the structure key, the element of
// an array, the value of a map, the key of a map. Joined over every value ever stored there.
struct SlotLabels {
    std::optional<Origin> scalar;        // scalar values stored (joined)
    bool ref = false;                    // an untainted reference value is stored (the serializer describes it)
    bool opaque = false;                 // a value of a type the description does not model
    std::set<const Expr*> nested;        // collections stored
    bool operator==(const SlotLabels& o) const {
        return scalar == o.scalar && ref == o.ref && opaque == o.opaque && nested == o.nested;
    }
    bool Empty() const { return !scalar && !ref && !opaque && nested.empty(); }
};

struct FieldSlot {
    std::string name;                    // the key as written (first spelling)
    SlotLabels slot;
    bool collide = false;                // two spellings of the key differ in ё / е (AllowlistKey folds them)
    bool operator==(const FieldSlot& o) const { return name == o.name && slot == o.slot && collide == o.collide; }
};

// Flow-insensitive part of the label pass, joined monotonically over the whole program: collections can be
// aliased (Д = М, Результат = М before М.Добавить), so their contents live per allocation site, not per variable.
// An empty optional is "nothing stored yet".
struct ObjectLabels {
    std::optional<Origin> content;       // values stored in the collection (nested collections included)
    std::optional<Origin> shape;         // pc of every mutation: what its size and key set depend on
    std::optional<Origin> keys;          // Соответствие keys
    std::set<std::string> refs;          // reference targets among the stored values
    bool ref_unknown = false;            // a stored value may be a reference with unknown targets
    std::set<const Expr*> nested;        // collections stored in it
    // The structure of the collection for the Результат description.
    Ty kind = Ty::Unknown;               // Array / Structure / Map / ...: set at the allocation
    SlotLabels elem;                     // array elements, map values, structure values under a non-literal key
    SlotLabels key_slot;                 // map keys
    std::map<std::string, FieldSlot> fields;   // structure keys by AllowlistKey
    bool operator==(const ObjectLabels& o) const {
        return content == o.content && shape == o.shape && keys == o.keys && refs == o.refs &&
               ref_unknown == o.ref_unknown && nested == o.nested && kind == o.kind && elem == o.elem &&
               key_slot == o.key_slot && fields == o.fields;
    }
};

struct ParamBinding {
    Origin label = Origin::Open;         // join of every value the code binds to the parameter
    std::set<std::string> spellings;     // names as written (ё kept), for the policy lookup of the query check
    bool operator==(const ParamBinding& o) const { return label == o.label && spellings == o.spellings; }
};

struct LabelState {
    std::map<const Expr*, ObjectLabels> objects;                         // key: the allocating expression
    std::map<const Expr*, std::map<std::string, ParamBinding>> params;   // Запрос site -> AllowlistKey(name)
    // Запрос sites the walk cannot follow (see "Lost queries" above): unlabeled ones keep their column names with
    // Unknown labels, untracked ones (the text may differ from every recorded one) have no known columns at all.
    std::set<const Expr*> unlabeled_queries;
    std::set<const Expr*> untracked_queries;
    bool operator==(const LabelState& o) const {
        return objects == o.objects && params == o.params && unlabeled_queries == o.unlabeled_queries &&
               untracked_queries == o.untracked_queries;
    }
};

struct CodeReason {
    std::string code;
    Pos pos;
};

// Hand-off to the taint pass: one Запрос site may carry several texts (Новый Запрос(t1), then .Текст = t2, or assignments on
// different paths). CheckQuery every text recorded for the site and use the worst case per column across them.
// A constant query text given to a Запрос: "Новый Запрос(<text>)" or "<Запрос>.Текст = <text>".
struct QueryTextUse {
    const Expr* site = nullptr;        // the "Новый Запрос" node the text belongs to
    const Expr* text_expr = nullptr;   // the expression that supplies the text
    std::string text;                  // the constant-folded text (a literal or a concatenation of literals)
};

enum class CodeVerdict { Safe, Confirm };

// One entry of result_labels: a path over the serialized Результат and its label (see check_code.h).
struct ResultPath {
    std::string path;
    std::string label;   // open | client | protected | protected_derived | unknown | mixed | container | ref
};

// "Tainted" labels: ProtectedDirect, ProtectedDerived, Mixed and Unknown.
inline bool IsTainted(Origin o) { return o != Origin::Open && o != Origin::Client; }

// Reasons (all positions are in the client's own text; no value is ever echoed):
//   C_UNKNOWN_IDENT   name not assigned on every path and not allowlisted; also an unknown global function
//   C_COMMON_MODULE   "Имя.Метод(...)" on an unassigned name that is not a context name (a module call)
//   C_UNKNOWN_RECEIVER  member / method / index / iteration on a value whose type is Unknown
//   C_UNKNOWN_MEMBER  property or column not allowed on a known type (also assignment to a property)
//   C_UNKNOWN_METHOD  method not allowed on a known type
//   C_UNKNOWN_INDEX   index on a type that has none in the allowlist
//   C_UNKNOWN_TYPE    "Новый" of a type that is not allowlisted
//   C_UNKNOWN_ITER    "Для Каждого" over a type that is not iterable in the allowlist
//   C_EVAL            member named Вычислить / Eval
//   C_DYNAMIC_QUERY   query text that is not a constant-folded literal
//   C_DYNAMIC_KEY     structure / map key (index, Свойство) or a type name that is not a literal
//   C_DYNAMIC_PARAM   query parameter name that is not a literal
//   C_BAD_PARAM_NAME  query parameter name literal outside the alphabet of a parameter after '&' (empty, a space, a
//                     digit first, an invisible or look-alike character; strict::IsIdentifierName)
//   C_BAD_ARGS        argument count outside the allowlisted range, or a malformed out-parameter
//   C_QUERY_ESCAPES   a Запрос value used other than as a receiver (its results are no longer labelled, see "Lost
//                     queries")
//   C_AMBIGUOUS_IDENT variable spellings that differ only in ё / е
//   C_LIMIT / C_PARSE a tree that fails ValidateBody (hand-built trees); C_LIMIT also when the walk or the fixpoint
//                     runs out of budget
//   C_RESERVED        a query parameter name in the МСПТокен namespace (set by the code or used in a query text)
// Taint:
//   C_FUNC_OVER_PROTECTED  an operation whose input is tainted: an operator (arithmetic, concatenation, comparison,
//                     И / ИЛИ / НЕ), an allowlisted function or method, a key or an index, a dereference of a
//                     tainted reference; also a reference read as a string unless every target's presentation is open
//   C_IMPLICIT_FLOW   a branch, loop, ?() or И / ИЛИ conditioned on a tainted value, and a Попытка whose body runs
//                     an operation over tainted data (the Исключение block depends on it)
//   C_TAINTED_KEY     a tainted key inserted into a Соответствие / Структура, or any key inserted next to tainted
//                     keys (the de-duplication compares them)
//   C_TOKEN_IN_STRING a token inside a longer string (a transformation of a token)
//   C_TOKEN_UNKNOWN   a token-format string the policy's token table does not list
//   C_TOKEN_AS_NAME   a token used as a name (parameter, key, column, type), not as a value
//   C_TOKEN_IN_QUERY  a token in a query text (in code only a token bound with УстановитьПараметр is supported)
//   Q_*               a reason of CheckQuery for a query text, at the position of that text in the code
struct CodeAnalysis {
    CodeVerdict verdict = CodeVerdict::Confirm;   // fail closed: Safe only on a clean pass
    std::vector<CodeReason> reasons;
    std::vector<CodeReason> risks;                // filled by the risk scan
    std::vector<QueryTextUse> query_texts;        // one entry per text expression
    std::set<std::string> missing_tables;         // tables the policy lacks (the host may retry with them)
    std::map<const Expr*, int> bound_tokens;      // string literals bound to issued tokens (PrintOptions)
    std::map<const Expr*, std::string> query_canonical;   // text expression -> canonical query text (PrintOptions)
    Origin result_label = Origin::Unknown;        // label of Результат at the end, contents included
    // The per-path labels of Результат. result_reasons (C_UNLABELED_RESULT, C_MIXED_RESULT, C_DERIVED_RESULT)
    // are NOT in `reasons` and do not change `verdict`: the CheckCode layer merges them.
    std::vector<ResultPath> result_labels;
    std::vector<CodeReason> result_reasons;
};

// The final value of Результат as the description pass sees it (a flattened VType of bsl_types.cpp).
struct ResultValue {
    Ty ty = Ty::Unknown;
    Origin label = Origin::Unknown;
    bool maybe_ref = true;                       // the value may be a reference
    std::set<const Expr*> objs;                  // collections the value may be
    std::shared_ptr<const ColumnSet> columns;    // ValueTable
};

// Builds result_labels / result_reasons from the final value (null: Результат was never assigned, so it keeps the
// host's initial Неопределено) and the label state. Mirrors БезопасноеЗначениеДляПередачи, depth limit included.
// Never throws.
void DescribeResult(const ResultValue* value, const LabelState& state, CodeAnalysis& out);

// One walk of the type and label pass. `state` is read and joined into; bsl_taint.h repeats the walk until it is
// stable. The reasons describe this walk only.
CodeAnalysis AnalyzeCodeWalk(const Body& body, const TypeCheckOptions& options, LabelState& state);

}  // namespace lineage::bsl
