#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Closed AST of the strict BSL parser. Every enum is an allowlist: there is no "unknown", "raw" or
// "passthrough" node. Whatever the parser cannot map onto one of these kinds is a ParseError. Default-constructed
// nodes have kind Unset, which ValidateBody rejects. The tree carries no comments and no token text beyond
// identifier spelling, string values and number/date digits.
//
// Member names (after a dot) may be spelled like a keyword ("Запрос.Выполнить()"); they are stored as ordinary
// names with their source spelling. Whether a member exists on the receiver is the allowlist's job.

namespace lineage::bsl {

struct Pos {
    size_t offset = 0;
    uint32_t line = 1;
    uint32_t col = 1;
};

struct Expr {
    enum class K {
        Unset,        // default-constructed node: never valid
        Lit,          // lit + text
        Var,          // text = identifier
        Member,       // args[0] = object; text = member name
        Index,        // args[0] = object; args[1] = index
        Call,         // global function call; text = function name; args = arguments
        MethodCall,   // args[0] = object; text = method name; args[1..] = arguments
        New,          // text = type name; has_parens; args = constructor arguments
        Ternary,      // ?(a, b, c): args[0..2]
        Unary,        // text = "-" | "НЕ"; args[0]
        Binary        // text = + - * / % = <> < > <= >= И ИЛИ; args[0], args[1]
    };
    enum class L { None, Number, String, Date, True, False, Undefined, Null };

    K kind = K::Unset;
    Pos pos;
    L lit = L::None;                         // Lit only
    std::string text;                        // Number: digits as written; String: decoded value; Date: digits
    std::vector<std::shared_ptr<Expr>> args;
    bool has_parens = false;                 // New only: "Новый Тип(...)" versus "Новый Тип"
};
using ExprP = std::shared_ptr<Expr>;

struct Stmt;
using StmtP = std::shared_ptr<Stmt>;

struct Body {
    std::vector<StmtP> stmts;
};

struct Stmt {
    enum class K {
        Unset,        // default-constructed node: never valid
        Assign,       // exprs[0] = lvalue (Var / Member / Index chain), exprs[1] = value
        CallStmt,     // exprs[0] = Call | MethodCall
        If,           // exprs = conditions; blocks = one per condition, plus the Иначе block when has_else
        ForEach,      // var; exprs[0] = collection; blocks[0]
        ForTo,        // var; exprs[0] = from, exprs[1] = to; blocks[0]
        While,        // exprs[0] = condition; blocks[0]
        Try,          // blocks[0] = Попытка body, blocks[1] = Исключение body
        Break,
        Continue
    };

    K kind = K::Unset;
    Pos pos;
    std::string var;                         // ForEach / ForTo loop variable
    std::vector<ExprP> exprs;
    std::vector<Body> blocks;
    bool has_else = false;                   // If only
};

struct ParseError {
    std::string code;                        // C_PARSE | C_EVAL | C_HANDLER | C_RAISE | C_DYNAMIC_NEW | C_LIMIT | C_INTERNAL
    Pos pos;
    std::string detail;                      // fixed text; never an echo of the input
};

}  // namespace lineage::bsl
