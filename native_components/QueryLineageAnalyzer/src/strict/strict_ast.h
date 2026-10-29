#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Closed AST of the strict query parser. Every enum below is an allowlist: there is deliberately no "unknown",
// "raw" or "passthrough" node. Whatever the parser cannot map onto one of these kinds is a ParseError, so
// downstream code (checker, canonical printer) can switch over the kinds exhaustively and treat a missing
// case as a bug. The tree carries no comments and no original token text beyond identifier spelling.
//
// Parser responsibilities that are NOT encoded here (see strict_tokenizer.h): identifiers that mix Latin and
// Cyrillic letters are rejected by the parser, and the two-word form "В ИЕРАРХИИ" arrives as separate tokens
// and becomes Expr::K::InHierarchy.

namespace lineage::strict {

struct Pos {
    size_t offset = 0;
    uint32_t line = 1;
    uint32_t col = 1;
};

struct Select;  // forward declaration (subqueries)

struct Expr {
    enum class K {
        Unset,        // default-constructed node: never valid, rejected by ValidateExpr (fail closed)
        Field,        // path: parts (original spelling)
        Param,        // text = parameter name
        StringLit,    // text = unescaped value
        NumberLit,    // text = digits as written
        BoolLit,      // text = "ИСТИНА" | "ЛОЖЬ"
        NullLit,
        UndefinedLit,
        Unary,        // text = "-" | "НЕ"; args[0]
        Binary,       // text = + - * / = <> < > <= >= И ИЛИ; args[0], args[1]
        Call,         // text = normalized function name; args; distinct for КОЛИЧЕСТВО(РАЗЛИЧНЫЕ x)
        Case,         // args: [operand if has_operand] then (when, then)* then [else if has_else]
        Cast,         // args[0] КАК type_name (+ type_args)
        InList,       // args[0] В (args[1..]); negated
        InSubquery,   // args[0] В (subquery); negated; args[0] may be a Row
        InHierarchy,  // args[0] В ИЕРАРХИИ (args[1..]) or (subquery); negated
        Between,      // args[0] МЕЖДУ args[1] И args[2]; negated
        Like,         // args[0] ПОДОБНО args[1] [СПЕЦСИМВОЛ args[2]]; negated
        IsNullTest,   // args[0] ЕСТЬ [НЕ] NULL; negated = "НЕ" present
        RefsTest,     // args[0] ССЫЛКА type_name; negated
        ValueFn,      // ЗНАЧЕНИЕ(type_name)
        TypeFn,       // ТИП(type_name)
        Row           // (a, b, ...) for multi-column В
    };

    K kind = K::Unset;
    Pos pos;
    std::string text;                        // literal value / operator / function name / parameter name
    std::vector<std::string> parts;          // Field path, original spelling
    std::string type_name;                   // Cast / RefsTest / ValueFn / TypeFn (dotted)
    std::vector<std::string> type_args;      // Cast: length / precision digits
    std::vector<std::shared_ptr<Expr>> args;
    std::shared_ptr<Select> subquery;        // InSubquery / InHierarchy (subquery form)
    bool negated = false;
    bool distinct = false;
    bool has_operand = false;                // Case: simple form ВЫБОР x КОГДА ...
    bool has_else = false;                   // Case: trailing ИНАЧЕ branch present
};
using ExprP = std::shared_ptr<Expr>;

struct SelectItem {
    ExprP expr;                              // null only when wildcard
    std::string alias;
    bool alias_explicit = false;
    bool wildcard = false;                   // "*" or "Name.*"
    std::vector<std::string> wildcard_prefix;
    Pos pos;
};

struct Source {
    enum class K { Table, VirtualTable, Subquery } kind = K::Table;
    std::string name;                        // "Справочник.Контрагенты" / temp table name
    std::vector<ExprP> vt_params;            // virtual table parameters; empty slots are nullptr
    std::shared_ptr<Select> subquery;
    std::string alias;
    Pos pos;
};

struct Join {
    enum class K { Inner, Left, Right, Full } kind = K::Inner;
    Source source;
    ExprP on;
    Pos pos;
};

struct OrderItem {
    ExprP expr;
    bool desc = false;
};

struct TotalsItem {
    ExprP expr;                              // null when overall
    bool overall = false;                    // ОБЩИЕ
    bool hierarchy = false;
    bool only_hierarchy = false;
    bool periods = false;                    // ПЕРИОДАМИ(...): parsed so the analyzer can reject it explicitly
    std::vector<ExprP> periods_args;
    std::string alias;
};

struct Select {
    bool allowed = false;
    bool distinct = false;
    std::string top;                         // digits or empty
    std::vector<SelectItem> items;
    std::string into;                        // ПОМЕСТИТЬ
    std::vector<Source> from;                // comma-separated sources
    std::vector<std::vector<Join>> joins;    // joins[i] attached to from[i]
    ExprP where;
    std::vector<ExprP> group_by;
    ExprP having;
    bool for_update = false;
    std::vector<std::string> for_update_tables;
    std::vector<ExprP> index_by;

    struct UnionPart {
        bool all = false;
        std::shared_ptr<Select> select;
    };
    std::vector<UnionPart> unions;

    // ORDER BY / AUTOORDER / TOTALS belong to the whole (union) statement:
    std::vector<OrderItem> order_by;
    bool autoorder = false;
    bool has_totals = false;
    std::vector<ExprP> totals_aggregates;
    bool totals_overall = false;
    std::vector<TotalsItem> totals_by;
    Pos pos;
};

struct Statement {
    enum class K { Select, Drop } kind = K::Select;
    std::shared_ptr<Select> select;          // K::Select
    std::string drop_name;                   // K::Drop
    Pos pos;
};

struct Batch {
    std::vector<Statement> statements;
};

struct ParseError {
    std::string code;                        // Q_PARSE_*
    Pos pos;
    std::string detail;
};

}  // namespace lineage::strict
