#include "bsl/bsl_canonical.h"

#include <set>
#include <utility>
#include <vector>

#include "bsl/bsl_parser.h"
#include "bsl/bsl_tokenizer.h"

namespace lineage::bsl {

namespace {

struct Refuse {
    const char* code;
    const char* detail;
};

bool IsMixedScript(const std::string& s) {
    bool latin = false;
    bool cyrillic = false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) latin = true;
        else if (c == 0xD0 || c == 0xD1) cyrillic = true;
    }
    return latin && cyrillic;
}

// `s` lexes as exactly one token of `kind` spelled exactly as `s` (nothing left over).
bool LexesAs(const std::string& s, Bt kind) {
    if (s.empty()) return false;
    const std::vector<Token> toks = Tokenize(s);
    return toks.size() == 2 && toks[0].kind == kind && toks[0].text == s && toks[1].kind == Bt::End &&
           !toks[0].mixed_script;
}

bool IsPlainIdent(const std::string& s) { return LexesAs(s, Bt::Identifier) && !IsMixedScript(s); }
bool IsMemberName(const std::string& s) {
    return (LexesAs(s, Bt::Identifier) || LexesAs(s, Bt::Keyword)) && !IsMixedScript(s);
}

// Strict UTF-8 decode of one code point; 0 on any malformation (overlong, surrogate, > U+10FFFF, truncated).
size_t Decode(const std::string& s, size_t i, uint32_t* out) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    size_t need = 0;
    uint32_t cp = 0;
    uint32_t minCp = 0;
    if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; minCp = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; minCp = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; minCp = 0x10000; }
    else return 0;
    if (i + need >= s.size()) return 0;
    for (size_t k = 1; k <= need; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    *out = cp;
    return need + 1;
}

bool IsSuspicious(uint32_t cp) {
    return (cp >= 0x80 && cp <= 0xA0) || cp == 0xAD || cp == 0x061C || cp == 0x180E ||
           (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
           (cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF || (cp >= 0xFFF0 && cp <= 0xFFFF) ||
           (cp >= 0xE0000 && cp <= 0xE0FFF) || cp == 0x034F || cp == 0x115F || cp == 0x1160 || cp == 0x1680 ||
           cp == 0x17B4 || cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180F) || cp == 0x2800 || cp == 0x3164 ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFFA0 || (cp >= 0x1D173 && cp <= 0x1D17A);
}

// A string value may hold plain text, TAB and LF. Nothing else that is invisible or ambiguous.
bool IsSafeStringValue(const std::string& v) {
    for (size_t i = 0; i < v.size();) {
        uint32_t cp = 0;
        const size_t n = Decode(v, i, &cp);
        if (n == 0) return false;
        if (cp < 0x20 && cp != '\t' && cp != '\n') return false;
        if (cp == 0x7F || IsSuspicious(cp)) return false;
        i += n;
    }
    return true;
}

bool IsDigits(const std::string& s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// ---- replacements: build the effective tree ----

ExprP MkExpr(Expr::K kind, std::string text, std::vector<ExprP> args = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    e->text = std::move(text);
    e->args = std::move(args);
    return e;
}

ExprP MkLit(Expr::L lit, std::string text) {
    auto e = std::make_shared<Expr>();
    e->kind = Expr::K::Lit;
    e->lit = lit;
    e->text = std::move(text);
    return e;
}

// Balanced "+" tree over parts[lo, hi): depth is log2(n), so long queries stay inside the parser's limits.
ExprP Balanced(const std::vector<ExprP>& parts, size_t lo, size_t hi) {
    if (hi - lo == 1) return parts[lo];
    const size_t mid = lo + (hi - lo) / 2;
    return MkExpr(Expr::K::Binary, "+", {Balanced(parts, lo, mid), Balanced(parts, mid, hi)});
}

// Query text -> "line" + Символы.ПС + "line" ...; empty text -> "".
ExprP QueryTextExpr(const std::string& text) {
    if (!IsSafeStringValue(text)) throw Refuse{"C_PRINT_STRING", "query text"};
    std::vector<ExprP> parts;
    size_t start = 0;
    for (;;) {
        const size_t nl = text.find('\n', start);
        const std::string seg = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!seg.empty()) parts.push_back(MkLit(Expr::L::String, seg));
        if (nl == std::string::npos) break;
        parts.push_back(MkExpr(Expr::K::Member, "ПС", {MkExpr(Expr::K::Var, "Символы")}));
        start = nl + 1;
    }
    if (parts.empty()) return MkLit(Expr::L::String, "");
    return Balanced(parts, 0, parts.size());
}

class Rewriter {
public:
    explicit Rewriter(const PrintOptions& o) : o_(o) {}

    Body Run(const Body& b) {
        for (const auto& kv : o_.query_replacements) {
            if (o_.bound_tokens.count(kv.first) != 0) throw Refuse{"C_PRINT_REPLACEMENT", "node replaced twice"};
        }
        Body out = Rewrite(b);
        if (used_.size() != o_.query_replacements.size() + o_.bound_tokens.size()) {
            throw Refuse{"C_PRINT_REPLACEMENT", "replacement target not in the tree"};
        }
        return out;
    }

private:
    ExprP Rewrite(const ExprP& e) {
        const auto q = o_.query_replacements.find(e.get());
        if (q != o_.query_replacements.end()) {
            used_.insert(e.get());
            return QueryTextExpr(q->second);
        }
        const auto t = o_.bound_tokens.find(e.get());
        if (t != o_.bound_tokens.end()) {
            used_.insert(e.get());
            if (t->second < 0) throw Refuse{"C_PRINT_REPLACEMENT", "negative token index"};
            return MkExpr(Expr::K::Index, "",
                          {MkExpr(Expr::K::Var, "МСПЗначенияТокенов"), MkLit(Expr::L::Number, std::to_string(t->second))});
        }
        std::vector<ExprP> args;
        bool changed = false;
        for (const ExprP& a : e->args) {
            args.push_back(Rewrite(a));
            changed = changed || args.back() != a;
        }
        if (!changed) return e;
        auto c = std::make_shared<Expr>(*e);
        c->args = std::move(args);
        return c;
    }

    // A replaced node must not be part of an assignment target (the name or the member / index chain).
    void CheckTarget(const Expr* e) const {
        while (e) {
            if (o_.query_replacements.count(e) != 0 || o_.bound_tokens.count(e) != 0) {
                throw Refuse{"C_PRINT_REPLACEMENT", "replacement in an assignment target"};
            }
            if ((e->kind != Expr::K::Member && e->kind != Expr::K::Index) || e->args.empty()) return;
            e = e->args[0].get();
        }
    }

    Body Rewrite(const Body& b) {
        Body out;
        for (const StmtP& s : b.stmts) {
            if (s->kind == Stmt::K::Assign && !s->exprs.empty()) CheckTarget(s->exprs[0].get());
            auto c = std::make_shared<Stmt>(*s);
            c->exprs.clear();
            c->blocks.clear();
            for (const ExprP& x : s->exprs) c->exprs.push_back(Rewrite(x));
            for (const Body& blk : s->blocks) c->blocks.push_back(Rewrite(blk));
            out.stmts.push_back(std::move(c));
        }
        return out;
    }

    const PrintOptions& o_;
    std::set<const Expr*> used_;
};

// ---- the printer ----

class Printer {
public:
    std::string Run(const Body& b) {
        out_.clear();
        depth_ = 0;
        PrintBody(b, 0);
        return out_;
    }

private:
    void W(const std::string& s) {
        if (out_.size() + s.size() > kMaxCanonicalBytes) throw Refuse{"C_PRINT_LIMIT", "text too large"};
        out_ += s;
    }
    struct Guard {
        Printer& p;
        explicit Guard(Printer& pp) : p(pp) {
            if (++p.depth_ > kMaxPrintDepth) throw Refuse{"C_PRINT_LIMIT", "nesting too deep"};
        }
        ~Guard() { --p.depth_; }
    };

    void Indent(int level) { W(std::string(static_cast<size_t>(level) * 4, ' ')); }

    void Name(const std::string& s, bool member) {
        if (!(member ? IsMemberName(s) : IsPlainIdent(s))) throw Refuse{"C_PRINT_IDENT", "identifier"};
        W(s);
    }

    void StringLiteral(const std::string& value) {
        if (value.size() >= kMaxCanonicalBytes) throw Refuse{"C_PRINT_LIMIT", "text too large"};
        if (!IsSafeStringValue(value)) throw Refuse{"C_PRINT_STRING", "string literal"};
        std::string lit = "\"";
        for (const char c : value) {
            if (c == '"') lit += "\"\"";
            else if (c == '\n') lit += "\n|";
            else lit += c;
        }
        lit += "\"";
        const std::vector<Token> toks = Tokenize(lit);
        if (toks.size() != 2 || toks[0].kind != Bt::String || toks[0].value != value || toks[1].kind != Bt::End) {
            throw Refuse{"C_PRINT_STRING", "string literal"};
        }
        W(lit);
    }

    void Literal(const Expr& e) {
        switch (e.lit) {
            case Expr::L::Number: {
                const std::vector<Token> t = Tokenize(e.text);
                if (t.size() != 2 || t[0].kind != Bt::Number || t[0].text != e.text) throw Refuse{"C_PRINT_NUMBER", "number"};
                W(e.text);
                return;
            }
            case Expr::L::Date: {
                if (!IsDigits(e.text)) throw Refuse{"C_PRINT_DATE", "date"};
                const std::string lit = "'" + e.text + "'";
                const std::vector<Token> t = Tokenize(lit);
                if (t.size() != 2 || t[0].kind != Bt::Date || t[0].value != e.text) throw Refuse{"C_PRINT_DATE", "date"};
                W(lit);
                return;
            }
            case Expr::L::String: StringLiteral(e.text); return;
            case Expr::L::True: W("Истина"); return;
            case Expr::L::False: W("Ложь"); return;
            case Expr::L::Undefined: W("Неопределено"); return;
            case Expr::L::Null: W("NULL"); return;
            case Expr::L::None: break;
        }
        throw Refuse{"C_PRINT_NODE", "literal kind"};
    }

    static bool IsAtom(const Expr& e) {
        switch (e.kind) {
            case Expr::K::Lit: case Expr::K::Var: case Expr::K::Member: case Expr::K::Index: case Expr::K::Call:
            case Expr::K::MethodCall: case Expr::K::New: case Expr::K::Ternary:
                return true;
            default:
                return false;
        }
    }
    // Receiver of '.', '[' and method calls: only forms the parser accepts before a postfix operator unwrapped.
    // "Новый Запрос(...)" is accepted before '.' only; before '[' the re-parse refuses it (fail closed).
    static bool IsPostfixBase(const Expr& e) {
        switch (e.kind) {
            case Expr::K::Var: case Expr::K::Member: case Expr::K::Index: case Expr::K::Call:
            case Expr::K::MethodCall: case Expr::K::Ternary:
                return true;
            case Expr::K::New:
                return IsChainableNew(e);
            default:
                return false;
        }
    }

    static int Level(const std::string& op) {
        if (op == "ИЛИ") return 1;
        if (op == "И") return 2;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 4;   // comparison (validated operators only)
    }

    const Expr& Child(const ExprP& e) {
        if (!e) throw Refuse{"C_PRINT_NULL", "null child"};
        return *e;
    }
    void Wrapped(const Expr& e, bool wrap) {
        if (wrap) W("(");
        Print(e);
        if (wrap) W(")");
    }
    void Args(const std::vector<ExprP>& v, size_t from) {
        for (size_t i = from; i < v.size(); ++i) {
            if (i != from) W(", ");
            Print(Child(v[i]));
        }
    }
    void Arity(const Expr& e, size_t n) {
        if (e.args.size() != n) throw Refuse{"C_PRINT_NODE", "arity"};
    }

    void Print(const Expr& e) {
        Guard g(*this);
        switch (e.kind) {
            case Expr::K::Lit: Literal(e); return;
            case Expr::K::Var: Arity(e, 0); Name(e.text, false); return;
            case Expr::K::Member:
                Arity(e, 1);
                Wrapped(Child(e.args[0]), !IsPostfixBase(Child(e.args[0])));
                W(".");
                Name(e.text, true);
                return;
            case Expr::K::Index:
                Arity(e, 2);
                Wrapped(Child(e.args[0]), !IsPostfixBase(Child(e.args[0])));
                W("[");
                Print(Child(e.args[1]));
                W("]");
                return;
            case Expr::K::Call:
                Name(e.text, false);
                W("(");
                Args(e.args, 0);
                W(")");
                return;
            case Expr::K::MethodCall:
                if (e.args.empty()) throw Refuse{"C_PRINT_NODE", "arity"};
                Wrapped(Child(e.args[0]), !IsPostfixBase(Child(e.args[0])));
                W(".");
                Name(e.text, true);
                W("(");
                Args(e.args, 1);
                W(")");
                return;
            case Expr::K::New:
                W("Новый ");
                Name(e.text, false);
                if (e.has_parens) {
                    W("(");
                    Args(e.args, 0);
                    W(")");
                } else if (!e.args.empty()) {
                    throw Refuse{"C_PRINT_NODE", "new without parentheses"};
                }
                return;
            case Expr::K::Ternary:
                Arity(e, 3);
                W("?(");
                Args(e.args, 0);
                W(")");
                return;
            case Expr::K::Unary:
                Arity(e, 1);
                if (e.text == "-") W("-");
                else if (e.text == "НЕ") W("НЕ ");
                else throw Refuse{"C_PRINT_NODE", "unary operator"};
                Wrapped(Child(e.args[0]), !IsAtom(Child(e.args[0])));
                return;
            case Expr::K::Binary: PrintBinary(e); return;
            case Expr::K::Unset: break;
        }
        throw Refuse{"C_PRINT_NODE", "unset node"};
    }

    void PrintBinary(const Expr& e) {
        Arity(e, 2);
        static const char* const ops[] = {"+", "-", "*", "/", "%", "=", "<>", "<", ">", "<=", ">=", "И", "ИЛИ"};
        bool known = false;
        for (const char* o : ops) known = known || e.text == o;
        if (!known) throw Refuse{"C_PRINT_NODE", "binary operator"};
        const int level = Level(e.text);
        const Expr& l = Child(e.args[0]);
        const Expr& r = Child(e.args[1]);
        const bool chain = l.kind == Expr::K::Binary && level != 4 && Level(l.text) == level;
        Wrapped(l, !chain && !IsAtom(l));
        W(" " + e.text + " ");
        Wrapped(r, !IsAtom(r));
    }

    void Block(const Body& b, int level) {
        Guard g(*this);
        PrintBody(b, level);
    }

    void PrintBody(const Body& b, int level) {
        for (const StmtP& sp : b.stmts) {
            if (!sp) throw Refuse{"C_PRINT_NULL", "null statement"};
            PrintStmt(*sp, level);
        }
    }

    void Expect(const Stmt& s, size_t ne, size_t nb) {
        if (s.exprs.size() != ne || s.blocks.size() != nb) throw Refuse{"C_PRINT_NODE", "statement shape"};
    }

    void PrintStmt(const Stmt& s, int level) {
        Indent(level);
        switch (s.kind) {
            case Stmt::K::Assign:
                Expect(s, 2, 0);
                Print(Child(s.exprs[0]));
                W(" = ");
                Print(Child(s.exprs[1]));
                W(";\n");
                return;
            case Stmt::K::CallStmt:
                Expect(s, 1, 0);
                Print(Child(s.exprs[0]));
                W(";\n");
                return;
            case Stmt::K::If: PrintIf(s, level); return;
            case Stmt::K::ForEach:
                Expect(s, 1, 1);
                W("Для Каждого ");
                Name(s.var, false);
                W(" Из ");
                Print(Child(s.exprs[0]));
                W(" Цикл\n");
                Block(s.blocks[0], level + 1);
                Indent(level);
                W("КонецЦикла;\n");
                return;
            case Stmt::K::ForTo:
                Expect(s, 2, 1);
                W("Для ");
                Name(s.var, false);
                W(" = ");
                Print(Child(s.exprs[0]));
                W(" По ");
                Print(Child(s.exprs[1]));
                W(" Цикл\n");
                Block(s.blocks[0], level + 1);
                Indent(level);
                W("КонецЦикла;\n");
                return;
            case Stmt::K::While:
                Expect(s, 1, 1);
                W("Пока ");
                Print(Child(s.exprs[0]));
                W(" Цикл\n");
                Block(s.blocks[0], level + 1);
                Indent(level);
                W("КонецЦикла;\n");
                return;
            case Stmt::K::Try:
                Expect(s, 0, 2);
                W("Попытка\n");
                Block(s.blocks[0], level + 1);
                Indent(level);
                W("Исключение\n");
                Block(s.blocks[1], level + 1);
                Indent(level);
                W("КонецПопытки;\n");
                return;
            case Stmt::K::Break: Expect(s, 0, 0); W("Прервать;\n"); return;
            case Stmt::K::Continue: Expect(s, 0, 0); W("Продолжить;\n"); return;
            case Stmt::K::Unset: break;
        }
        throw Refuse{"C_PRINT_NODE", "unset statement"};
    }

    void PrintIf(const Stmt& s, int level) {
        const size_t nc = s.exprs.size();
        if (nc == 0 || s.blocks.size() != nc + (s.has_else ? 1u : 0u)) throw Refuse{"C_PRINT_NODE", "statement shape"};
        for (size_t i = 0; i < nc; ++i) {
            if (i != 0) Indent(level);
            W(i == 0 ? "Если " : "ИначеЕсли ");
            Print(Child(s.exprs[i]));
            W(" Тогда\n");
            Block(s.blocks[i], level + 1);
        }
        if (s.has_else) {
            Indent(level);
            W("Иначе\n");
            Block(s.blocks[nc], level + 1);
        }
        Indent(level);
        W("КонецЕсли;\n");
    }

    std::string out_;
    int depth_ = 0;
};

PrintResult Refused(const char* code, const char* detail) {
    PrintResult r;
    r.error = ParseError{code, Pos{}, detail};
    return r;
}

}  // namespace

PrintResult PrintBslCanonicalChecked(const Body& body, const PrintOptions& options) {
    ParseError verr;
    try {
        if (!ValidateBody(body, verr)) {
            if (verr.code == "C_RESERVED") return Refused("C_PRINT_RESERVED", "reserved name");
            PrintResult r;
            r.error = verr;
            return r;
        }
        const Body effective = Rewriter(options).Run(body);
        // The printer's own nodes use the reserved names; client names were already checked above.
        if (!ValidateBody(effective, verr, true)) {
            PrintResult r;
            r.error = verr;
            return r;
        }
        std::string text = Printer().Run(effective);

        // The text that will be executed must parse again to the very same tree and print to itself.
        const ParseResult again = ParseBsl(text, true);
        if (!again.ok) return Refused("C_PRINT_REPARSE", "canonical text does not parse");
        if (DumpBsl(again.body) != DumpBsl(effective)) return Refused("C_PRINT_MISMATCH", "re-parse differs");
        if (Printer().Run(again.body) != text) return Refused("C_PRINT_MISMATCH", "print is not idempotent");

        PrintResult r;
        r.ok = true;
        r.text = std::move(text);
        return r;
    } catch (const Refuse& f) {
        return Refused(f.code, f.detail);
    } catch (...) {
        return Refused("C_PRINT_INTERNAL", "internal error");
    }
}

std::string PrintBslCanonical(const Body& body, const PrintOptions& options) {
    PrintResult r = PrintBslCanonicalChecked(body, options);
    return r.ok ? std::move(r.text) : std::string();
}

}  // namespace lineage::bsl
