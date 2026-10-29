#include "strict/strict_tokenizer.h"

#include <unordered_map>

#include "utf_utils.h"

namespace lineage::strict {

namespace {

constexpr size_t kMaxTokens = 200000;

struct KwEntry {
    Kw kw;
    const char* ru;
    const char* en;   // nullptr when the English spelling equals the Russian one
};

const KwEntry kKeywords[] = {
    {Kw::Select, "ВЫБРАТЬ", "SELECT"},
    {Kw::Allowed, "РАЗРЕШЕННЫЕ", "ALLOWED"},
    {Kw::Distinct, "РАЗЛИЧНЫЕ", "DISTINCT"},
    {Kw::Top, "ПЕРВЫЕ", "TOP"},
    {Kw::As, "КАК", "AS"},
    {Kw::From, "ИЗ", "FROM"},
    {Kw::Into, "ПОМЕСТИТЬ", "INTO"},
    {Kw::Drop, "УНИЧТОЖИТЬ", "DROP"},
    {Kw::Where, "ГДЕ", "WHERE"},
    {Kw::Group, "СГРУППИРОВАТЬ", "GROUP"},
    {Kw::Po, "ПО", "BY"},
    {Kw::Po, "ПО", "ON"},
    {Kw::Having, "ИМЕЮЩИЕ", "HAVING"},
    {Kw::Order, "УПОРЯДОЧИТЬ", "ORDER"},
    {Kw::Asc, "ВОЗР", "ASC"},
    {Kw::Desc, "УБЫВ", "DESC"},
    {Kw::Autoorder, "АВТОУПОРЯДОЧИВАНИЕ", "AUTOORDER"},
    {Kw::Totals, "ИТОГИ", "TOTALS"},
    {Kw::Overall, "ОБЩИЕ", "OVERALL"},
    {Kw::Hierarchy, "ИЕРАРХИЯ", "HIERARCHY"},
    {Kw::Only, "ТОЛЬКО", "ONLY"},
    {Kw::Periods, "ПЕРИОДАМИ", "PERIODS"},
    {Kw::Union, "ОБЪЕДИНИТЬ", "UNION"},
    {Kw::All, "ВСЕ", "ALL"},
    {Kw::Join, "СОЕДИНЕНИЕ", "JOIN"},
    {Kw::Left, "ЛЕВОЕ", "LEFT"},
    {Kw::Right, "ПРАВОЕ", "RIGHT"},
    {Kw::Inner, "ВНУТРЕННЕЕ", "INNER"},
    {Kw::Full, "ПОЛНОЕ", "FULL"},
    {Kw::Outer, "ВНЕШНЕЕ", "OUTER"},
    {Kw::And, "И", "AND"},
    {Kw::Or, "ИЛИ", "OR"},
    {Kw::Not, "НЕ", "NOT"},
    {Kw::In, "В", "IN"},
    {Kw::Between, "МЕЖДУ", "BETWEEN"},
    {Kw::Like, "ПОДОБНО", "LIKE"},
    {Kw::Escape, "СПЕЦСИМВОЛ", "ESCAPE"},
    {Kw::Is, "ЕСТЬ", "IS"},
    {Kw::Null, "NULL", nullptr},
    {Kw::True, "ИСТИНА", "TRUE"},
    {Kw::False, "ЛОЖЬ", "FALSE"},
    {Kw::Undefined, "НЕОПРЕДЕЛЕНО", "UNDEFINED"},
    {Kw::Case, "ВЫБОР", "CASE"},
    {Kw::When, "КОГДА", "WHEN"},
    {Kw::Then, "ТОГДА", "THEN"},
    {Kw::Else, "ИНАЧЕ", "ELSE"},
    {Kw::End, "КОНЕЦ", "END"},
    {Kw::Refs, "ССЫЛКА", "REFS"},
    {Kw::Cast, "ВЫРАЗИТЬ", "CAST"},
    {Kw::For, "ДЛЯ", "FOR"},
    {Kw::Update, "ИЗМЕНЕНИЯ", "UPDATE"},
    {Kw::Index, "ИНДЕКСИРОВАТЬ", "INDEX"},
    {Kw::Value, "ЗНАЧЕНИЕ", "VALUE"},
    {Kw::Type, "ТИП", "TYPE"},
    {Kw::ValueType, "ТИПЗНАЧЕНИЯ", "VALUETYPE"},
    {Kw::Presentation, "ПРЕДСТАВЛЕНИЕ", "PRESENTATION"},
    {Kw::RefPresentation, "ПРЕДСТАВЛЕНИЕССЫЛКИ", "REFPRESENTATION"},
    {Kw::IsNull, "ЕСТЬNULL", "ISNULL"},
};

const std::unordered_map<std::string, Kw>& KeywordTable() {
    static const std::unordered_map<std::string, Kw> table = [] {
        std::unordered_map<std::string, Kw> t;
        for (const auto& e : kKeywords) {
            t[NormalizeIdentifierKey(e.ru)] = e.kw;
            if (e.en) t[NormalizeIdentifierKey(e.en)] = e.kw;
        }
        return t;
    }();
    return table;
}

bool IsAsciiLetter(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool IsDigit(unsigned char c) { return c >= '0' && c <= '9'; }

// Length (2) of a Cyrillic letter U+0410-U+044F / U+0401 / U+0451 at s[i], else 0.
size_t CyrillicLetterLen(const std::string& s, size_t i) {
    if (i + 1 >= s.size()) return 0;
    const unsigned char a = static_cast<unsigned char>(s[i]);
    const unsigned char b = static_cast<unsigned char>(s[i + 1]);
    if (a == 0xD0 && (b == 0x81 || (b >= 0x90 && b <= 0xBF))) return 2;   // Ё, А-п
    if (a == 0xD1 && ((b >= 0x80 && b <= 0x8F) || b == 0x91)) return 2;   // р-я, ё
    return 0;
}

size_t IdentStartLen(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    if (IsAsciiLetter(static_cast<unsigned char>(s[i]))) return 1;
    return CyrillicLetterLen(s, i);
}

size_t IdentPartLen(const std::string& s, size_t i) {
    if (i < s.size() && IsDigit(static_cast<unsigned char>(s[i]))) return 1;
    return IdentStartLen(s, i);
}

// Strict UTF-8 check: rejects NUL, overlongs, surrogates, > U+10FFFF, truncated and stray continuation bytes.
bool IsValidUtf8NoNul(const std::string& v) {
    size_t i = 0;
    while (i < v.size()) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        if (c == 0) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t need = 0;
        uint32_t cp = 0;
        uint32_t minCp = 0;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; minCp = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; minCp = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; minCp = 0x10000; }
        else return false;   // stray continuation byte, C0/C1, F5..FF
        if (i + need >= v.size()) return false;
        for (size_t k = 1; k <= need; ++k) {
            const unsigned char cc = static_cast<unsigned char>(v[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += need + 1;
    }
    return true;
}

// Replaces Ё (U+0401, upper-cased key form) with Е (U+0415) in an upper-cased UTF-8 key.
std::string FoldYo(std::string key) {
    for (size_t p = 0; p + 1 < key.size(); ++p) {
        if (static_cast<unsigned char>(key[p]) == 0xD0 && static_cast<unsigned char>(key[p + 1]) == 0x81) {
            key[p + 1] = static_cast<char>(0x95);
        }
    }
    return key;
}

}  // namespace

bool IsIdentifierName(const std::string& name) {
    size_t i = IdentStartLen(name, 0);
    if (i == 0) return false;
    while (i < name.size()) {
        const size_t n = IdentPartLen(name, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

const char* CanonicalKeyword(Kw kw) {
    for (const auto& e : kKeywords) {
        if (e.kw == kw) return e.ru;
    }
    return "";
}

std::vector<Token> Tokenize(const std::string& s) {
    std::vector<Token> out;
    if (s.size() > kMaxSourceBytes) {
        Token t;
        t.kind = Tk::Unknown;
        t.text = "<size>";
        out.push_back(std::move(t));
        out.push_back(Token());
        return out;
    }
    size_t i = 0;
    uint32_t line = 1;
    uint32_t col = 1;

    // Advances i to `to`, tracking line/column (columns count code points).
    auto advance = [&](size_t to) {
        for (; i < to; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '\n') {
                ++line;
                col = 1;
            } else if ((c & 0xC0) != 0x80) {
                ++col;
            }
        }
    };
    auto emit = [&](Tk kind, size_t len, const std::string& value = std::string()) -> Token& {
        Token t;
        t.kind = kind;
        t.text = s.substr(i, len);
        t.value = value;
        t.pos = i;
        t.line = line;
        t.col = col;
        out.push_back(std::move(t));
        advance(i + len);
        return out.back();
    };
    // Length of the maximal run of identifier characters and dots starting at e.
    auto badRunEnd = [&](size_t e) {
        while (e < s.size()) {
            if (s[e] == '.') {
                ++e;
            } else if (const size_t m = IdentPartLen(s, e)) {
                e += m;
            } else {
                break;
            }
        }
        return e;
    };

    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(i + 1);
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            size_t e = i;
            while (e < s.size() && s[e] != '\n') ++e;
            advance(e);
            continue;
        }
        if (out.size() >= kMaxTokens) {
            Token t;
            t.kind = Tk::Unknown;
            t.text = "<limit>";
            t.pos = i;
            t.line = line;
            t.col = col;
            out.push_back(std::move(t));
            break;
        }

        if (const size_t n = IdentStartLen(s, i)) {
            size_t e = i + n;
            while (const size_t m = IdentPartLen(s, e)) e += m;
            const auto& table = KeywordTable();
            const std::string key = NormalizeIdentifierKey(s.substr(i, e - i));
            const auto it = table.find(key);
            if (it == table.end() && table.count(FoldYo(key)) != 0) {
                emit(Tk::Unknown, e - i);   // ё/е folding would make it a keyword: ambiguous, fail closed
                continue;
            }
            Token& t = emit(it == table.end() ? Tk::Identifier : Tk::Keyword, e - i);
            if (it != table.end()) t.kw = it->second;
            continue;
        }
        if (IsDigit(c)) {
            size_t e = i;
            while (e < s.size() && IsDigit(static_cast<unsigned char>(s[e]))) ++e;
            bool ok = true;
            if (e < s.size() && s[e] == '.') {
                ++e;
                if (e < s.size() && IsDigit(static_cast<unsigned char>(s[e]))) {
                    while (e < s.size() && IsDigit(static_cast<unsigned char>(s[e]))) ++e;
                } else {
                    ok = false;   // trailing dot
                }
            }
            if (e < s.size() && (s[e] == '.' || IdentPartLen(s, e) != 0)) ok = false;   // 1..2, 1.5.2, 1abc
            if (!ok) e = badRunEnd(e);
            emit(ok ? Tk::Number : Tk::Unknown, e - i);
            continue;
        }
        if (c == '"') {
            size_t e = i + 1;
            std::string value;
            bool closed = false;
            while (e < s.size()) {
                if (s[e] == '"') {
                    if (e + 1 < s.size() && s[e + 1] == '"') {
                        value.push_back('"');
                        e += 2;
                        continue;
                    }
                    closed = true;
                    ++e;
                    break;
                }
                value.push_back(s[e]);
                ++e;
            }
            if (closed && IsValidUtf8NoNul(value)) emit(Tk::String, e - i, value);
            else emit(Tk::Unknown, e - i);
            continue;
        }
        if (c == '&') {
            const size_t n = IdentStartLen(s, i + 1);
            if (n == 0) {
                emit(Tk::Unknown, 1);
                continue;
            }
            size_t e = i + 1 + n;
            while (const size_t m = IdentPartLen(s, e)) e += m;
            emit(Tk::Parameter, e - i);
            continue;
        }
        if (c == '<') {
            const bool two = i + 1 < s.size() && (s[i + 1] == '>' || s[i + 1] == '=');
            emit(Tk::Op, two ? 2 : 1);
            continue;
        }
        if (c == '>') {
            const bool two = i + 1 < s.size() && s[i + 1] == '=';
            emit(Tk::Op, two ? 2 : 1);
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            emit(Tk::Unknown, 2);   // 1C query text has no block comments
            continue;
        }
        switch (c) {
            case '=': case '+': case '-': case '*': case '/': emit(Tk::Op, 1); continue;
            case '.': emit(Tk::Dot, 1); continue;
            case ',': emit(Tk::Comma, 1); continue;
            case '(': emit(Tk::LParen, 1); continue;
            case ')': emit(Tk::RParen, 1); continue;
            case ';': emit(Tk::Semicolon, 1); continue;
            default: break;
        }
        // Everything else is foreign: one whole UTF-8 sequence (or one byte) becomes a single Unknown token.
        size_t len = 1;
        if (c >= 0xC0) {
            while (i + len < s.size() && (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) ++len;
        }
        emit(Tk::Unknown, len);
    }

    Token end;
    end.kind = Tk::End;
    end.pos = s.size();
    end.line = line;
    end.col = col;
    out.push_back(std::move(end));
    return out;
}

}  // namespace lineage::strict
