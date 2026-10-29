#include "strict/canonical_printer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "strict/ast_dump.h"
#include "strict/strict_parser.h"
#include "strict/strict_tokenizer.h"

namespace lineage::strict {

namespace {

struct Refuse {
    const char* code;
    const char* detail;
};

using AstExpr = lineage::strict::Expr;

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

// `s` lexes as exactly one token of `kind` spelled exactly as `s` (no surrounding blanks, nothing left over).
bool LexesAs(const std::string& s, Tk kind, Kw* kw = nullptr) {
    if (s.empty()) return false;
    const std::vector<Token> toks = Tokenize(s);
    if (toks.size() != 2 || toks[0].kind != kind || toks[0].text != s || toks[1].kind != Tk::End) return false;
    if (kw) *kw = toks[0].kw;
    return true;
}

bool IsPlainIdent(const std::string& s) { return LexesAs(s, Tk::Identifier) && !IsMixedScript(s); }

bool IsAllowedKeywordSegment(const std::string& s) {
    Kw kw = Kw::None;
    if (!LexesAs(s, Tk::Keyword, &kw)) return false;
    return kw == Kw::Refs || kw == Kw::Presentation || kw == Kw::RefPresentation || kw == Kw::Value;
}

// The keyword В as an alias after КАК (ИЗ Т КАК В, Т.Х КАК В) and as the first segment of a path to a source (В.Поле).
bool IsInKeywordAlias(const std::string& s) {
    Kw kw = Kw::None;
    return LexesAs(s, Tk::Keyword, &kw) && kw == Kw::In;
}

bool IsDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Dotted name: first segment a plain identifier, the others a plain identifier or an allowlisted keyword
// (ССЫЛКА, ПРЕДСТАВЛЕНИЕ, ПРЕДСТАВЛЕНИЕССЫЛКИ, ЗНАЧЕНИЕ) in its stored spelling.
bool IsPrintableDotted(const std::string& s, size_t min_segments) {
    size_t n = 0;
    size_t start = 0;
    for (;;) {
        const size_t dot = s.find('.', start);
        const std::string seg = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!(IsPlainIdent(seg) || (n > 0 && IsAllowedKeywordSegment(seg)))) return false;
        ++n;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return n >= min_segments;
}

// Binding strength; a child needs parentheses when it binds weaker than its slot requires.
constexpr int kPrecOr = 1;
constexpr int kPrecAnd = 2;
constexpr int kPrecNot = 3;
constexpr int kPrecPredicate = 4;
constexpr int kPrecAdd = 5;
constexpr int kPrecMul = 6;
constexpr int kPrecNeg = 7;
constexpr int kPrecPrimary = 8;

int PrecOf(const AstExpr& e) {
    switch (e.kind) {
        case AstExpr::K::Binary:
            if (e.text == "ИЛИ") return kPrecOr;
            if (e.text == "И") return kPrecAnd;
            if (e.text == "+" || e.text == "-") return kPrecAdd;
            if (e.text == "*" || e.text == "/") return kPrecMul;
            return kPrecPredicate;   // comparisons
        case AstExpr::K::Unary:
            return e.text == "НЕ" ? kPrecNot : kPrecNeg;
        case AstExpr::K::InList:
        case AstExpr::K::InSubquery:
        case AstExpr::K::InHierarchy:
        case AstExpr::K::Between:
        case AstExpr::K::Like:
        case AstExpr::K::IsNullTest:
        case AstExpr::K::RefsTest:
            return kPrecPredicate;
        default:
            return kPrecPrimary;
    }
}

class Printer {
public:
    std::string Run(const Batch& b) {
        for (size_t i = 0; i < b.statements.size(); ++i) {
            if (i != 0) W(";\n");
            const Statement& st = b.statements[i];
            if (st.kind == Statement::K::Drop) {
                if (!IsPlainIdent(st.drop_name)) Fail("Q_PRINT_IDENT", "drop name");
                W("УНИЧТОЖИТЬ ");
                W(st.drop_name);
            } else {
                if (!st.select) Fail("Q_PRINT_NULL", "statement");
                PrintQuery(*st.select);
            }
        }
        return std::move(out_);
    }

private:
    std::string out_;
    int depth_ = 0;

    struct Level {
        Printer& p;
        explicit Level(Printer& pp) : p(pp) {
            if (++p.depth_ > kMaxPrintDepth) p.Fail("Q_PRINT_LIMIT", "nesting too deep");
        }
        ~Level() { --p.depth_; }
    };

    [[noreturn]] void Fail(const char* code, const char* detail) { throw Refuse{code, detail}; }

    void W(const char* s) { W(std::string(s)); }
    void W(const std::string& s) {
        if (out_.size() + s.size() > kMaxCanonicalBytes) Fail("Q_PRINT_LIMIT", "text too large");
        out_ += s;
    }

    void Ident(const std::string& s, const char* what) {
        if (!IsPlainIdent(s)) Fail("Q_PRINT_IDENT", what);
        W(s);
    }
    void Dotted(const std::string& s, size_t min_segments, const char* what) {
        if (!IsPrintableDotted(s, min_segments)) Fail("Q_PRINT_IDENT", what);
        W(s);
    }
    void Integer(const std::string& s, const char* what) {
        if (!IsDigits(s) || !LexesAs(s, Tk::Number)) Fail("Q_PRINT_NUMBER", what);
        W(s);
    }

    // ---- expressions ----
    // `logic` marks an operand of AND / OR: a BETWEEN there is always parenthesised so its own AND never reads
    // as the connective.
    void Child(const ExprP& e, int need, bool logic = false) {
        if (!e) Fail("Q_PRINT_NULL", "null child");
        const bool paren = PrecOf(*e) < need || (logic && e->kind == AstExpr::K::Between);
        if (paren) W("(");
        PrintExpr(*e);
        if (paren) W(")");
    }
    void List(const std::vector<ExprP>& v, size_t from = 0) {
        for (size_t i = from; i < v.size(); ++i) {
            if (i != from) W(", ");
            Child(v[i], 0);
        }
    }
    void StringLiteral(const std::string& value) {
        std::string lit = "\"";
        for (const char c : value) {
            if (c == '"') lit += "\"\"";
            else lit += c;
        }
        lit += "\"";
        // The tokenizer is the only judge of what a string may contain (valid UTF-8, no NUL).
        const std::vector<Token> toks = Tokenize(lit);
        if (toks.size() != 2 || toks[0].kind != Tk::String || toks[0].value != value || toks[1].kind != Tk::End) {
            Fail("Q_PRINT_STRING", "string literal");
        }
        W(lit);
    }
    void PrintCall(const AstExpr& e) {
        const size_t n = e.args.size();
        Kw kw = Kw::None;
        if (!IsPlainIdent(e.text)) {
            // Only the four fixed-arity keyword functions may be called by a keyword spelling.
            if (!LexesAs(e.text, Tk::Keyword, &kw) || e.text != CanonicalKeyword(kw) || e.distinct) {
                Fail("Q_PRINT_IDENT", "call name");
            }
            const size_t arity = kw == Kw::IsNull ? 2 : 1;
            const bool fixed = kw == Kw::IsNull || kw == Kw::ValueType || kw == Kw::Presentation ||
                               kw == Kw::RefPresentation;
            if (!fixed || n != arity) Fail("Q_PRINT_IDENT", "keyword call shape");
        }
        W(e.text);
        W("(");
        if (e.distinct) {
            W("РАЗЛИЧНЫЕ ");
        } else if (n == 0 && (e.text == "КОЛИЧЕСТВО" || e.text == "COUNT")) {
            W("*");
        }
        List(e.args);
        W(")");
    }
    void PrintCase(const AstExpr& e) {
        const size_t n = e.args.size();
        size_t i = 0;
        W("ВЫБОР");
        if (e.has_operand) {
            W(" ");
            Child(e.args[i++], 0);
        }
        const size_t end = n - (e.has_else ? 1 : 0);
        for (; i < end; i += 2) {
            W(" КОГДА ");
            Child(e.args[i], 0);
            W(" ТОГДА ");
            Child(e.args[i + 1], 0);
        }
        if (e.has_else) {
            W(" ИНАЧЕ ");
            Child(e.args[n - 1], 0);
        }
        W(" КОНЕЦ");
    }
    void PrintCast(const AstExpr& e) {
        W("ВЫРАЗИТЬ(");
        Child(e.args[0], 0);
        W(" КАК ");
        Dotted(e.type_name, 1, "type name");
        if (!e.type_args.empty()) {
            W("(");
            for (size_t i = 0; i < e.type_args.size(); ++i) {
                if (i != 0) W(", ");
                Integer(e.type_args[i], "type argument");
            }
            W(")");
        }
        W(")");
    }
    void PrintSubqueryBody(const Select& s) {
        W("(");
        PrintQuery(s);
        W(")");
    }
    void PrintIn(const AstExpr& e) {
        Child(e.args[0], kPrecAdd);
        W(e.negated ? " НЕ В " : " В ");
        if (e.kind == AstExpr::K::InHierarchy) W("ИЕРАРХИИ ");
        if (e.subquery) {
            PrintSubqueryBody(*e.subquery);
            return;
        }
        W("(");
        List(e.args, 1);
        W(")");
    }
    void PrintField(const AstExpr& e) {
        if (e.parts.empty()) Fail("Q_PRINT_IDENT", "empty path");
        for (size_t i = 0; i < e.parts.size(); ++i) {
            const std::string& p = e.parts[i];
            const bool keyword_ok = i > 0 ? IsAllowedKeywordSegment(p) : e.parts.size() > 1 && IsInKeywordAlias(p);
            if (!(IsPlainIdent(p) || keyword_ok)) Fail("Q_PRINT_IDENT", "path segment");
            if (i != 0) W(".");
            W(p);
        }
    }

    void PrintExpr(const AstExpr& e) {
        Level lv(*this);
        switch (e.kind) {
            case AstExpr::K::Unset:
                Fail("Q_PRINT_NODE", "unset node");
            case AstExpr::K::Field:
                PrintField(e);
                return;
            case AstExpr::K::Param:
                if (!LexesAs("&" + e.text, Tk::Parameter) || IsMixedScript(e.text)) Fail("Q_PRINT_IDENT", "parameter");
                W("&");
                W(e.text);
                return;
            case AstExpr::K::StringLit:
                StringLiteral(e.text);
                return;
            case AstExpr::K::NumberLit:
                if (!LexesAs(e.text, Tk::Number)) Fail("Q_PRINT_NUMBER", "number");
                W(e.text);
                return;
            case AstExpr::K::BoolLit:
            case AstExpr::K::NullLit:
            case AstExpr::K::UndefinedLit:
                // ValidateBatch pins the text to ИСТИНА / ЛОЖЬ / NULL / НЕОПРЕДЕЛЕНО.
                W(e.text);
                return;
            case AstExpr::K::Unary:
                W(e.text == "НЕ" ? "НЕ " : "-");
                Child(e.args[0], kPrecPrimary);
                return;
            case AstExpr::K::Binary:
                PrintBinary(e);
                return;
            case AstExpr::K::Call:
                PrintCall(e);
                return;
            case AstExpr::K::Case:
                PrintCase(e);
                return;
            case AstExpr::K::Cast:
                PrintCast(e);
                return;
            case AstExpr::K::InList:
            case AstExpr::K::InSubquery:
            case AstExpr::K::InHierarchy:
                PrintIn(e);
                return;
            case AstExpr::K::Between:
                Child(e.args[0], kPrecAdd);
                W(e.negated ? " НЕ МЕЖДУ " : " МЕЖДУ ");
                Child(e.args[1], kPrecAdd);
                W(" И ");
                Child(e.args[2], kPrecAdd);
                return;
            case AstExpr::K::Like:
                Child(e.args[0], kPrecAdd);
                W(e.negated ? " НЕ ПОДОБНО " : " ПОДОБНО ");
                Child(e.args[1], kPrecAdd);
                if (e.args.size() == 3) {
                    W(" СПЕЦСИМВОЛ ");
                    Child(e.args[2], kPrecAdd);
                }
                return;
            case AstExpr::K::IsNullTest:
                Child(e.args[0], kPrecAdd);
                W(e.negated ? " ЕСТЬ НЕ NULL" : " ЕСТЬ NULL");
                return;
            case AstExpr::K::RefsTest:
                Child(e.args[0], kPrecAdd);
                W(e.negated ? " НЕ ССЫЛКА " : " ССЫЛКА ");
                Dotted(e.type_name, 2, "type name");
                return;
            case AstExpr::K::ValueFn:
                W("ЗНАЧЕНИЕ(");
                Dotted(e.type_name, 2, "type name");
                W(")");
                return;
            case AstExpr::K::TypeFn:
                W("ТИП(");
                Dotted(e.type_name, 1, "type name");
                W(")");
                return;
            case AstExpr::K::Row:
                W("(");
                List(e.args);
                W(")");
                return;
        }
        Fail("Q_PRINT_NODE", "unknown node kind");
    }
    void PrintBinary(const AstExpr& e) {
        const int prec = PrecOf(e);
        // Left-associative operators accept an equally strong left operand; the right operand must bind
        // strictly tighter, so the printed text re-associates to the same tree. Comparisons take additive
        // operands on both sides.
        int left = prec;
        int right = prec + 1;
        if (prec == kPrecPredicate) left = right = kPrecAdd;
        const bool logic = e.text == "И" || e.text == "ИЛИ";
        Child(e.args[0], left, logic);
        W(" " + e.text + " ");
        Child(e.args[1], right, logic);
    }

    // ---- queries ----
    void PrintSource(const Source& src) {
        switch (src.kind) {
            case Source::K::Table:
                Dotted(src.name, 1, "table name");
                break;
            case Source::K::VirtualTable:
                Dotted(src.name, 1, "table name");
                W("(");
                for (size_t i = 0; i < src.vt_params.size(); ++i) {
                    if (i != 0) W(", ");
                    if (src.vt_params[i]) Child(src.vt_params[i], 0);
                }
                W(")");
                break;
            case Source::K::Subquery:
                if (!src.subquery) Fail("Q_PRINT_NULL", "subquery source");
                PrintSubqueryBody(*src.subquery);
                break;
        }
        if (!src.alias.empty()) {
            W(" КАК ");
            if (IsInKeywordAlias(src.alias)) W(src.alias);
            else Ident(src.alias, "source alias");
        } else if (src.kind == Source::K::Subquery) {
            Fail("Q_PRINT_IDENT", "subquery source alias");
        }
    }
    static const char* JoinWord(Join::K k) {
        switch (k) {
            case Join::K::Inner: return "ВНУТРЕННЕЕ";
            case Join::K::Left: return "ЛЕВОЕ";
            case Join::K::Right: return "ПРАВОЕ";
            case Join::K::Full: return "ПОЛНОЕ";
        }
        return "";
    }
    void PrintItem(const SelectItem& it, const std::string& name) {
        if (it.wildcard) {
            for (const std::string& p : it.wildcard_prefix) {
                Ident(p, "wildcard prefix");
                W(".");
            }
            W("*");
            return;
        }
        if (!it.expr) Fail("Q_PRINT_NULL", "item expression");
        Child(it.expr, 0);
        if (!name.empty()) {
            W(" КАК ");
            if (!IsPlainIdent(name) && !IsAllowedKeywordSegment(name) && !IsInKeywordAlias(name)) {
                Fail("Q_PRINT_IDENT", "item alias");
            }
            W(name);
        }
    }
    // One select without its ОБЪЕДИНИТЬ parts and statement-level clauses.
    void PrintCore(const Select& s, bool head) {
        W("ВЫБРАТЬ");
        if (s.allowed) W(" РАЗРЕШЕННЫЕ");
        if (s.distinct) W(" РАЗЛИЧНЫЕ");
        if (!s.top.empty()) {
            W(" ПЕРВЫЕ ");
            Integer(s.top, "top count");
        }
        std::vector<std::string> names;
        if (head && !ComputeResultNames(s, names)) Fail("Q_PRINT_DUPLICATE_COLUMN", "duplicate explicit column name");
        for (size_t i = 0; i < s.items.size(); ++i) {
            W(i == 0 ? "\n\t" : ",\n\t");
            PrintItem(s.items[i], head ? names[i] : s.items[i].alias);
        }
        if (!s.into.empty()) {
            W("\nПОМЕСТИТЬ ");
            Ident(s.into, "temporary table name");
        }
        if (!s.from.empty()) {
            W("\nИЗ");
            for (size_t i = 0; i < s.from.size(); ++i) {
                W(i == 0 ? "\n\t" : ",\n\t");
                PrintSource(s.from[i]);
                if (i >= s.joins.size()) continue;
                for (const Join& j : s.joins[i]) {
                    W("\n\t");
                    W(JoinWord(j.kind));
                    W(" СОЕДИНЕНИЕ ");
                    PrintSource(j.source);
                    W(" ПО ");
                    Child(j.on, 0);
                }
            }
        }
        if (s.where) {
            W("\nГДЕ ");
            Child(s.where, 0);
        }
        if (!s.group_by.empty()) {
            W("\nСГРУППИРОВАТЬ ПО ");
            List(s.group_by);
        }
        if (s.having) {
            W("\nИМЕЮЩИЕ ");
            Child(s.having, 0);
        }
        if (s.for_update) {
            W("\nДЛЯ ИЗМЕНЕНИЯ");
            for (size_t i = 0; i < s.for_update_tables.size(); ++i) {
                W(i == 0 ? " " : ", ");
                Dotted(s.for_update_tables[i], 1, "for-update table");
            }
        }
        if (!s.index_by.empty()) {
            W("\nИНДЕКСИРОВАТЬ ПО ");
            List(s.index_by);
        }
    }
    void PrintTotals(const Select& s) {
        W("\nИТОГИ");
        if (!s.totals_aggregates.empty()) {
            W(" ");
            List(s.totals_aggregates);
        }
        W(" ПО ");
        for (size_t i = 0; i < s.totals_by.size(); ++i) {
            const TotalsItem& t = s.totals_by[i];
            if (i != 0) W(", ");
            if (t.overall) {
                W("ОБЩИЕ");
                continue;
            }
            Child(t.expr, 0);
            if (t.only_hierarchy) {
                W(" ТОЛЬКО ИЕРАРХИЯ");
            } else if (t.hierarchy) {
                W(" ИЕРАРХИЯ");
            } else if (t.periods) {
                W(" ПЕРИОДАМИ(");
                List(t.periods_args);
                W(")");
            }
            if (!t.alias.empty()) {
                W(" КАК ");
                Ident(t.alias, "totals alias");
            }
        }
    }
    void PrintQuery(const Select& s) {
        Level lv(*this);
        PrintCore(s, true);
        for (const Select::UnionPart& u : s.unions) {
            if (!u.select) Fail("Q_PRINT_NULL", "union part");
            W(u.all ? "\nОБЪЕДИНИТЬ ВСЕ\n" : "\nОБЪЕДИНИТЬ\n");
            PrintCore(*u.select, false);
        }
        if (!s.order_by.empty()) {
            W("\nУПОРЯДОЧИТЬ ПО ");
            for (size_t i = 0; i < s.order_by.size(); ++i) {
                if (i != 0) W(", ");
                Child(s.order_by[i].expr, 0);
                if (s.order_by[i].desc) W(" УБЫВ");
            }
        }
        if (s.autoorder) W("\nАВТОУПОРЯДОЧИВАНИЕ");
        if (s.has_totals) PrintTotals(s);
    }
};

PrintResult Refused(const char* code, const char* detail) {
    PrintResult r;
    r.error = ParseError{code, Pos{}, detail};
    return r;
}

}  // namespace

PrintResult PrintCanonicalChecked(const Batch& batch) {
    ParseError verr;
    if (!ValidateBatch(batch, verr)) {
        PrintResult r;
        r.error = verr;
        return r;
    }
    try {
        std::string text = Printer().Run(batch);

        // The text that will be executed must parse again to the very same tree and print to itself.
        const ParseResult again = ParseBatchStrict(text);
        if (!again.ok) return Refused("Q_PRINT_REPARSE", "canonical text does not parse");
        if (DumpBatch(again.batch) != DumpBatch(batch, true)) return Refused("Q_PRINT_MISMATCH", "re-parse differs");
        if (Printer().Run(again.batch) != text) return Refused("Q_PRINT_MISMATCH", "print is not idempotent");

        PrintResult r;
        r.ok = true;
        r.text = std::move(text);
        return r;
    } catch (const Refuse& f) {
        return Refused(f.code, f.detail);
    } catch (...) {
        return Refused("Q_PRINT_INTERNAL", "internal error");
    }
}

std::string PrintCanonical(const Batch& batch) {
    PrintResult r = PrintCanonicalChecked(batch);
    return r.ok ? std::move(r.text) : std::string();
}

}  // namespace lineage::strict
