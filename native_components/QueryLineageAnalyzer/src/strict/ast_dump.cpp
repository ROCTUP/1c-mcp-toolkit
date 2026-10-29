#include "strict/ast_dump.h"

#include <cstdio>
#include <unordered_set>
#include <vector>

#include "strict/strict_tokenizer.h"
#include "utf_utils.h"

namespace lineage::strict {

namespace {

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '"') out += "\\\"";
        else if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", c);
            out += buf;
        } else out += ch;
    }
    return out + "\"";
}

void Args(const Expr& e, std::string& out, bool fill, size_t from = 0) {
    for (size_t i = from; i < e.args.size(); ++i) {
        out += ' ';
        out += e.args[i] ? DumpExpr(*e.args[i], fill) : std::string("(NULL)");
    }
}

std::string DumpSelect(const Select& s, bool fill, bool head);

std::string OptExpr(const ExprP& e, bool fill) { return e ? DumpExpr(*e, fill) : std::string("_"); }

std::string DumpSource(const Source& src, bool fill) {
    std::string o;
    switch (src.kind) {
        case Source::K::Table: o = "(table " + src.name; break;
        case Source::K::VirtualTable:
            o = "(vt " + src.name + " (params";
            for (const ExprP& p : src.vt_params) o += " " + OptExpr(p, fill);
            o += ")";
            break;
        case Source::K::Subquery:
            o = "(subq " + (src.subquery ? DumpSelect(*src.subquery, fill, true) : std::string("(NULL)"));
            break;
    }
    if (!src.alias.empty()) o += " КАК " + src.alias;
    return o + ")";
}

const char* JoinName(Join::K k) {
    switch (k) {
        case Join::K::Inner: return "inner";
        case Join::K::Left: return "left";
        case Join::K::Right: return "right";
        case Join::K::Full: return "full";
    }
    return "?";
}

std::string ExprList(const std::vector<ExprP>& v, bool fill) {
    std::string o;
    for (const ExprP& e : v) o += " " + OptExpr(e, fill);
    return o;
}

// `head` is false only for an ОБЪЕДИНИТЬ part: the canonical printer names the columns of the first part only.
std::string DumpSelect(const Select& s, bool fill, bool head) {
    std::string o = "(select";
    if (s.allowed) o += " allowed";
    if (s.distinct) o += " distinct";
    if (!s.top.empty()) o += " top " + s.top;
    o += " (items";
    std::vector<std::string> names;
    if (fill && head && !ComputeResultNames(s, names)) names.clear();
    for (size_t ii = 0; ii < s.items.size(); ++ii) {
        const SelectItem& it = s.items[ii];
        o += " (item ";
        if (it.wildcard) {
            for (const std::string& p : it.wildcard_prefix) o += p + ".";
            o += "*";
        } else {
            o += OptExpr(it.expr, fill);
        }
        std::string alias = it.alias;
        if (alias.empty() && ii < names.size()) alias = names[ii];
        if (!alias.empty()) o += " КАК " + alias;
        o += ")";
    }
    o += ")";
    if (!s.into.empty()) o += " (into " + s.into + ")";
    if (!s.from.empty()) {
        o += " (from";
        for (size_t i = 0; i < s.from.size(); ++i) {
            o += " " + DumpSource(s.from[i], fill);
            if (i < s.joins.size()) {
                for (const Join& j : s.joins[i]) {
                    o += std::string(" (join ") + JoinName(j.kind) + " " + DumpSource(j.source, fill) + " ON " + OptExpr(j.on, fill) + ")";
                }
            }
        }
        o += ")";
    }
    if (s.where) o += " (where " + DumpExpr(*s.where, fill) + ")";
    if (!s.group_by.empty()) o += " (group" + ExprList(s.group_by, fill) + ")";
    if (s.having) o += " (having " + DumpExpr(*s.having, fill) + ")";
    if (s.for_update) {
        o += " (for-update";
        for (const std::string& t : s.for_update_tables) o += " " + t;
        o += ")";
    }
    if (!s.index_by.empty()) o += " (index" + ExprList(s.index_by, fill) + ")";
    for (const Select::UnionPart& u : s.unions) {
        o += std::string(" (union ") + (u.all ? "all " : "") + (u.select ? DumpSelect(*u.select, fill, false) : std::string("(NULL)")) + ")";
    }
    if (!s.order_by.empty()) {
        o += " (order";
        for (const OrderItem& it : s.order_by) o += " " + OptExpr(it.expr, fill) + (it.desc ? " desc" : "");
        o += ")";
    }
    if (s.autoorder) o += " autoorder";
    if (s.has_totals) {
        o += " (totals (aggregates" + ExprList(s.totals_aggregates, fill) + ") (by";
        for (const TotalsItem& t : s.totals_by) {
            o += " (";
            o += t.overall ? "overall" : OptExpr(t.expr, fill);
            if (t.only_hierarchy) o += " only-hierarchy";
            else if (t.hierarchy) o += " hierarchy";
            if (t.periods) o += " periods" + ExprList(t.periods_args, fill);
            if (!t.alias.empty()) o += " КАК " + t.alias;
            o += ")";
        }
        o += "))";
    }
    return o + ")";
}

}  // namespace

std::string DerivedResultAlias(const Expr& e) {
    if (e.kind != Expr::K::Field || e.parts.empty()) return std::string();
    std::string name;
    if (e.parts.size() <= 2) {
        name = e.parts.back();
    } else {
        for (size_t i = 1; i < e.parts.size(); ++i) name += e.parts[i];
    }
    // Printable only as a plain identifier or as one of the path keywords a column alias may be (Т.Ссылка is named
    // Ссылка): any other keyword or a name that does not lex as one token cannot follow КАК, and a mixed-script
    // name is never produced.
    const std::vector<Token> toks = Tokenize(name);
    if (toks.size() != 2 || toks[1].kind != Tk::End || toks[0].text != name) return std::string();
    const bool keyword_alias = toks[0].kind == Tk::Keyword &&
                               (toks[0].kw == Kw::Refs || toks[0].kw == Kw::Presentation ||
                                toks[0].kw == Kw::RefPresentation || toks[0].kw == Kw::Value);
    if (toks[0].kind != Tk::Identifier && !keyword_alias) return std::string();
    bool latin = false;
    bool cyrillic = false;
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) latin = true;
        else if (c == 0xD0 || c == 0xD1) cyrillic = true;
    }
    return latin && cyrillic ? std::string() : name;
}

namespace {

// Case-insensitive key with yo folded into ye, as the platform compares column names.
std::string NameKey(const std::string& s) {
    std::string key = lineage::NormalizeIdentifierKey(s);
    for (size_t p = 0; p + 1 < key.size(); ++p) {
        if (static_cast<unsigned char>(key[p]) == 0xD0 && static_cast<unsigned char>(key[p + 1]) == 0x81) {
            key[p + 1] = static_cast<char>(0x95);
        }
    }
    return key;
}

}  // namespace

bool ComputeResultNames(const Select& head, std::vector<std::string>& names) {
    names.assign(head.items.size(), std::string());
    std::unordered_set<std::string> used;
    for (size_t i = 0; i < head.items.size(); ++i) {
        const SelectItem& it = head.items[i];
        if (it.wildcard || it.alias.empty()) continue;
        names[i] = it.alias;
        if (!used.insert(NameKey(it.alias)).second) {
            names.clear();
            return false;
        }
    }
    for (size_t i = 0; i < head.items.size(); ++i) {
        const SelectItem& it = head.items[i];
        if (it.wildcard || !it.alias.empty()) continue;
        std::string base = it.expr ? DerivedResultAlias(*it.expr) : std::string();
        if (base.empty()) base = "Поле" + std::to_string(i + 1);
        std::string name = base;
        for (unsigned k = 1; !used.insert(NameKey(name)).second; ++k) name = base + std::to_string(k);
        names[i] = name;
    }
    return true;
}

bool StatementResultNames(const Batch& b, std::vector<std::vector<std::string>>& out) {
    out.clear();
    for (const Statement& st : b.statements) {
        out.emplace_back();
        if (st.kind != Statement::K::Select || !st.select) continue;
        if (!ComputeResultNames(*st.select, out.back())) {
            out.clear();
            return false;
        }
    }
    return true;
}

std::string DumpBatch(const Batch& b, bool fill_result_names) {
    std::string o = "(batch";
    for (const Statement& st : b.statements) {
        if (st.kind == Statement::K::Drop) o += " (drop " + st.drop_name + ")";
        else o += " " + (st.select ? DumpSelect(*st.select, fill_result_names, true) : std::string("(NULL)"));
    }
    return o + ")";
}

std::string DumpExpr(const Expr& e, bool fill) {
    std::string o;
    const char* neg = e.negated ? "!" : "";
    switch (e.kind) {
        case Expr::K::Unset: return "(UNSET)";
        case Expr::K::Field: {
            o = "(f";
            for (const auto& p : e.parts) o += " " + p;
            return o + ")";
        }
        case Expr::K::Param: return "(param &" + e.text + ")";
        case Expr::K::StringLit: return Quote(e.text);
        case Expr::K::NumberLit: return e.text;
        case Expr::K::BoolLit: return e.text;
        case Expr::K::NullLit: return "NULL";
        case Expr::K::UndefinedLit: return "НЕОПРЕДЕЛЕНО";
        case Expr::K::Unary:
        case Expr::K::Binary:
            o = "(" + e.text;
            Args(e, o, fill);
            return o + ")";
        case Expr::K::Call:
            o = std::string(e.distinct ? "(call! " : "(call ") + e.text;
            if (e.args.empty() && (e.text == "КОЛИЧЕСТВО" || e.text == "COUNT")) o += " *";
            Args(e, o, fill);
            return o + ")";
        case Expr::K::Case:
            o = e.has_operand ? "(case-op" : "(case";
            Args(e, o, fill);
            return o + ")";
        case Expr::K::Cast:
            o = "(cast";
            Args(e, o, fill);
            o += " " + e.type_name;
            for (const auto& a : e.type_args) o += " " + a;
            return o + ")";
        case Expr::K::InList:
            o = std::string("(") + neg + "in";
            Args(e, o, fill);
            return o + ")";
        case Expr::K::InSubquery:
            o = std::string("(") + neg + "in-sub";
            Args(e, o, fill);
            return o + " (subquery " + (e.subquery ? DumpSelect(*e.subquery, fill, true) : std::string("(NULL)")) + "))";
        case Expr::K::InHierarchy:
            o = std::string("(") + neg + "in-h";
            Args(e, o, fill);
            if (e.subquery) o += " (subquery " + DumpSelect(*e.subquery, fill, true) + ")";
            return o + ")";
        case Expr::K::Between:
            o = std::string("(") + neg + "between";
            Args(e, o, fill);
            return o + ")";
        case Expr::K::Like:
            o = std::string("(") + neg + "like";
            Args(e, o, fill);
            return o + ")";
        case Expr::K::IsNullTest:
            o = std::string("(") + neg + "isnull";
            Args(e, o, fill);
            return o + ")";
        case Expr::K::RefsTest:
            o = std::string("(") + neg + "refs";
            Args(e, o, fill);
            return o + " " + e.type_name + ")";
        case Expr::K::ValueFn: return "(value " + e.type_name + ")";
        case Expr::K::TypeFn: return "(type " + e.type_name + ")";
        case Expr::K::Row:
            o = "(row";
            Args(e, o, fill);
            return o + ")";
    }
    return "(UNKNOWN)";
}

}  // namespace lineage::strict
