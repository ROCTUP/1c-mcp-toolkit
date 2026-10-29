#include "bsl/bsl_parser.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bsl/bsl_reserved.h"
#include "bsl/bsl_tokenizer.h"
#include "utf_utils.h"

namespace lineage::bsl {

namespace {

struct Fail {
    ParseError e;
};

bool IsLatin(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool IsMixedScript(const std::string& s) {
    bool latin = false;
    bool cyrillic = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (IsLatin(c)) {
            latin = true;
        } else if (c == 0xD0 || c == 0xD1) {
            cyrillic = true;
            ++i;
        }
    }
    return latin && cyrillic;
}

// Length of the identifier character at s[i] (1 or 2 bytes), 0 when it is not one. The tokenizer's charset.
size_t IdentCharLen(const std::string& s, size_t i, bool first) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (IsLatin(c) || c == '_') return 1;
    if (!first && c >= '0' && c <= '9') return 1;
    if (i + 1 < s.size()) {
        const unsigned char b = static_cast<unsigned char>(s[i + 1]);
        if ((c == 0xD0 && (b == 0x81 || (b >= 0x90 && b <= 0xBF))) || (c == 0xD1 && ((b >= 0x80 && b <= 0x8F) || b == 0x91))) {
            return 2;
        }
    }
    return 0;
}

bool IsValidIdent(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size();) {
        const size_t n = IdentCharLen(s, i, i == 0);
        if (n == 0) return false;
        i += n;
    }
    return !IsMixedScript(s);
}

bool AllDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool IsNumberText(const std::string& s) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return AllDigits(s);
    return AllDigits(s.substr(0, dot)) && AllDigits(s.substr(dot + 1));
}

bool IsEvalName(const std::string& text) {
    const std::string key = NormalizeIdentifierKey(text);
    return key == NormalizeIdentifierKey("Вычислить") || key == "EVAL";
}

// A plain name: one identifier token (not a keyword, yo-folded keywords included, no script mixing) that is not
// the name of a dynamic-evaluation primitive. Used for variables, global callees, type names of "Новый" and loop
// variables; member names after a dot only need IsValidIdent.
bool IsPlainName(const std::string& s) {
    if (!IsValidIdent(s) || IsEvalName(s)) return false;
    const std::vector<Token> t = Tokenize(s);
    return t.size() == 2 && t[0].kind == Bt::Identifier && !t[0].mixed_script && t[0].text == s && t[1].kind == Bt::End;
}

bool IsBlockTerminator(Bk kw) {
    return kw == Bk::EndIf || kw == Bk::Else || kw == Bk::ElsIf || kw == Bk::EndDo || kw == Bk::Except ||
           kw == Bk::EndTry;
}

class P {
public:
    P(std::vector<Token> t, bool allow_reserved) : t_(std::move(t)), allow_reserved_(allow_reserved) {}

    // Any token the tokenizer could not classify rejects the whole text before any grammar decision.
    void Prescan() const {
        for (const Token& t : t_) {
            if (t.kind != Bt::Unknown) continue;
            const bool limit = t.text == "<size>" || t.text == "<limit>";
            throw Fail{{limit ? "C_LIMIT" : "C_PARSE", {t.pos, t.line, t.col}, limit ? "source too large" : "unsupported token"}};
        }
    }

    Body ParseProgram() {
        Body b;
        ParseBlock(b, {});
        return b;
    }

private:
    // ---- token helpers ----
    const Token& Peek(size_t k = 0) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
    bool IsKw(Bk kw, size_t k = 0) const { return Peek(k).kind == Bt::Keyword && Peek(k).kw == kw; }
    bool IsOp(const char* op) const { return Peek().kind == Bt::Op && Peek().text == op; }
    const Token& Next() { return t_[i_ < t_.size() - 1 ? i_++ : i_]; }
    bool Accept(Bt k) {
        if (Peek().kind != k) return false;
        ++i_;
        return true;
    }
    bool AcceptKw(Bk kw) {
        if (!IsKw(kw)) return false;
        ++i_;
        return true;
    }
    void Expect(Bt k) {
        if (!Accept(k)) Error("C_PARSE", "unexpected token");
    }
    void ExpectKw(Bk kw) {
        if (!AcceptKw(kw)) Error("C_PARSE", "expected keyword");
    }
    Pos Here() const { return {Peek().pos, Peek().line, Peek().col}; }
    [[noreturn]] void Error(const char* code, const char* detail) const {
        const Token& tok = Peek();
        throw Fail{{code, {tok.pos, tok.line, tok.col}, detail}};
    }

    struct Guard {
        P& p;
        explicit Guard(P& pp) : p(pp) {
            if (++p.depth_ > kMaxParseDepth) p.Error("C_LIMIT", "nesting too deep");
        }
        ~Guard() { --p.depth_; }
    };

    // ---- nodes: created with a concrete kind and sealed (tree depth accounted) ----
    ExprP Node(Expr::K kind, Pos pos, std::vector<ExprP> args = {}) {
        auto e = std::make_shared<Expr>();
        e->kind = kind;
        e->pos = pos;
        e->args = std::move(args);
        return e;
    }
    ExprP Seal(ExprP e) {
        int d = 0;
        for (const ExprP& a : e->args) {
            const auto it = depth_of_.find(a.get());
            if (it == depth_of_.end()) Error("C_INTERNAL", "unsealed child");
            d = std::max(d, it->second);
        }
        ++d;
        if (d > kMaxExprDepth) Error("C_LIMIT", "expression too deep");
        depth_of_[e.get()] = d;
        return e;
    }
    ExprP Make(Expr::K kind, Pos pos, std::string text, std::vector<ExprP> args = {}) {
        ExprP e = Node(kind, pos, std::move(args));
        e->text = std::move(text);
        return Seal(std::move(e));
    }
    ExprP Literal(Expr::L lit, Pos pos, std::string text) {
        ExprP e = Node(Expr::K::Lit, pos);
        e->lit = lit;
        e->text = std::move(text);
        return Seal(std::move(e));
    }

    // ---- keywords the grammar does not list ----
    [[noreturn]] void RejectKeyword(Bk kw) const {
        switch (kw) {
            case Bk::Execute: Error("C_EVAL", "dynamic code execution is not allowed");
            case Bk::AddHandler:
            case Bk::RemoveHandler: Error("C_HANDLER", "event handlers are not allowed");
            default: Error("C_PARSE", "construct is not allowed");
        }
    }

    // A name that is accepted as an identifier (not a keyword, not mixed-script).
    std::string TakeIdent() {
        const Token& t = Peek();
        if (t.kind == Bt::Keyword) RejectKeyword(t.kw);
        if (t.kind != Bt::Identifier) Error("C_PARSE", "identifier expected");
        if (t.mixed_script || !IsValidIdent(t.text)) Error("C_PARSE", "invalid identifier");
        if (IsEvalName(t.text)) Error("C_EVAL", "dynamic evaluation is not allowed");
        if (!allow_reserved_ && IsReservedName(t.text)) Error("C_RESERVED", "reserved name");
        return Next().text;
    }

    // ---- statements ----
    void ParseBlock(Body& body, std::initializer_list<Bk> stops) {
        for (;;) {
            const Token& t = Peek();
            if (t.kind == Bt::End) {
                if (stops.size() != 0) Error("C_PARSE", "unterminated block");
                return;
            }
            if (t.kind == Bt::Keyword && std::find(stops.begin(), stops.end(), t.kw) != stops.end()) return;
            body.stmts.push_back(ParseStatement());
        }
    }

    StmtP NewStmt(Stmt::K kind, Pos pos) {
        if (++stmt_count_ > kMaxStatements) Error("C_LIMIT", "too many statements");
        auto s = std::make_shared<Stmt>();
        s->kind = kind;
        s->pos = pos;
        return s;
    }

    // Simple statements end with ';', which may be omitted only right before a block terminator or the end.
    void EndSimple() {
        if (Accept(Bt::Semicolon)) return;
        const Token& t = Peek();
        if (t.kind == Bt::End || (t.kind == Bt::Keyword && IsBlockTerminator(t.kw))) return;
        Error("C_PARSE", "';' expected");
    }
    void EndCompound() { Accept(Bt::Semicolon); }

    // Counts nested compound statements (Если / Для / Пока / Попытка); leaf statements do not count.
    struct BlockGuard {
        P& p;
        explicit BlockGuard(P& pp) : p(pp) {
            if (++p.block_depth_ > kMaxBlockDepth) p.Error("C_LIMIT", "blocks nested too deep");
        }
        ~BlockGuard() { --p.block_depth_; }
    };

    StmtP ParseStatement() {
        const Token& t = Peek();
        if (t.kind == Bt::Identifier) return ParseSimple();
        if (t.kind != Bt::Keyword) Error("C_PARSE", "statement expected");
        switch (t.kw) {
            case Bk::If: { BlockGuard bg(*this); return ParseIf(); }
            case Bk::For: { BlockGuard bg(*this); return ParseFor(); }
            case Bk::While: { BlockGuard bg(*this); return ParseWhile(); }
            case Bk::Try: { BlockGuard bg(*this); return ParseTry(); }
            case Bk::Break:
            case Bk::Continue: {
                const bool brk = t.kw == Bk::Break;
                if (loop_depth_ == 0) Error("C_PARSE", "loop control outside a loop");
                StmtP s = NewStmt(brk ? Stmt::K::Break : Stmt::K::Continue, Here());
                Next();
                EndSimple();
                return s;
            }
            case Bk::Raise:
                if (Peek(1).kind == Bt::Semicolon || Peek(1).kind == Bt::End) Error("C_PARSE", "construct is not allowed");
                Error("C_RAISE", "raising an exception is not allowed");
            default: RejectKeyword(t.kw);
        }
    }

    StmtP ParseSimple() {
        const Pos pos = Here();
        ExprP lhs = Postfix();
        if (IsOp("=")) {
            if (!IsLvalue(*lhs)) Error("C_PARSE", "invalid assignment target");
            Next();
            StmtP s = NewStmt(Stmt::K::Assign, pos);
            s->exprs = {lhs, Expression()};
            EndSimple();
            return s;
        }
        if (lhs->kind != Expr::K::Call && lhs->kind != Expr::K::MethodCall) Error("C_PARSE", "call or assignment expected");
        StmtP s = NewStmt(Stmt::K::CallStmt, pos);
        s->exprs = {lhs};
        EndSimple();
        return s;
    }

    static bool IsLvalue(const Expr& e) {
        const Expr* cur = &e;
        while (cur->kind == Expr::K::Member || cur->kind == Expr::K::Index) cur = cur->args[0].get();
        return cur->kind == Expr::K::Var && (e.kind == Expr::K::Var || e.kind == Expr::K::Member || e.kind == Expr::K::Index);
    }

    StmtP ParseIf() {
        StmtP s = NewStmt(Stmt::K::If, Here());
        Next();
        for (;;) {
            s->exprs.push_back(Expression());
            ExpectKw(Bk::Then);
            s->blocks.emplace_back();
            ParseBlock(s->blocks.back(), {Bk::ElsIf, Bk::Else, Bk::EndIf});
            if (AcceptKw(Bk::ElsIf)) continue;
            if (AcceptKw(Bk::Else)) {
                s->has_else = true;
                s->blocks.emplace_back();
                ParseBlock(s->blocks.back(), {Bk::EndIf});
            }
            break;
        }
        ExpectKw(Bk::EndIf);
        EndCompound();
        return s;
    }

    StmtP ParseLoopBody(StmtP s) {
        ExpectKw(Bk::Do);
        s->blocks.emplace_back();
        ++loop_depth_;
        ParseBlock(s->blocks.back(), {Bk::EndDo});
        --loop_depth_;
        ExpectKw(Bk::EndDo);
        EndCompound();
        return s;
    }

    StmtP ParseFor() {
        const Pos pos = Here();
        Next();
        if (AcceptKw(Bk::Each)) {
            StmtP s = NewStmt(Stmt::K::ForEach, pos);
            s->var = TakeIdent();
            ExpectKw(Bk::In);
            s->exprs.push_back(Expression());
            return ParseLoopBody(std::move(s));
        }
        StmtP s = NewStmt(Stmt::K::ForTo, pos);
        s->var = TakeIdent();
        if (!IsOp("=")) Error("C_PARSE", "'=' expected");
        Next();
        s->exprs.push_back(Expression());
        ExpectKw(Bk::To);
        s->exprs.push_back(Expression());
        return ParseLoopBody(std::move(s));
    }

    StmtP ParseWhile() {
        StmtP s = NewStmt(Stmt::K::While, Here());
        Next();
        s->exprs.push_back(Expression());
        return ParseLoopBody(std::move(s));
    }

    StmtP ParseTry() {
        StmtP s = NewStmt(Stmt::K::Try, Here());
        Next();
        s->blocks.emplace_back();
        ParseBlock(s->blocks.back(), {Bk::Except});
        ExpectKw(Bk::Except);
        s->blocks.emplace_back();
        ParseBlock(s->blocks.back(), {Bk::EndTry});
        ExpectKw(Bk::EndTry);
        EndCompound();
        return s;
    }

    // ---- expressions: Или < И < Не < comparison < additive < multiplicative < unary < postfix ----
    ExprP Expression() {
        Guard g(*this);
        return Or();
    }
    ExprP Bin(std::string op, ExprP l, ExprP r) {
        const Pos pos = l->pos;
        return Make(Expr::K::Binary, pos, std::move(op), {std::move(l), std::move(r)});
    }
    ExprP Or() {
        ExprP l = And();
        while (IsKw(Bk::Or)) {
            Next();
            l = Bin("ИЛИ", std::move(l), And());
        }
        return l;
    }
    ExprP And() {
        ExprP l = NotExpr();
        while (IsKw(Bk::And)) {
            Next();
            l = Bin("И", std::move(l), NotExpr());
        }
        return l;
    }
    ExprP NotExpr() {
        if (IsKw(Bk::Not)) {
            Guard g(*this);
            const Pos pos = Here();
            Next();
            return Make(Expr::K::Unary, pos, "НЕ", {NotExpr()});
        }
        return Comparison();
    }
    ExprP Comparison() {
        ExprP l = Additive();
        const Token& t = Peek();
        if (t.kind == Bt::Op && (t.text == "=" || t.text == "<>" || t.text == "<" || t.text == ">" || t.text == "<=" || t.text == ">=")) {
            std::string op = Next().text;
            l = Bin(std::move(op), std::move(l), Additive());
        }
        return l;
    }
    ExprP Additive() {
        ExprP l = Multiplicative();
        while (IsOp("+") || IsOp("-")) {
            std::string op = Next().text;
            l = Bin(std::move(op), std::move(l), Multiplicative());
        }
        return l;
    }
    ExprP Multiplicative() {
        ExprP l = UnaryExpr();
        while (IsOp("*") || IsOp("/") || IsOp("%")) {
            std::string op = Next().text;
            l = Bin(std::move(op), std::move(l), UnaryExpr());
        }
        return l;
    }
    ExprP UnaryExpr() {
        if (IsOp("-")) {
            Guard g(*this);
            const Pos pos = Here();
            Next();
            return Make(Expr::K::Unary, pos, "-", {UnaryExpr()});
        }
        return Postfix();
    }

    std::vector<ExprP> Args() {
        std::vector<ExprP> args;
        Expect(Bt::LParen);
        if (Accept(Bt::RParen)) return args;
        for (;;) {
            if (args.size() >= kMaxCallArgs) Error("C_LIMIT", "too many arguments");
            args.push_back(Expression());
            if (Accept(Bt::Comma)) continue;
            Expect(Bt::RParen);
            return args;
        }
    }

    ExprP Postfix() {
        ExprP e = Primary();
        // No member / index / call chain directly on "Новый ...", except a member chain on "Новый Запрос(...)".
        if (e->kind == Expr::K::New && (!IsChainableNew(*e) || Peek().kind != Bt::Dot)) return e;
        for (;;) {
            if (Peek().kind == Bt::Dot) {
                Next();
                const Token& m = Peek();
                // After a dot every keyword is a plain member name; it never changes statement structure.
                if (m.kind != Bt::Identifier && m.kind != Bt::Keyword) Error("C_PARSE", "member name expected");
                if (m.mixed_script || !IsValidIdent(m.text)) Error("C_PARSE", "invalid identifier");
                std::string name = Next().text;
                if (Peek().kind == Bt::LParen) {
                    std::vector<ExprP> args = Args();
                    args.insert(args.begin(), e);
                    e = Make(Expr::K::MethodCall, e->pos, std::move(name), std::move(args));
                } else {
                    e = Make(Expr::K::Member, e->pos, std::move(name), {e});
                }
            } else if (Peek().kind == Bt::LBracket) {
                Next();
                ExprP idx = Expression();
                Expect(Bt::RBracket);
                e = Make(Expr::K::Index, e->pos, "", {e, std::move(idx)});
            } else if (Peek().kind == Bt::LParen) {
                if (e->kind != Expr::K::Var) Error("C_PARSE", "only a plain name can be called");
                std::vector<ExprP> args = Args();
                e = Make(Expr::K::Call, e->pos, e->text, std::move(args));
            } else {
                return e;
            }
        }
    }

    ExprP Primary() {
        const Token& t = Peek();
        const Pos pos = Here();
        switch (t.kind) {
            case Bt::Number: return Literal(Expr::L::Number, pos, Next().text);
            case Bt::String: return Literal(Expr::L::String, pos, Next().value);
            case Bt::Date: return Literal(Expr::L::Date, pos, Next().value);
            case Bt::Identifier: {
                const std::string name = TakeIdent();
                if (IsEvalName(name)) throw Fail{{"C_EVAL", pos, "dynamic evaluation is not allowed"}};
                return Make(Expr::K::Var, pos, name);
            }
            case Bt::LParen: {
                Next();
                ExprP e = Expression();
                Expect(Bt::RParen);
                return e;
            }
            case Bt::Question: return Ternary(pos);
            case Bt::Keyword: break;
            default: Error("C_PARSE", "expression expected");
        }
        switch (t.kw) {
            case Bk::True: Next(); return Literal(Expr::L::True, pos, "");
            case Bk::False: Next(); return Literal(Expr::L::False, pos, "");
            case Bk::Undefined: Next(); return Literal(Expr::L::Undefined, pos, "");
            case Bk::Null: Next(); return Literal(Expr::L::Null, pos, "");
            case Bk::New: return NewExpr(pos);
            default: RejectKeyword(t.kw);
        }
    }

    ExprP Ternary(Pos pos) {
        Next();
        Expect(Bt::LParen);
        std::vector<ExprP> parts;
        parts.push_back(Expression());
        Expect(Bt::Comma);
        parts.push_back(Expression());
        Expect(Bt::Comma);
        parts.push_back(Expression());
        Expect(Bt::RParen);
        return Make(Expr::K::Ternary, pos, "", std::move(parts));
    }

    ExprP NewExpr(Pos pos) {
        Next();
        if (Peek().kind == Bt::LParen) Error("C_DYNAMIC_NEW", "the type of a new object must be a plain name");
        std::string type = TakeIdent();
        std::vector<ExprP> args;
        bool parens = false;
        if (Peek().kind == Bt::LParen) {
            parens = true;
            args = Args();
        }
        ExprP e = Node(Expr::K::New, pos, std::move(args));
        e->text = std::move(type);
        e->has_parens = parens;
        return Seal(std::move(e));
    }

    std::vector<Token> t_;
    bool allow_reserved_ = false;
    size_t i_ = 0;
    int depth_ = 0;
    int block_depth_ = 0;
    int loop_depth_ = 0;
    size_t stmt_count_ = 0;
    std::unordered_map<const Expr*, int> depth_of_;
};

// ---- iterative validator ----

struct Work {
    const Expr* e = nullptr;
    const Stmt* s = nullptr;
    const Body* b = nullptr;
    int depth = 0;       // Expr: tree depth; Stmt / Body: block nesting
    bool in_loop = false;
};

bool Bad(ParseError& err, const char* code, const char* detail) {
    err = ParseError{code, Pos{0, 1, 1}, detail};
    return false;
}

bool CheckExprNodeImpl(const Expr& e, ParseError& err, bool allow_reserved) {
    const char* kInvalid = "invalid expression node";
    if (e.lit != Expr::L::None && e.kind != Expr::K::Lit) return Bad(err, "C_INTERNAL", kInvalid);
    if (e.has_parens && e.kind != Expr::K::New) return Bad(err, "C_INTERNAL", kInvalid);
    for (const ExprP& a : e.args) {
        if (!a) return Bad(err, "C_INTERNAL", "null child");
    }
    const size_t n = e.args.size();
    switch (e.kind) {
        case Expr::K::Lit:
            if (n != 0) return Bad(err, "C_INTERNAL", kInvalid);
            switch (e.lit) {
                case Expr::L::Number: return IsNumberText(e.text) || Bad(err, "C_INTERNAL", "invalid number");
                case Expr::L::Date:
                    return (AllDigits(e.text) && (e.text.size() == 8 || e.text.size() == 12 || e.text.size() == 14)) ||
                           Bad(err, "C_INTERNAL", "invalid date");
                case Expr::L::String:
                    return e.text.find('\0') == std::string::npos || Bad(err, "C_INTERNAL", "invalid string");
                case Expr::L::True: case Expr::L::False: case Expr::L::Undefined: case Expr::L::Null:
                    return e.text.empty() || Bad(err, "C_INTERNAL", kInvalid);
                case Expr::L::None: break;
            }
            return Bad(err, "C_INTERNAL", kInvalid);
        case Expr::K::Var:
            if (!allow_reserved && IsReservedName(e.text)) return Bad(err, "C_RESERVED", "reserved name");
            return (n == 0 && IsPlainName(e.text)) || Bad(err, "C_INTERNAL", "invalid identifier");
        case Expr::K::Member:
            return (n == 1 && IsValidIdent(e.text)) || Bad(err, "C_INTERNAL", "invalid member");
        case Expr::K::Index:
            return (n == 2 && e.text.empty()) || Bad(err, "C_INTERNAL", kInvalid);
        case Expr::K::Call:
            if (n > kMaxCallArgs) return Bad(err, "C_LIMIT", "too many arguments");
            if (!allow_reserved && IsReservedName(e.text)) return Bad(err, "C_RESERVED", "reserved name");
            return IsPlainName(e.text) || Bad(err, "C_INTERNAL", "invalid identifier");
        case Expr::K::MethodCall:
            if (n > kMaxCallArgs + 1) return Bad(err, "C_LIMIT", "too many arguments");
            return (n >= 1 && IsValidIdent(e.text)) || Bad(err, "C_INTERNAL", "invalid method");
        case Expr::K::New:
            if (n > kMaxCallArgs) return Bad(err, "C_LIMIT", "too many arguments");
            if (!allow_reserved && IsReservedName(e.text)) return Bad(err, "C_RESERVED", "reserved name");
            return (IsPlainName(e.text) && (e.has_parens || n == 0)) || Bad(err, "C_INTERNAL", "invalid new");
        case Expr::K::Ternary:
            return (n == 3 && e.text.empty()) || Bad(err, "C_INTERNAL", kInvalid);
        case Expr::K::Unary:
            return (n == 1 && (e.text == "-" || e.text == "НЕ")) || Bad(err, "C_INTERNAL", kInvalid);
        case Expr::K::Binary: {
            static const char* const ops[] = {"+", "-", "*", "/", "%", "=", "<>", "<", ">", "<=", ">=", "И", "ИЛИ"};
            const bool known = std::find_if(std::begin(ops), std::end(ops), [&](const char* o) { return e.text == o; }) != std::end(ops);
            return (n == 2 && known) || Bad(err, "C_INTERNAL", kInvalid);
        }
        case Expr::K::Unset: break;
    }
    return Bad(err, "C_INTERNAL", "unset expression node");
}

bool CheckExprNode(const Expr& e, ParseError& err, bool allow_reserved) {
    if (CheckExprNodeImpl(e, err, allow_reserved)) return true;
    err.pos = e.pos;
    return false;
}

bool ValidLvalue(const Expr& e) {
    if (e.kind != Expr::K::Var && e.kind != Expr::K::Member && e.kind != Expr::K::Index) return false;
    const Expr* cur = &e;
    size_t guard = 0;
    while (cur->kind == Expr::K::Member || cur->kind == Expr::K::Index) {
        if (cur->args.empty() || !cur->args[0] || ++guard > static_cast<size_t>(kMaxExprDepth)) return false;
        cur = cur->args[0].get();
    }
    return cur->kind == Expr::K::Var;
}

bool CheckStmtNodeImpl(const Stmt& s, bool in_loop, ParseError& err, bool allow_reserved) {
    const char* kInvalid = "invalid statement node";
    if (s.has_else && s.kind != Stmt::K::If) return Bad(err, "C_INTERNAL", kInvalid);
    const bool wantsVar = s.kind == Stmt::K::ForEach || s.kind == Stmt::K::ForTo;
    if (wantsVar && !allow_reserved && IsReservedName(s.var)) return Bad(err, "C_RESERVED", "reserved name");
    if (wantsVar ? !IsPlainName(s.var) : !s.var.empty()) return Bad(err, "C_INTERNAL", kInvalid);
    for (const ExprP& x : s.exprs) {
        if (!x) return Bad(err, "C_INTERNAL", "null child");
    }
    const size_t ne = s.exprs.size();
    const size_t nb = s.blocks.size();
    switch (s.kind) {
        case Stmt::K::Assign:
            return (ne == 2 && nb == 0 && ValidLvalue(*s.exprs[0])) || Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::CallStmt:
            return (ne == 1 && nb == 0 && (s.exprs[0]->kind == Expr::K::Call || s.exprs[0]->kind == Expr::K::MethodCall)) ||
                   Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::If:
            return (ne >= 1 && nb == ne + (s.has_else ? 1u : 0u)) || Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::ForEach: case Stmt::K::While:
            return (ne == 1 && nb == 1) || Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::ForTo:
            return (ne == 2 && nb == 1) || Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::Try:
            return (ne == 0 && nb == 2) || Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::Break: case Stmt::K::Continue:
            return (ne == 0 && nb == 0 && in_loop) || Bad(err, "C_INTERNAL", kInvalid);
        case Stmt::K::Unset: break;
    }
    return Bad(err, "C_INTERNAL", "unset statement node");
}

bool CheckStmtNode(const Stmt& s, bool in_loop, ParseError& err, bool allow_reserved) {
    if (CheckStmtNodeImpl(s, in_loop, err, allow_reserved)) return true;
    err.pos = s.pos;
    return false;
}

}  // namespace

bool IsChainableNew(const Expr& e) {
    if (e.kind != Expr::K::New || !e.has_parens) return false;
    const std::string key = NormalizeIdentifierKey(e.text);
    return key == NormalizeIdentifierKey("Запрос") || key == "QUERY";
}

bool ValidateBody(const Body& body, ParseError& err, bool allow_reserved) {
    std::vector<Work> stack;
    Work root;
    root.b = &body;
    stack.push_back(root);
    size_t visits = 0;
    size_t stmts = 0;
    while (!stack.empty()) {
        const Work w = stack.back();
        stack.pop_back();
        if (++visits > kMaxNodes) return Bad(err, "C_LIMIT", "tree too large");
        if (w.b) {
            if (w.depth > kMaxBlockDepth) return Bad(err, "C_LIMIT", "blocks nested too deep");
            for (const StmtP& sp : w.b->stmts) {
                if (!sp) return Bad(err, "C_INTERNAL", "null statement");
                if (++stmts > kMaxStatements) return Bad(err, "C_LIMIT", "too many statements");
                Work x;
                x.s = sp.get();
                x.depth = w.depth;
                x.in_loop = w.in_loop;
                stack.push_back(x);
            }
        } else if (w.s) {
            const Stmt& s = *w.s;
            if (!CheckStmtNode(s, w.in_loop, err, allow_reserved)) return false;
            for (const ExprP& x : s.exprs) {
                Work e;
                e.e = x.get();
                e.depth = 1;
                stack.push_back(e);
            }
            const bool loop = s.kind == Stmt::K::ForEach || s.kind == Stmt::K::ForTo || s.kind == Stmt::K::While;
            for (const Body& blk : s.blocks) {
                Work b;
                b.b = &blk;
                b.depth = w.depth + 1;
                b.in_loop = w.in_loop || loop;
                stack.push_back(b);
            }
        } else {
            if (w.depth > kMaxExprDepth) {
                Bad(err, "C_LIMIT", "expression too deep");
                err.pos = w.e->pos;
                return false;
            }
            if (!CheckExprNode(*w.e, err, allow_reserved)) return false;
            for (const ExprP& a : w.e->args) {
                Work c;
                c.e = a.get();
                c.depth = w.depth + 1;
                stack.push_back(c);
            }
        }
    }
    return true;
}

ParseResult ParseBsl(const std::string& utf8, bool allow_reserved) {
    ParseResult r;
    try {
        P p(Tokenize(utf8), allow_reserved);
        p.Prescan();
        r.body = p.ParseProgram();
        ParseError err;
        if (!ValidateBody(r.body, err, allow_reserved)) throw Fail{err};
        r.ok = true;
    } catch (const Fail& f) {
        r.ok = false;
        r.body = Body{};
        r.error = f.e;
    } catch (...) {
        r.ok = false;
        r.body = Body{};
        r.error = ParseError{"C_INTERNAL", Pos{0, 1, 1}, "internal error"};
    }
    return r;
}

}  // namespace lineage::bsl
