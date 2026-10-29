#include "strict/strict_parser.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utf_utils.h"

namespace lineage::strict {

namespace {

using AstExpr = lineage::strict::Expr;

struct Fail {
    ParseError e;
};

// An identifier mixing Latin and Cyrillic letters is a homoglyph vector (a Cyrillic "а" inside a Latin name and
// the like). Digits and '_' are neutral. Non-ASCII letters other than Cyrillic never reach this point: the
// tokenizer turns them into Tk::Unknown.
bool IsMixedScript(const std::string& s) {
    bool latin = false;
    bool cyrillic = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            latin = true;
        } else if (c == 0xD0 || c == 0xD1) {
            cyrillic = true;
            ++i;
        }
    }
    return latin && cyrillic;
}

// Keywords accepted as a path segment AFTER a dot (Т.Ссылка, Т.Представление, ...); never as the first segment.
bool IsAllowedKeywordSegment(Kw kw) {
    return kw == Kw::Refs || kw == Kw::Presentation || kw == Kw::RefPresentation || kw == Kw::Value;
}

bool IsCountName(const std::string& key) { return key == "КОЛИЧЕСТВО" || key == "COUNT"; }

bool IsComparisonOp(const Token& t) {
    return t.kind == Tk::Op &&
           (t.text == "=" || t.text == "<>" || t.text == "<" || t.text == ">" || t.text == "<=" || t.text == ">=");
}

bool IsIntegerText(const std::string& s);

constexpr size_t kMaxTopDigits = 18;
constexpr size_t kMaxPeriodsArgs = 4;

bool IsIdentChar(const std::string& s, size_t i, size_t& len, bool first) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    len = 1;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') return true;
    if (!first && c >= '0' && c <= '9') return true;
    if (i + 1 < s.size()) {
        const unsigned char b = static_cast<unsigned char>(s[i + 1]);
        if ((c == 0xD0 && (b == 0x81 || (b >= 0x90 && b <= 0xBF))) || (c == 0xD1 && ((b >= 0x80 && b <= 0x8F) || b == 0x91))) {
            len = 2;
            return true;
        }
    }
    return false;
}

// Tokenizer identifier charset + no Latin/Cyrillic mixing. ЕСТЬNULL is the one canonical keyword that is mixed.
bool IsValidIdent(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0, len = 1; i < s.size(); i += len) {
        if (!IsIdentChar(s, i, len, i == 0)) return false;
    }
    return s == "ЕСТЬNULL" || !IsMixedScript(s);
}

bool IsValidDotted(const std::string& s) {
    size_t start = 0;
    for (;;) {
        const size_t dot = s.find('.', start);
        if (!IsValidIdent(s.substr(start, dot == std::string::npos ? std::string::npos : dot - start))) return false;
        if (dot == std::string::npos) return true;
        start = dot + 1;
    }
}

bool IsNumberText(const std::string& s) {
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return IsIntegerText(s);
    return IsIntegerText(s.substr(0, dot)) && IsIntegerText(s.substr(dot + 1));
}

bool IsIntegerText(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

class P {
public:
    explicit P(std::vector<Token> t) : t_(std::move(t)) {}

    // Any token the tokenizer could not classify rejects the whole text, before any grammar decision.
    void Prescan() const {
        for (const Token& t : t_) {
            if (t.kind != Tk::Unknown) continue;
            const bool limit = t.text == "<size>" || t.text == "<limit>";
            throw Fail{{limit ? "Q_PARSE_LIMIT" : "Q_PARSE_TOKEN", {t.pos, t.line, t.col}, limit ? t.text : ""}};
        }
    }

    ExprP Expr() {
        Guard g(*this);
        return Or();
    }

    void ExpectEnd() {
        if (Peek().kind != Tk::End) Error("Q_PARSE_UNEXPECTED");
    }

    // ---- helpers ----
    const Token& Peek(size_t k = 0) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
    bool IsKw(Kw kw, size_t k = 0) const { return Peek(k).kind == Tk::Keyword && Peek(k).kw == kw; }
    bool IsOp(const char* op) const { return Peek().kind == Tk::Op && Peek().text == op; }
    bool AcceptKw(Kw kw) {
        if (!IsKw(kw)) return false;
        ++i_;
        return true;
    }
    bool Accept(Tk k) {
        if (Peek().kind != k) return false;
        ++i_;
        return true;
    }
    const Token& Next() {
        if (Peek().kind == Tk::Unknown) Error("Q_PARSE_TOKEN");
        return t_[i_ < t_.size() - 1 ? i_++ : i_];
    }
    void ExpectKw(Kw kw) {
        if (!AcceptKw(kw)) Error("Q_PARSE_EXPECTED");
    }
    void Expect(Tk k) {
        if (Peek().kind != k) Error("Q_PARSE_EXPECTED");
        Next();
    }
    [[noreturn]] void Error(const char* code, const char* detail = "") {
        const Token& tok = Peek();
        const bool unknown = tok.kind == Tk::Unknown;
        throw Fail{{unknown ? "Q_PARSE_TOKEN" : code, {tok.pos, tok.line, tok.col}, unknown ? "unknown token" : detail}};
    }
    Pos Here() const { return {Peek().pos, Peek().line, Peek().col}; }

    struct Guard {
        P& p;
        explicit Guard(P& pp) : p(pp) {
            if (++p.depth_ > kMaxParseDepth) p.Error("Q_PARSE_LIMIT", "nesting too deep");
        }
        ~Guard() { --p.depth_; }
    };

    // ---- nodes: every node is created with a concrete kind and sealed (tree depth accounted) ----
    ExprP Node(AstExpr::K kind, Pos pos, std::vector<ExprP> args = {}) {
        auto e = std::make_shared<AstExpr>();
        e->kind = kind;
        e->pos = pos;
        e->args = std::move(args);
        return e;
    }
    ExprP Seal(ExprP e) {
        int d = 0;
        for (const ExprP& a : e->args) {
            const auto it = depth_of_.find(a.get());
            if (it == depth_of_.end()) Error("Q_PARSE_INTERNAL", "unsealed child");
            d = std::max(d, it->second);
        }
        ++d;
        if (d > kMaxExprDepth) Error("Q_PARSE_LIMIT", "expression too deep");
        depth_of_[e.get()] = d;
        return e;
    }
    ExprP Leaf(AstExpr::K kind, Pos pos, std::string text) {
        ExprP e = Node(kind, pos);
        e->text = std::move(text);
        return Seal(std::move(e));
    }
    ExprP Bin(std::string op, ExprP l, ExprP r) {
        const Pos pos = l->pos;
        ExprP e = Node(AstExpr::K::Binary, pos, {std::move(l), std::move(r)});
        e->text = std::move(op);
        return Seal(std::move(e));
    }
    ExprP Un(std::string op, ExprP a, Pos p) {
        ExprP e = Node(AstExpr::K::Unary, p, {std::move(a)});
        e->text = std::move(op);
        return Seal(std::move(e));
    }

    // ---- precedence ladder: ИЛИ < И < НЕ < predicate < additive < multiplicative < unary < primary ----
    ExprP Or() {
        ExprP l = And();
        while (AcceptKw(Kw::Or)) l = Bin("ИЛИ", l, And());
        return l;
    }
    ExprP And() {
        ExprP l = Not();
        while (AcceptKw(Kw::And)) l = Bin("И", l, Not());
        return l;
    }
    ExprP Not() {
        if (!IsKw(Kw::Not)) return Predicate();
        Guard g(*this);
        const Pos p = Here();
        Next();
        return Un("НЕ", Not(), p);
    }
    ExprP Additive() {
        ExprP l = Mul();
        while (IsOp("+") || IsOp("-")) {
            std::string op = Next().text;
            l = Bin(std::move(op), l, Mul());
        }
        return l;
    }
    ExprP Mul() {
        ExprP l = Unary();
        while (IsOp("*") || IsOp("/")) {
            std::string op = Next().text;
            l = Bin(std::move(op), l, Unary());
        }
        return l;
    }
    ExprP Unary() {
        if (!IsOp("-")) return Primary();
        Guard g(*this);
        const Pos p = Here();
        Next();
        return Un("-", Unary(), p);
    }

    ExprP Predicate();
    ExprP Primary();

    // A nested query (subquery body): the same grammar as a statement, except that ПОМЕСТИТЬ is not allowed.
    std::shared_ptr<Select> SelectStmt() { return Query(false); }
    Batch ParseBatch();

private:
    // True when the next tokens start another predicate: a chain such as `a = b = c` or `x В (1) В (2)`.
    bool AtPredicateStart() const {
        if (IsComparisonOp(Peek())) return true;
        const size_t k = IsKw(Kw::Not) ? 1 : 0;
        if (Peek(k).kind != Tk::Keyword) return false;
        const Kw kw = Peek(k).kw;
        return kw == Kw::In || kw == Kw::Between || kw == Kw::Like || kw == Kw::Refs || (k == 0 && kw == Kw::Is);
    }
    // Called right after the token was consumed with Next(); reports at that token.
    void CheckName(const Token& t) {
        if (IsMixedScript(t.text)) {
            --i_;
            Error("Q_PARSE_MIXED_SCRIPT");
        }
    }
    // Dotted name: every segment is an identifier (a keyword segment is rejected unless allowlisted).
    std::string DottedName(size_t min_segments) {
        if (Peek().kind != Tk::Identifier) Error("Q_PARSE_EXPECTED");
        const Token& first = Next();
        CheckName(first);
        std::string name = first.text;
        size_t n = 1;
        while (Peek().kind == Tk::Dot) {
            Next();
            const Token& seg = Peek();
            if (seg.kind == Tk::Keyword && !IsAllowedKeywordSegment(seg.kw)) Error("Q_PARSE_KEYWORD_SEGMENT");
            if (seg.kind != Tk::Identifier && seg.kind != Tk::Keyword) Error("Q_PARSE_EXPECTED");
            Next();
            if (seg.kind == Tk::Identifier) CheckName(seg);
            name += "." + seg.text;
            ++n;
        }
        if (n < min_segments) Error("Q_PARSE_EXPECTED", "qualified name expected");
        return name;
    }
    bool AtHierarchyWord() const {
        const Token& t = Peek();
        if (t.kind == Tk::Identifier) return NormalizeIdentifierKey(t.text) == "ИЕРАРХИИ";
        return t.kind == Tk::Keyword && t.kw == Kw::Hierarchy && NormalizeIdentifierKey(t.text) == "HIERARCHY";
    }

    // ---- statements ----
    std::shared_ptr<Select> Query(bool top);
    std::shared_ptr<Select> SelectCore(bool allow_into);
    SelectItem ItemClause();
    bool WildcardAhead() const;
    Source SourceClause();
    Join JoinClause();
    bool AtJoin() const {
        return IsKw(Kw::Join) || IsKw(Kw::Left) || IsKw(Kw::Right) || IsKw(Kw::Inner) || IsKw(Kw::Full);
    }
    std::string AliasName();
    std::string SourceAlias();
    std::string SingleName();
    std::vector<ExprP> ExprList();
    void OrderClause(Select& s);
    void TotalsClause(Select& s);

    ExprP IdentifierExpr();
    ExprP KeywordExpr();
    ExprP CallArgs(const Token& name_tok, std::string name);
    ExprP CaseExpr();
    ExprP CastExpr();
    ExprP FixedCall(Kw kw, size_t arity);
    ExprP Paren();

    std::vector<Token> t_;
    size_t i_ = 0;
    int depth_ = 0;
    std::unordered_map<const AstExpr*, int> depth_of_;
};

ExprP P::Predicate() {
    ExprP l = Additive();
    const Pos pos = l->pos;

    if (IsComparisonOp(Peek())) {
        std::string op = Next().text;
        ExprP r = Additive();
        ExprP e = Bin(std::move(op), l, r);
        if (AtPredicateStart()) Error("Q_PARSE_CHAIN");
        return e;
    }

    bool negated = false;
    if (IsKw(Kw::Not)) {
        const bool ok = Peek(1).kind == Tk::Keyword &&
                        (Peek(1).kw == Kw::In || Peek(1).kw == Kw::Between || Peek(1).kw == Kw::Like ||
                         Peek(1).kw == Kw::Refs);
        if (!ok) Error("Q_PARSE_UNEXPECTED");
        Next();
        negated = true;
    }
    if (Peek().kind != Tk::Keyword) return l;

    ExprP e;
    switch (Peek().kw) {
        case Kw::In: {
            Next();
            const bool hier = AtHierarchyWord();
            if (hier) Next();
            Expect(Tk::LParen);
            if (IsKw(Kw::Select)) {
                std::shared_ptr<Select> sub = SelectStmt();
                Expect(Tk::RParen);
                e = Node(hier ? AstExpr::K::InHierarchy : AstExpr::K::InSubquery, pos, {l});
                e->subquery = std::move(sub);
            } else {
                if (l->kind == AstExpr::K::Row) Error("Q_PARSE_ROW_POSITION", "row operand needs a subquery");
                std::vector<ExprP> args{l};
                do {
                    args.push_back(Expr());
                } while (Accept(Tk::Comma));
                Expect(Tk::RParen);
                e = Node(hier ? AstExpr::K::InHierarchy : AstExpr::K::InList, pos, std::move(args));
            }
            e->negated = negated;
            break;
        }
        case Kw::Between: {
            Next();
            ExprP lo = Additive();
            ExpectKw(Kw::And);
            ExprP hi = Additive();
            e = Node(AstExpr::K::Between, pos, {l, lo, hi});
            e->negated = negated;
            break;
        }
        case Kw::Like: {
            Next();
            std::vector<ExprP> args{l, Additive()};
            if (AcceptKw(Kw::Escape)) args.push_back(Additive());
            e = Node(AstExpr::K::Like, pos, std::move(args));
            e->negated = negated;
            break;
        }
        case Kw::Is: {
            Next();
            const bool not_null = AcceptKw(Kw::Not);
            ExpectKw(Kw::Null);
            e = Node(AstExpr::K::IsNullTest, pos, {l});
            e->negated = not_null;
            break;
        }
        case Kw::Refs: {
            Next();
            e = Node(AstExpr::K::RefsTest, pos, {l});
            e->type_name = DottedName(2);
            e->negated = negated;
            break;
        }
        default:
            return l;   // a keyword that continues an enclosing production (И, ИЛИ, ТОГДА, КАК, ...)
    }
    Seal(e);
    if (AtPredicateStart()) Error("Q_PARSE_CHAIN");
    return e;
}

ExprP P::Primary() {
    const Token& t = Peek();
    const Pos pos = Here();
    switch (t.kind) {
        case Tk::String:
            Next();
            return Leaf(AstExpr::K::StringLit, pos, t.value);
        case Tk::Number:
            Next();
            return Leaf(AstExpr::K::NumberLit, pos, t.text);
        case Tk::Parameter:
            Next();
            CheckName(t);
            return Leaf(AstExpr::K::Param, pos, t.text.substr(1));
        case Tk::Identifier:
            return IdentifierExpr();
        case Tk::Keyword:
            // В.Поле: a source named В (КАК В, see SourceAlias). The operator В never starts an operand.
            if (t.kw == Kw::In && Peek(1).kind == Tk::Dot) return IdentifierExpr();
            return KeywordExpr();
        case Tk::LParen:
            return Paren();
        default:
            Error("Q_PARSE_UNEXPECTED");
    }
}

ExprP P::IdentifierExpr() {
    const Token& first = Next();
    CheckName(first);
    const Pos pos{first.pos, first.line, first.col};
    if (Peek().kind == Tk::LParen) return CallArgs(first, NormalizeIdentifierKey(first.text));

    ExprP e = Node(AstExpr::K::Field, pos);
    e->parts.push_back(first.text);
    while (Peek().kind == Tk::Dot) {
        Next();
        const Token& seg = Peek();
        if (seg.kind == Tk::Keyword && !IsAllowedKeywordSegment(seg.kw)) Error("Q_PARSE_KEYWORD_SEGMENT");
        if (seg.kind != Tk::Identifier && seg.kind != Tk::Keyword) Error("Q_PARSE_EXPECTED");
        Next();
        if (seg.kind == Tk::Identifier) CheckName(seg);
        e->parts.push_back(seg.text);
    }
    if (Peek().kind == Tk::LParen) Error("Q_PARSE_UNEXPECTED", "call on a path");
    return Seal(std::move(e));
}

// Call syntax after the name: ( [РАЗЛИЧНЫЕ] args ) with the КОЛИЧЕСТВО(*) form.
ExprP P::CallArgs(const Token& name_tok, std::string name) {
    const Pos pos{name_tok.pos, name_tok.line, name_tok.col};
    const bool is_count = IsCountName(name);
    ExprP e = Node(AstExpr::K::Call, pos);
    e->text = std::move(name);
    Expect(Tk::LParen);
    if (Peek().kind == Tk::RParen) {
        // КОЛИЧЕСТВО() would collide with the КОЛИЧЕСТВО(*) representation (no args): not a valid call anyway.
        if (is_count) Error("Q_PARSE_EXPECTED", "argument expected");
        Next();
        return Seal(std::move(e));
    }
    if (IsKw(Kw::Distinct)) {
        if (!is_count) Error("Q_PARSE_UNEXPECTED", "РАЗЛИЧНЫЕ outside КОЛИЧЕСТВО");
        Next();
        e->distinct = true;
    } else if (IsOp("*")) {
        if (!is_count) Error("Q_PARSE_UNEXPECTED", "* outside КОЛИЧЕСТВО");
        Next();
        Expect(Tk::RParen);
        return Seal(std::move(e));   // COUNT(*): a Call with no arguments
    }
    do {
        e->args.push_back(Expr());
    } while (Accept(Tk::Comma));
    Expect(Tk::RParen);
    if (e->distinct && e->args.size() != 1) Error("Q_PARSE_UNEXPECTED", "РАЗЛИЧНЫЕ takes one argument");
    return Seal(std::move(e));
}

ExprP P::FixedCall(Kw kw, size_t arity) {
    const Token& tok = Next();
    const Pos pos{tok.pos, tok.line, tok.col};
    ExprP e = Node(AstExpr::K::Call, pos);
    e->text = CanonicalKeyword(kw);
    Expect(Tk::LParen);
    for (size_t k = 0; k < arity; ++k) {
        if (k != 0) Expect(Tk::Comma);
        e->args.push_back(Expr());
    }
    Expect(Tk::RParen);
    return Seal(std::move(e));
}

ExprP P::KeywordExpr() {
    const Pos pos = Here();
    switch (Peek().kw) {
        case Kw::True:
            Next();
            return Leaf(AstExpr::K::BoolLit, pos, "ИСТИНА");
        case Kw::False:
            Next();
            return Leaf(AstExpr::K::BoolLit, pos, "ЛОЖЬ");
        case Kw::Null:
            Next();
            return Leaf(AstExpr::K::NullLit, pos, "NULL");
        case Kw::Undefined:
            Next();
            return Leaf(AstExpr::K::UndefinedLit, pos, "НЕОПРЕДЕЛЕНО");
        case Kw::Case:
            return CaseExpr();
        case Kw::Cast:
            return CastExpr();
        case Kw::Value:
        case Kw::Type: {
            const bool is_value = Peek().kw == Kw::Value;
            Next();
            Expect(Tk::LParen);
            ExprP e = Node(is_value ? AstExpr::K::ValueFn : AstExpr::K::TypeFn, pos);
            e->type_name = DottedName(is_value ? 2 : 1);
            Expect(Tk::RParen);
            return Seal(std::move(e));
        }
        case Kw::ValueType:
        case Kw::Presentation:
        case Kw::RefPresentation:
            return FixedCall(Peek().kw, 1);
        case Kw::IsNull:
            return FixedCall(Kw::IsNull, 2);
        default:
            Error("Q_PARSE_UNEXPECTED");
    }
}

ExprP P::CaseExpr() {
    const Pos pos = Here();
    Next();   // ВЫБОР
    ExprP e = Node(AstExpr::K::Case, pos);
    if (!IsKw(Kw::When)) {
        e->args.push_back(Expr());
        e->has_operand = true;
    }
    if (!IsKw(Kw::When)) Error("Q_PARSE_EXPECTED", "КОГДА expected");
    while (AcceptKw(Kw::When)) {
        e->args.push_back(Expr());
        ExpectKw(Kw::Then);
        e->args.push_back(Expr());
    }
    if (AcceptKw(Kw::Else)) {
        e->args.push_back(Expr());
        e->has_else = true;
    }
    ExpectKw(Kw::End);
    return Seal(std::move(e));
}

ExprP P::CastExpr() {
    const Pos pos = Here();
    Next();   // ВЫРАЗИТЬ
    Expect(Tk::LParen);
    ExprP e = Node(AstExpr::K::Cast, pos);
    e->args.push_back(Expr());
    ExpectKw(Kw::As);
    e->type_name = DottedName(1);
    if (Accept(Tk::LParen)) {
        for (int k = 0; k < 2; ++k) {
            if (k != 0 && !Accept(Tk::Comma)) break;
            if (Peek().kind != Tk::Number || !IsIntegerText(Peek().text)) Error("Q_PARSE_EXPECTED", "integer expected");
            e->type_args.push_back(Next().text);
        }
        Expect(Tk::RParen);
    }
    Expect(Tk::RParen);
    return Seal(std::move(e));
}

// ( expr ) or ( a, b, ... ) row. A subquery here is not allowed: it is valid only as the right side of В.
ExprP P::Paren() {
    const Pos pos = Here();
    Next();   // (
    if (IsKw(Kw::Select)) Error("Q_PARSE_SUBQUERY_POSITION");
    ExprP first = Expr();
    if (Peek().kind != Tk::Comma) {
        Expect(Tk::RParen);
        return first;
    }
    ExprP row = Node(AstExpr::K::Row, pos, {first});
    while (Accept(Tk::Comma)) row->args.push_back(Expr());
    Expect(Tk::RParen);
    return Seal(std::move(row));
}

// ---- statements ----

std::string P::AliasName() {
    if (Peek().kind != Tk::Identifier) Error("Q_PARSE_EXPECTED", "alias expected");
    const Token& t = Next();
    CheckName(t);
    return t.text;
}

// Alias of a source after an explicit КАК: an identifier, or the keyword В (ИЗ Т КАК В): after КАК it cannot start
// anything else, and the operator В never starts an operand, so В.Поле is a path (Primary).
std::string P::SourceAlias() {
    if (IsKw(Kw::In)) return Next().text;
    return AliasName();
}

// A name with exactly one segment (temporary table, alias): no dots.
std::string P::SingleName() {
    std::string name = AliasName();
    if (Peek().kind == Tk::Dot) Error("Q_PARSE_UNEXPECTED", "qualified name not allowed here");
    return name;
}

std::vector<ExprP> P::ExprList() {
    std::vector<ExprP> v;
    do {
        v.push_back(Expr());
    } while (Accept(Tk::Comma));
    return v;
}

// Name '.' ... '.' '*' : a wildcard over a path (Т.*), checked without consuming.
bool P::WildcardAhead() const {
    size_t k = 0;
    if (Peek(k).kind != Tk::Identifier) return false;
    for (;;) {
        ++k;
        if (Peek(k).kind != Tk::Dot) return false;
        ++k;
        if (Peek(k).kind == Tk::Op && Peek(k).text == "*") return true;
        if (Peek(k).kind != Tk::Identifier) return false;
    }
}

SelectItem P::ItemClause() {
    SelectItem it;
    it.pos = Here();
    if (IsOp("*")) {
        Next();
        it.wildcard = true;
        return it;
    }
    if (WildcardAhead()) {
        it.wildcard = true;
        while (!IsOp("*")) {
            const Token& seg = Next();
            CheckName(seg);
            it.wildcard_prefix.push_back(seg.text);
            Expect(Tk::Dot);
        }
        Next();   // *
        return it;
    }
    it.expr = Expr();
    if (AcceptKw(Kw::As)) {
        // A column may be named like an allowlisted path keyword (КАК Ссылка) or В (КАК В): after КАК it cannot start
        // anything else. A column named В can be defined only, not referred to (bare В and Т.В stay errors).
        const bool keyword = Peek().kind == Tk::Keyword && (IsAllowedKeywordSegment(Peek().kw) || Peek().kw == Kw::In);
        it.alias = keyword ? Next().text : AliasName();
        it.alias_explicit = true;
    } else if (Peek().kind == Tk::Identifier) {
        it.alias = AliasName();
    }
    return it;
}

Source P::SourceClause() {
    Source src;
    src.pos = Here();
    if (Peek().kind == Tk::LParen) {
        Next();
        if (!IsKw(Kw::Select)) Error("Q_PARSE_EXPECTED", "subquery expected");
        src.kind = Source::K::Subquery;
        src.subquery = Query(false);
        Expect(Tk::RParen);
        src.alias = AcceptKw(Kw::As) ? SourceAlias() : AliasName();   // a subquery source always needs an alias
        return src;
    }
    if (Peek().kind != Tk::Identifier) Error("Q_PARSE_EXPECTED", "table expected");
    src.name = DottedName(1);
    if (Peek().kind == Tk::LParen) {
        Next();
        src.kind = Source::K::VirtualTable;
        if (Peek().kind != Tk::RParen) {
            for (;;) {
                if (src.vt_params.size() >= kMaxVirtualTableParams) Error("Q_PARSE_LIMIT", "too many table parameters");
                if (Peek().kind == Tk::Comma || Peek().kind == Tk::RParen) src.vt_params.push_back(nullptr);
                else src.vt_params.push_back(Expr());
                if (!Accept(Tk::Comma)) break;
            }
        }
        Expect(Tk::RParen);
    }
    if (AcceptKw(Kw::As)) src.alias = SourceAlias();
    else if (Peek().kind == Tk::Identifier) src.alias = AliasName();
    return src;
}

Join P::JoinClause() {
    Join j;
    j.pos = Here();
    if (AcceptKw(Kw::Left)) {
        j.kind = Join::K::Left;
        AcceptKw(Kw::Outer);
    } else if (AcceptKw(Kw::Right)) {
        j.kind = Join::K::Right;
        AcceptKw(Kw::Outer);
    } else if (AcceptKw(Kw::Full)) {
        j.kind = Join::K::Full;
        AcceptKw(Kw::Outer);
    } else if (AcceptKw(Kw::Inner)) {
        j.kind = Join::K::Inner;
    }
    ExpectKw(Kw::Join);
    j.source = SourceClause();
    ExpectKw(Kw::Po);
    j.on = Expr();
    return j;
}

std::shared_ptr<Select> P::SelectCore(bool allow_into) {
    auto s = std::make_shared<Select>();
    s->pos = Here();
    ExpectKw(Kw::Select);
    s->allowed = AcceptKw(Kw::Allowed);
    s->distinct = AcceptKw(Kw::Distinct);
    if (AcceptKw(Kw::Top)) {
        if (Peek().kind != Tk::Number || !IsIntegerText(Peek().text) || Peek().text.size() > kMaxTopDigits) {
            Error("Q_PARSE_EXPECTED", "integer expected after ПЕРВЫЕ");
        }
        s->top = Next().text;
    }
    do {
        s->items.push_back(ItemClause());
    } while (Accept(Tk::Comma));
    if (IsKw(Kw::Into)) {
        if (!allow_into) Error("Q_PARSE_UNEXPECTED", "ПОМЕСТИТЬ is allowed only in the first query of a statement");
        Next();
        s->into = SingleName();
    }
    if (AcceptKw(Kw::From)) {
        do {
            s->from.push_back(SourceClause());
            s->joins.emplace_back();
            while (AtJoin()) s->joins.back().push_back(JoinClause());
        } while (Accept(Tk::Comma));
    }
    if (AcceptKw(Kw::Where)) s->where = Expr();
    if (AcceptKw(Kw::Group)) {
        ExpectKw(Kw::Po);
        s->group_by = ExprList();
    }
    if (AcceptKw(Kw::Having)) s->having = Expr();
    if (AcceptKw(Kw::For)) {
        ExpectKw(Kw::Update);
        s->for_update = true;
        if (Peek().kind == Tk::Identifier) {
            do {
                s->for_update_tables.push_back(DottedName(1));
            } while (Accept(Tk::Comma));
        }
    }
    if (AcceptKw(Kw::Index)) {
        ExpectKw(Kw::Po);
        s->index_by = ExprList();
        if (s->into.empty()) Error("Q_PARSE_UNEXPECTED", "ИНДЕКСИРОВАТЬ ПО needs ПОМЕСТИТЬ in the same query");
    }
    // Attachment of these clauses to a union is not verified against the platform: fail closed.
    if ((s->for_update || !s->index_by.empty()) && IsKw(Kw::Union)) {
        Error("Q_PARSE_AMBIGUOUS_CLAUSE", "ДЛЯ ИЗМЕНЕНИЯ / ИНДЕКСИРОВАТЬ ПО before ОБЪЕДИНИТЬ");
    }
    return s;
}

void P::OrderClause(Select& s) {
    Next();   // УПОРЯДОЧИТЬ
    ExpectKw(Kw::Po);
    do {
        OrderItem it;
        it.expr = Expr();
        if (AcceptKw(Kw::Desc)) it.desc = true;
        else AcceptKw(Kw::Asc);
        s.order_by.push_back(std::move(it));
    } while (Accept(Tk::Comma));
}

void P::TotalsClause(Select& s) {
    Next();   // ИТОГИ
    s.has_totals = true;
    if (!IsKw(Kw::Po)) s.totals_aggregates = ExprList();
    ExpectKw(Kw::Po);
    do {
        TotalsItem it;
        if (AcceptKw(Kw::Overall)) {
            it.overall = true;
            s.totals_overall = true;
        } else {
            it.expr = Expr();
            if (AcceptKw(Kw::Only)) {
                ExpectKw(Kw::Hierarchy);
                it.hierarchy = true;
                it.only_hierarchy = true;
            } else if (AcceptKw(Kw::Hierarchy)) {
                it.hierarchy = true;
            } else if (AcceptKw(Kw::Periods)) {
                it.periods = true;
                Expect(Tk::LParen);
                it.periods_args = ExprList();
                Expect(Tk::RParen);
                if (it.periods_args.size() > kMaxPeriodsArgs) Error("Q_PARSE_LIMIT", "too many arguments");
            }
            if (AcceptKw(Kw::As)) it.alias = AliasName();
        }
        s.totals_by.push_back(std::move(it));
    } while (Accept(Tk::Comma));
}

// query := select_core (ОБЪЕДИНИТЬ [ВСЕ] select_core)* [order] [АВТОУПОРЯДОЧИВАНИЕ] [totals]
std::shared_ptr<Select> P::Query(bool top) {
    Guard g(*this);
    std::shared_ptr<Select> head = SelectCore(top);
    while (IsKw(Kw::Union)) {
        if (head->unions.size() >= kMaxUnionParts) Error("Q_PARSE_LIMIT", "too many union parts");
        Next();
        Select::UnionPart part;
        part.all = AcceptKw(Kw::All);
        part.select = SelectCore(false);
        head->unions.push_back(std::move(part));
    }
    if (IsKw(Kw::Order)) OrderClause(*head);
    if (AcceptKw(Kw::Autoorder)) head->autoorder = true;
    if (IsKw(Kw::Totals)) TotalsClause(*head);
    return head;
}

Batch P::ParseBatch() {
    Batch b;
    if (Peek().kind == Tk::End) Error("Q_PARSE_EMPTY", "empty batch");
    for (;;) {
        if (b.statements.size() >= kMaxBatchStatements) Error("Q_PARSE_LIMIT", "too many statements");
        Statement st;
        st.pos = Here();
        if (AcceptKw(Kw::Drop)) {
            st.kind = Statement::K::Drop;
            st.drop_name = SingleName();
        } else if (IsKw(Kw::Select)) {
            st.kind = Statement::K::Select;
            st.select = Query(true);
        } else {
            Error("Q_PARSE_EXPECTED", "statement expected");
        }
        b.statements.push_back(std::move(st));
        if (!Accept(Tk::Semicolon)) break;
        if (Peek().kind == Tk::End) break;
    }
    ExpectEnd();
    return b;
}

}  // namespace

namespace {

// Work item of the iterative tree walk. An item is either an expression (e) or a Select (s).
// sel_kind: 0 = statement-level query (ПОМЕСТИТЬ allowed), 1 = nested subquery, 2 = union part.
struct Item {
    const Expr* e = nullptr;
    const Select* s = nullptr;
    int depth = 1;       // expression depth, restarts inside every Select
    int sel_depth = 1;   // subquery nesting depth
    int sel_kind = 0;
    bool row_ok = false;
};

bool BadSelect(ParseError& err, const Select& s, const char* why) {
    err = ParseError{"Q_PARSE_INVALID_NODE", s.pos, why};
    return false;
}

bool BadAt(ParseError& err, const Pos& pos, const char* why) {
    err = ParseError{"Q_PARSE_INVALID_NODE", pos, why};
    return false;
}

bool TooLarge(ParseError& err, const Select& s) {
    err = ParseError{"Q_PARSE_LIMIT", s.pos, "query too large"};
    return false;
}

bool CheckSource(const Source& src, const Select& s, const Item& it, std::vector<Item>& stack, ParseError& err) {
    if (!src.alias.empty() && !IsValidIdent(src.alias)) return BadAt(err, src.pos, "source alias");
    if (src.kind != Source::K::Subquery) {
        if (src.subquery || !IsValidDotted(src.name)) return BadAt(err, src.pos, "source name");
        if (src.kind == Source::K::Table && !src.vt_params.empty()) return BadAt(err, src.pos, "table parameters");
        if (src.vt_params.size() > kMaxVirtualTableParams) return BadAt(err, src.pos, "too many table parameters");
        for (const ExprP& p : src.vt_params) {
            if (p) stack.push_back(Item{p.get(), nullptr, 1, it.sel_depth, 0, false});
        }
        return true;
    }
    if (!src.subquery || !src.name.empty() || !src.vt_params.empty() || src.alias.empty()) {
        return BadAt(err, src.pos, "subquery source");
    }
    stack.push_back(Item{nullptr, src.subquery.get(), 1, it.sel_depth + 1, 1, false});
    return true;
}

bool CheckTotals(const Select& s, const Item& it, std::vector<Item>& stack, ParseError& err) {
    if (!s.has_totals) {
        if (!s.totals_aggregates.empty() || !s.totals_by.empty() || s.totals_overall) {
            return BadSelect(err, s, "totals data without ИТОГИ");
        }
        return true;
    }
    if (s.totals_by.empty()) return BadSelect(err, s, "ИТОГИ without ПО");
    bool any_overall = false;
    for (const ExprP& a : s.totals_aggregates) {
        if (!a) return BadSelect(err, s, "null totals aggregate");
        stack.push_back(Item{a.get(), nullptr, 1, it.sel_depth, 0, false});
    }
    for (const TotalsItem& t : s.totals_by) {
        if (!t.alias.empty() && !IsValidIdent(t.alias)) return BadSelect(err, s, "totals alias");
        if (t.overall) {
            any_overall = true;
            if (t.expr || t.hierarchy || t.only_hierarchy || t.periods || !t.periods_args.empty() || !t.alias.empty()) {
                return BadSelect(err, s, "totals overall item");
            }
            continue;
        }
        if (!t.expr) return BadSelect(err, s, "null totals expression");
        stack.push_back(Item{t.expr.get(), nullptr, 1, it.sel_depth, 0, false});
        if (t.only_hierarchy && !t.hierarchy) return BadSelect(err, s, "totals hierarchy flags");
        if (t.periods && t.hierarchy) return BadSelect(err, s, "totals periods with hierarchy");
        if (t.periods != !t.periods_args.empty() || t.periods_args.size() > kMaxPeriodsArgs) {
            return BadSelect(err, s, "totals periods arguments");
        }
        for (const ExprP& a : t.periods_args) {
            if (!a) return BadSelect(err, s, "null periods argument");
            stack.push_back(Item{a.get(), nullptr, 1, it.sel_depth, 0, false});
        }
    }
    if (any_overall != s.totals_overall) return BadSelect(err, s, "totals overall flag");
    return true;
}

// Shape check of one Select; children are pushed on the stack. `visited` counts clause elements too.
bool CheckSelect(const Item& it, std::vector<Item>& stack, size_t& visited, ParseError& err) {
    const Select& s = *it.s;
    auto push = [&](const ExprP& e, const char* why) {
        if (!e) return BadSelect(err, s, why);
        stack.push_back(Item{e.get(), nullptr, 1, it.sel_depth, 0, false});
        return true;
    };
    if (it.sel_depth > kMaxParseDepth) {
        err = ParseError{"Q_PARSE_LIMIT", s.pos, "subqueries nested too deep"};
        return false;
    }
    if (s.items.empty()) return BadSelect(err, s, "no select items");
    if (!s.top.empty() && (!IsIntegerText(s.top) || s.top.size() > kMaxTopDigits)) return BadSelect(err, s, "top");
    if (!s.into.empty() && (it.sel_kind != 0 || !IsValidIdent(s.into))) return BadSelect(err, s, "into");
    for (const SelectItem& si : s.items) {
        if (++visited > kMaxExprNodes) return TooLarge(err, s);
        if (!si.alias.empty() && !IsValidIdent(si.alias)) return BadSelect(err, s, "item alias");
        if (si.alias_explicit && si.alias.empty()) return BadSelect(err, s, "item alias flag");
        if (si.wildcard) {
            if (si.expr || !si.alias.empty()) return BadSelect(err, s, "wildcard item");
            for (const std::string& p : si.wildcard_prefix) {
                if (!IsValidIdent(p)) return BadSelect(err, s, "wildcard prefix");
            }
        } else {
            if (!si.wildcard_prefix.empty()) return BadSelect(err, s, "item prefix");
            if (!push(si.expr, "null item expression")) return false;
        }
    }
    if (s.joins.size() != s.from.size()) return BadSelect(err, s, "joins do not match sources");
    for (size_t i = 0; i < s.from.size(); ++i) {
        if (++visited > kMaxExprNodes) return TooLarge(err, s);
        if (!CheckSource(s.from[i], s, it, stack, err)) return false;
        for (const Join& j : s.joins[i]) {
            if (++visited > kMaxExprNodes) return TooLarge(err, s);
            if (!CheckSource(j.source, s, it, stack, err)) return false;
            if (!j.on) return BadAt(err, j.pos, "join without condition");
            stack.push_back(Item{j.on.get(), nullptr, 1, it.sel_depth, 0, false});
        }
    }
    if (s.where && !push(s.where, "where")) return false;
    if (s.having && !push(s.having, "having")) return false;
    for (const ExprP& g : s.group_by) {
        if (!push(g, "null group expression")) return false;
    }
    for (const ExprP& g : s.index_by) {
        if (!push(g, "null index expression")) return false;
    }
    if (!s.for_update && !s.for_update_tables.empty()) return BadSelect(err, s, "for-update tables");
    for (const std::string& t : s.for_update_tables) {
        if (!IsValidDotted(t)) return BadSelect(err, s, "for-update table");
    }
    if (it.sel_kind == 2 && (!s.unions.empty() || !s.order_by.empty() || s.autoorder || s.has_totals)) {
        return BadSelect(err, s, "union part carries statement clauses");
    }
    if (!s.index_by.empty() && s.into.empty()) return BadSelect(err, s, "index without ПОМЕСТИТЬ");
    if (s.unions.size() > kMaxUnionParts) {
        err = ParseError{"Q_PARSE_LIMIT", s.pos, "too many union parts"};
        return false;
    }
    for (const Select::UnionPart& u : s.unions) {
        if (!u.select) return BadSelect(err, s, "null union part");
        stack.push_back(Item{nullptr, u.select.get(), 1, it.sel_depth, 2, false});
    }
    for (const OrderItem& o : s.order_by) {
        if (!push(o.expr, "null order expression")) return false;
    }
    return CheckTotals(s, it, stack, err);
}

bool Walk(std::vector<Item> stack, ParseError& err) {
    auto bad = [&](const Expr* e, const char* why) {
        err = ParseError{"Q_PARSE_INVALID_NODE", e ? e->pos : Pos{}, why};
        return false;
    };

    size_t visited = 0;
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        if (it.s) {
            if (++visited > kMaxExprNodes) return TooLarge(err, *it.s);
            if (!CheckSelect(it, stack, visited, err)) return false;
            continue;
        }
        const Expr& e = *it.e;
        if (it.depth > kMaxExprDepth || ++visited > kMaxExprNodes) {
            err = ParseError{"Q_PARSE_LIMIT", e.pos, "expression too large"};
            return false;
        }
        for (const ExprP& a : e.args) {
            if (!a) return bad(&e, "null child");
        }
        const size_t n = e.args.size();
        const AstExpr::K k = e.kind;
        const bool sub_kind = k == AstExpr::K::InSubquery || k == AstExpr::K::InHierarchy;
        if (e.subquery && !sub_kind) return bad(&e, "unexpected subquery");

        // Payload fields are legal only on the kinds that use them.
        const bool negatable = k == AstExpr::K::InList || sub_kind || k == AstExpr::K::Between ||
                               k == AstExpr::K::Like || k == AstExpr::K::IsNullTest || k == AstExpr::K::RefsTest;
        if (e.negated && !negatable) return bad(&e, "negated on wrong kind");
        if (e.distinct && k != AstExpr::K::Call) return bad(&e, "distinct on wrong kind");
        if ((e.has_operand || e.has_else) && k != AstExpr::K::Case) return bad(&e, "case flag on wrong kind");
        const bool typed = k == AstExpr::K::Cast || k == AstExpr::K::RefsTest || k == AstExpr::K::ValueFn ||
                           k == AstExpr::K::TypeFn;
        if (!typed && !e.type_name.empty()) return bad(&e, "type name on wrong kind");
        if (typed && !IsValidDotted(e.type_name)) return bad(&e, "type name");
        if (k != AstExpr::K::Cast && !e.type_args.empty()) return bad(&e, "type args on wrong kind");
        if (e.type_args.size() > 2) return bad(&e, "too many type args");
        for (const std::string& ta : e.type_args) {
            if (!IsIntegerText(ta)) return bad(&e, "type arg");
        }
        if (k != AstExpr::K::Field && !e.parts.empty()) return bad(&e, "path on wrong kind");
        const bool texted = k == AstExpr::K::Param || k == AstExpr::K::StringLit || k == AstExpr::K::NumberLit ||
                            k == AstExpr::K::BoolLit || k == AstExpr::K::NullLit ||
                            k == AstExpr::K::UndefinedLit || k == AstExpr::K::Unary ||
                            k == AstExpr::K::Binary || k == AstExpr::K::Call;
        if (!texted && !e.text.empty()) return bad(&e, "text on wrong kind");

        switch (k) {
            case AstExpr::K::Unset:
                return bad(&e, "unset node");
            case AstExpr::K::Field:
                if (n != 0 || e.parts.empty()) return bad(&e, "field");
                for (const std::string& p : e.parts) {
                    if (!IsValidIdent(p)) return bad(&e, "field segment");
                }
                break;
            case AstExpr::K::Param:
                if (n != 0 || !IsValidIdent(e.text)) return bad(&e, "parameter name");
                break;
            case AstExpr::K::NumberLit:
                if (n != 0 || !IsNumberText(e.text)) return bad(&e, "number literal");
                break;
            case AstExpr::K::StringLit:
                if (n != 0) return bad(&e, "literal with children");
                break;
            case AstExpr::K::NullLit:
                if (n != 0 || e.text != "NULL") return bad(&e, "null literal");
                break;
            case AstExpr::K::UndefinedLit:
                if (n != 0 || e.text != "НЕОПРЕДЕЛЕНО") return bad(&e, "undefined literal");
                break;
            case AstExpr::K::BoolLit:
                if (n != 0 || (e.text != "ИСТИНА" && e.text != "ЛОЖЬ")) return bad(&e, "bool literal");
                break;
            case AstExpr::K::Unary:
                if (n != 1 || (e.text != "-" && e.text != "НЕ")) return bad(&e, "unary");
                break;
            case AstExpr::K::Binary: {
                static const char* const ops[] = {"+", "-", "*", "/", "=", "<>", "<", ">", "<=", ">=", "И", "ИЛИ"};
                if (n != 2 || std::find(std::begin(ops), std::end(ops), e.text) == std::end(ops)) {
                    return bad(&e, "binary");
                }
                break;
            }
            case AstExpr::K::Call:
                if (!IsValidIdent(e.text)) return bad(&e, "call name");
                if (e.distinct && (!IsCountName(e.text) || n != 1)) return bad(&e, "distinct call");
                break;
            case AstExpr::K::Case: {
                const size_t fixed = (e.has_operand ? 1 : 0) + (e.has_else ? 1 : 0);
                if (n < fixed + 2 || (n - fixed) % 2 != 0) return bad(&e, "case shape");
                break;
            }
            case AstExpr::K::Cast:
                if (n != 1) return bad(&e, "cast");
                break;
            case AstExpr::K::InList:
                if (n < 2) return bad(&e, "in list");
                break;
            case AstExpr::K::InSubquery:
                if (n != 1 || !e.subquery) return bad(&e, "in subquery");
                break;
            case AstExpr::K::InHierarchy:
                if (e.subquery ? n != 1 : n < 2) return bad(&e, "in hierarchy");
                break;
            case AstExpr::K::Between:
                if (n != 3) return bad(&e, "between");
                break;
            case AstExpr::K::Like:
                if (n != 2 && n != 3) return bad(&e, "like");
                break;
            case AstExpr::K::IsNullTest:
                if (n != 1) return bad(&e, "is null");
                break;
            case AstExpr::K::RefsTest:
                if (n != 1) return bad(&e, "refs");
                break;
            case AstExpr::K::ValueFn:
            case AstExpr::K::TypeFn:
                if (n != 0) return bad(&e, "type function");
                break;
            case AstExpr::K::Row:
                if (!it.row_ok) return bad(&e, "row outside the left side of a subquery В");
                if (n < 2) return bad(&e, "row");
                break;
        }
        for (size_t i = 0; i < n; ++i) {
            const bool row_ok = i == 0 && sub_kind && e.subquery != nullptr;
            stack.push_back(Item{e.args[i].get(), nullptr, it.depth + 1, it.sel_depth, 0, row_ok});
        }
        if (e.subquery) stack.push_back(Item{nullptr, e.subquery.get(), 1, it.sel_depth + 1, 1, false});
    }
    return true;
}

}  // namespace

bool ValidateExpr(const ExprP& root, ParseError& err) {
    if (!root) {
        err = ParseError{"Q_PARSE_INVALID_NODE", Pos{}, "null expression"};
        return false;
    }
    return Walk({Item{root.get(), nullptr, 1, 1, 0, false}}, err);
}

bool ValidateBatch(const Batch& batch, ParseError& err) {
    if (batch.statements.empty()) {
        err = ParseError{"Q_PARSE_INVALID_NODE", Pos{}, "empty batch"};
        return false;
    }
    if (batch.statements.size() > kMaxBatchStatements) {
        err = ParseError{"Q_PARSE_LIMIT", Pos{}, "too many statements"};
        return false;
    }
    std::vector<Item> roots;
    for (const Statement& st : batch.statements) {
        if (st.kind == Statement::K::Drop) {
            if (st.select || !IsValidIdent(st.drop_name)) {
                err = ParseError{"Q_PARSE_INVALID_NODE", st.pos, "drop statement"};
                return false;
            }
        } else {
            if (!st.select || !st.drop_name.empty()) {
                err = ParseError{"Q_PARSE_INVALID_NODE", st.pos, "select statement"};
                return false;
            }
            roots.push_back(Item{nullptr, st.select.get(), 1, 1, 0, false});
        }
    }
    return Walk(std::move(roots), err);
}

ExprResult ParseExpressionForTest(const std::string& utf8) {
    ExprResult r;
    try {
        P p(Tokenize(utf8));
        p.Prescan();
        r.expr = p.Expr();
        p.ExpectEnd();
        ParseError err;
        if (!ValidateExpr(r.expr, err)) throw Fail{err};
        r.ok = true;
    } catch (const Fail& f) {
        r.ok = false;
        r.expr = nullptr;
        r.error = f.e;
    } catch (...) {
        r.ok = false;
        r.expr = nullptr;
        r.error = ParseError{"Q_PARSE_INTERNAL", Pos{0, 1, 1}, "internal error"};
    }
    return r;
}

ParseResult ParseBatchStrict(const std::string& utf8) {
    ParseResult r;
    try {
        P p(Tokenize(utf8));
        p.Prescan();
        r.batch = p.ParseBatch();
        ParseError err;
        if (!ValidateBatch(r.batch, err)) throw Fail{err};
        r.ok = true;
    } catch (const Fail& f) {
        r.ok = false;
        r.batch = Batch{};
        r.error = f.e;
    } catch (...) {
        r.ok = false;
        r.batch = Batch{};
        r.error = ParseError{"Q_PARSE_INTERNAL", Pos{0, 1, 1}, "internal error"};
    }
    return r;
}

}  // namespace lineage::strict
