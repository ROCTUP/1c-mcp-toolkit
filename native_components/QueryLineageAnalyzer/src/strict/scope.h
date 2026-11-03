#pragma once
#include <set>
#include <string>
#include <vector>

#include "strict/field_policy.h"
#include "strict/origin.h"
#include "strict/strict_ast.h"

// Name resolution of the strict checker. Fail closed: a name that cannot be tied to exactly one source column
// (unknown, ambiguous, opaque source, ё/е spelling mismatch) resolves to Origin::Unknown and records an issue;
// there is no "guess the source" fallback.
//
// Matching keys: NormalizeIdentifierKey (the policy's key), compared exactly. In addition every lookup is done on
// a folded key (ё = е) to notice near-collisions: a name that only matches another spelling, or that matches two
// spellings, is ambiguous (the tokenizer rejects identifiers that would fold into keywords for the same reason).

namespace lineage::strict {

// Resolution problem. The rule engine maps these onto public reason codes; they are never shown to the client as is.
struct ResolveIssue {
    std::string code;   // RESOLVE_*
    Pos pos;
};

constexpr const char* kResolveUnknownName = "RESOLVE_UNKNOWN_NAME";
constexpr const char* kResolveAmbiguousName = "RESOLVE_AMBIGUOUS_NAME";
constexpr const char* kResolveUnknownField = "RESOLVE_UNKNOWN_FIELD";
constexpr const char* kResolveUnknownTemp = "RESOLVE_UNKNOWN_TEMP_TABLE";
constexpr const char* kResolveUnknownSource = "RESOLVE_UNKNOWN_SOURCE";
constexpr const char* kResolveColumnMismatch = "RESOLVE_COLUMN_MISMATCH";
// Mirrored as Q_PARSE_UNION_ARITY: the parts of an ОБЪЕДИНИТЬ return different numbers of columns.
constexpr const char* kResolveUnionArity = "PARSE_UNION_ARITY";
constexpr const char* kResolveUnknownParam = "RESOLVE_UNKNOWN_PARAM";
constexpr const char* kResolveBadStatement = "RESOLVE_BAD_STATEMENT";

struct Column {
    std::string name;                   // final result name
    Origin origin = Origin::Unknown;
    std::vector<std::string> refs;      // target tables when the column holds a reference
    bool refs_known = false;            // see FieldLabel::refs_known
};

struct Binding {
    enum class Kind { Table, Temp, Subquery };

    Kind kind = Kind::Table;
    std::string table;                  // Table: source name as written (looked up through the policy)
    bool known = true;                  // false: table/temp table not usable, every lookup is Unknown
    bool complete = true;               // false: the column list cannot be enumerated (wildcard in the producer)
    bool rows_protected = false;        // the row set was filtered / joined / grouped by protected data
    std::vector<std::vector<std::string>> name_paths;   // normalized segment keys the binding can be addressed by
    std::vector<Column> columns;        // Temp / Subquery
};

struct Scope {
    std::vector<Binding> bindings;
    const Scope* parent = nullptr;      // enclosing query for a subquery inside an expression (correlation)
};

struct FieldLabel {
    Origin origin = Origin::Unknown;
    std::vector<std::string> refs;
    // true: `refs` lists every reference target the value may have (empty: provably not a reference), as for a policy
    // field path, a literal or a primitive-typed operation. false (the default, fail closed): the value may be a
    // reference whose targets were lost (ВЫРАЗИТЬ to a reference type, ЗНАЧЕНИЕ, a parameter, ЕСТЬNULL / ВЫБОР /
    // МИНИМУМ / МАКСИМУМ over such a value, an ОБЪЕДИНИТЬ of a reference and a non-reference); its presentation is
    // not known, so nothing may be ordered by it.
    bool refs_known = false;
};

// Sink for everything resolution discovers.
struct ResolveSink {
    std::set<std::string>* missing_tables = nullptr;
    std::vector<ResolveIssue>* issues = nullptr;

    void Issue(const char* code, const Pos& pos) const;
    void MissingTable(const std::string& name) const;
};

// Normalized lookup key (NormalizeIdentifierKey) and the folded variant (ё = е).
std::string ScopeKey(const std::string& name);
std::string FoldedKey(const std::string& name);

// Builds a Table / Temp binding name set: an explicit alias addresses the binding alone; without an alias a
// dotted table name is addressable by its full name and by its last segment, a temp table by its name.
void SetBindingNames(Binding& b, const std::string& source_name, const std::string& alias);

// Resolves a field path (`parts` of an Expr::K::Field) in `scope`: alias or implicit alias first segment, or an
// unqualified field present in exactly one binding of the whole scope chain, followed by reference dereferences.
FieldLabel ResolveFieldPath(const std::vector<std::string>& parts, const Pos& pos, const Scope& scope,
                            const FieldPolicy& policy, const ResolveSink& sink);

}  // namespace lineage::strict
