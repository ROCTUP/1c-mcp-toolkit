#include "bsl/bsl_parser.h"

namespace lineage::bsl {

namespace {

// Strings are dumped quoted with control characters, '"' and '\' escaped, so distinct values never collide.
void DumpString(const std::string& s, std::string& out) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    out += '"';
}

const char* LitName(Expr::L l) {
    switch (l) {
        case Expr::L::Number: return "num";
        case Expr::L::String: return "str";
        case Expr::L::Date: return "date";
        case Expr::L::True: return "true";
        case Expr::L::False: return "false";
        case Expr::L::Undefined: return "undef";
        case Expr::L::Null: return "null";
        case Expr::L::None: break;
    }
    return "?lit";
}

void DumpExprTo(const Expr& e, std::string& out);

void DumpExprs(const std::vector<ExprP>& v, size_t from, std::string& out) {
    for (size_t i = from; i < v.size(); ++i) {
        out += ' ';
        if (v[i]) DumpExprTo(*v[i], out);
        else out += "?null";
    }
}

void DumpExprTo(const Expr& e, std::string& out) {
    switch (e.kind) {
        case Expr::K::Lit:
            out += '(';
            out += LitName(e.lit);
            if (e.lit == Expr::L::String) {
                out += ' ';
                DumpString(e.text, out);
            } else if (!e.text.empty()) {
                out += ' ' + e.text;
            }
            out += ')';
            return;
        case Expr::K::Var: out += "(var " + e.text + ")"; return;
        case Expr::K::Member: out += "(member " + e.text; break;
        case Expr::K::Index: out += "(index"; break;
        case Expr::K::Call: out += "(call " + e.text; break;
        case Expr::K::MethodCall: out += "(mcall " + e.text; break;
        case Expr::K::New: out += "(new " + e.text + (e.has_parens ? " ()" : ""); break;
        case Expr::K::Ternary: out += "(ternary"; break;
        case Expr::K::Unary: out += "(unary " + e.text; break;
        case Expr::K::Binary: out += "(binary " + e.text; break;
        case Expr::K::Unset: out += "?unset"; return;
    }
    DumpExprs(e.args, 0, out);
    out += ')';
}

void DumpBodyTo(const Body& b, std::string& out);

void DumpStmtTo(const Stmt& s, std::string& out) {
    switch (s.kind) {
        case Stmt::K::Assign: out += "(assign"; break;
        case Stmt::K::CallStmt: out += "(callstmt"; break;
        case Stmt::K::If: out += s.has_else ? "(if+else" : "(if"; break;
        case Stmt::K::ForEach: out += "(foreach " + s.var; break;
        case Stmt::K::ForTo: out += "(forto " + s.var; break;
        case Stmt::K::While: out += "(while"; break;
        case Stmt::K::Try: out += "(try"; break;
        case Stmt::K::Break: out += "(break)"; return;
        case Stmt::K::Continue: out += "(continue)"; return;
        case Stmt::K::Unset: out += "?unset"; return;
    }
    DumpExprs(s.exprs, 0, out);
    for (const Body& blk : s.blocks) {
        out += ' ';
        DumpBodyTo(blk, out);
    }
    out += ')';
}

void DumpBodyTo(const Body& b, std::string& out) {
    out += "{";
    for (size_t i = 0; i < b.stmts.size(); ++i) {
        if (i != 0) out += ' ';
        if (b.stmts[i]) DumpStmtTo(*b.stmts[i], out);
        else out += "?null";
    }
    out += "}";
}

}  // namespace

std::string DumpBslExpr(const Expr& e) {
    std::string out;
    DumpExprTo(e, out);
    return out;
}

std::string DumpBsl(const Body& body) {
    std::string out;
    DumpBodyTo(body, out);
    return out;
}

}  // namespace lineage::bsl
