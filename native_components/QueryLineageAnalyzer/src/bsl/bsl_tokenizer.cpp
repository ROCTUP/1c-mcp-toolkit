#include "bsl/bsl_tokenizer.h"

#include <unordered_map>

#include "utf_utils.h"

namespace lineage::bsl {

namespace {

constexpr size_t kMaxTokens = 200000;

struct KwEntry {
    Bk kw;
    const char* ru;
    const char* en;   // nullptr when the English spelling equals the Russian one
};

const KwEntry kKeywords[] = {
    {Bk::If, "ЕСЛИ", "IF"},
    {Bk::Then, "ТОГДА", "THEN"},
    {Bk::ElsIf, "ИНАЧЕЕСЛИ", "ELSIF"},
    {Bk::Else, "ИНАЧЕ", "ELSE"},
    {Bk::EndIf, "КОНЕЦЕСЛИ", "ENDIF"},
    {Bk::For, "ДЛЯ", "FOR"},
    {Bk::Each, "КАЖДОГО", "EACH"},
    {Bk::In, "ИЗ", "IN"},
    {Bk::To, "ПО", "TO"},
    {Bk::Do, "ЦИКЛ", "DO"},
    {Bk::EndDo, "КОНЕЦЦИКЛА", "ENDDO"},
    {Bk::While, "ПОКА", "WHILE"},
    {Bk::Break, "ПРЕРВАТЬ", "BREAK"},
    {Bk::Continue, "ПРОДОЛЖИТЬ", "CONTINUE"},
    {Bk::Return, "ВОЗВРАТ", "RETURN"},
    {Bk::New, "НОВЫЙ", "NEW"},
    {Bk::True, "ИСТИНА", "TRUE"},
    {Bk::False, "ЛОЖЬ", "FALSE"},
    {Bk::Undefined, "НЕОПРЕДЕЛЕНО", "UNDEFINED"},
    {Bk::Null, "NULL", nullptr},
    {Bk::And, "И", "AND"},
    {Bk::Or, "ИЛИ", "OR"},
    {Bk::Not, "НЕ", "NOT"},
    {Bk::Try, "ПОПЫТКА", "TRY"},
    {Bk::Except, "ИСКЛЮЧЕНИЕ", "EXCEPT"},
    {Bk::EndTry, "КОНЕЦПОПЫТКИ", "ENDTRY"},
    {Bk::Raise, "ВЫЗВАТЬИСКЛЮЧЕНИЕ", "RAISE"},
    {Bk::Var, "ПЕРЕМ", "VAR"},
    {Bk::Procedure, "ПРОЦЕДУРА", "PROCEDURE"},
    {Bk::Function, "ФУНКЦИЯ", "FUNCTION"},
    {Bk::EndProcedure, "КОНЕЦПРОЦЕДУРЫ", "ENDPROCEDURE"},
    {Bk::EndFunction, "КОНЕЦФУНКЦИИ", "ENDFUNCTION"},
    {Bk::Export, "ЭКСПОРТ", "EXPORT"},
    {Bk::Val, "ЗНАЧ", "VAL"},
    {Bk::Execute, "ВЫПОЛНИТЬ", "EXECUTE"},
    {Bk::Goto, "ПЕРЕЙТИ", "GOTO"},
    {Bk::AddHandler, "ДОБАВИТЬОБРАБОТЧИК", "ADDHANDLER"},
    {Bk::RemoveHandler, "УДАЛИТЬОБРАБОТЧИК", "REMOVEHANDLER"},
    {Bk::Async, "АСИНХ", "ASYNC"},
    {Bk::Await, "ЖДАТЬ", "AWAIT"},
};

const std::unordered_map<std::string, Bk>& KeywordTable() {
    static const std::unordered_map<std::string, Bk> table = [] {
        std::unordered_map<std::string, Bk> t;
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

// Decodes one code point at s[i]; returns its byte length, or 0 for NUL / invalid UTF-8
// (overlong, surrogate, > U+10FFFF, truncated, stray continuation byte).
size_t DecodeUtf8(const std::string& s, size_t i, uint32_t* out) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0) return 0;
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

// Code points that must never be hidden inside a dropped comment: C1 controls, NBSP, soft hyphen,
// zero-width / bidi / line-separator / format characters, BOM.
bool IsSuspiciousInComment(uint32_t cp) {
    return (cp >= 0x80 && cp <= 0xA0) || cp == 0xAD || cp == 0x061C || cp == 0x180E ||
           (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
           (cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF || (cp >= 0xFFF0 && cp <= 0xFFFF) ||
           (cp >= 0xE0000 && cp <= 0xE0FFF);
}

// Valid UTF-8, no NUL (used for string values).
bool IsValidUtf8NoNul(const std::string& v) {
    size_t i = 0;
    while (i < v.size()) {
        uint32_t cp = 0;
        const size_t n = DecodeUtf8(v, i, &cp);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

// A comment [b, e) may be dropped only when it is plain text: valid UTF-8, no controls except TAB (and the
// CR of a CRLF line end), none of the suspicious code points.
bool IsCleanComment(const std::string& s, size_t b, size_t e) {
    size_t i = b;
    while (i < e) {
        uint32_t cp = 0;
        const size_t n = DecodeUtf8(s, i, &cp);
        if (n == 0 || i + n > e) return false;
        if (cp < 0x20 && cp != '\t') {
            if (!(cp == '\r' && i + 1 == e)) return false;
        }
        if (cp == 0x7F || IsSuspiciousInComment(cp)) return false;
        i += n;
    }
    return true;
}

std::string FoldYo(std::string key) {
    for (size_t p = 0; p + 1 < key.size(); ++p) {
        if (static_cast<unsigned char>(key[p]) == 0xD0 && static_cast<unsigned char>(key[p + 1]) == 0x81) {
            key[p + 1] = static_cast<char>(0x95);
        }
    }
    return key;
}

}  // namespace

const char* CanonicalKeyword(Bk kw) {
    for (const auto& e : kKeywords) {
        if (e.kw == kw) return e.ru;
    }
    return "";
}

std::vector<Token> Tokenize(const std::string& s) {
    std::vector<Token> out;
    if (s.size() > kMaxSourceBytes) {
        Token t;
        t.kind = Bt::Unknown;
        t.text = "<size>";
        out.push_back(std::move(t));
        out.push_back(Token());
        return out;
    }
    size_t i = 0;
    uint32_t line = 1;
    uint32_t col = 1;

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
    auto emit = [&](Bt kind, size_t len, const std::string& value = std::string()) -> Token& {
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
        if (c == ' ' || c == '\t' || c == '\n') {
            advance(i + 1);
            continue;
        }
        if (c == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') {
                advance(i + 1);
                continue;
            }
            emit(Bt::Unknown, 1);   // bare CR: line-end interpretation could differ from the platform's
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            size_t e = i;
            while (e < s.size() && s[e] != '\n') ++e;
            if (IsCleanComment(s, i, e)) {
                advance(e);
            } else {
                emit(Bt::Unknown, e - i);
            }
            continue;
        }
        if (out.size() >= kMaxTokens) {
            Token t;
            t.kind = Bt::Unknown;
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
                emit(Bt::Unknown, e - i);   // ё/е folding would make it a keyword: ambiguous, fail closed
                continue;
            }
            bool ascii = false;
            bool cyr = false;
            for (size_t p = i; p < e; ++p) {
                const unsigned char ch = static_cast<unsigned char>(s[p]);
                if (ch >= 0xC0) cyr = true;
                else if (ch < 0x80 && !IsDigit(ch) && ch != '_') ascii = true;
            }
            Token& t = emit(it == table.end() ? Bt::Identifier : Bt::Keyword, e - i);
            if (it != table.end()) t.kw = it->second;
            t.mixed_script = ascii && cyr;
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
            emit(ok ? Bt::Number : Bt::Unknown, e - i);
            continue;
        }
        if (c == '"') {
            // Platform string: "" is a quote; a newline must be followed by optional blanks and '|'.
            size_t e = i + 1;
            std::string value;
            bool closed = false;
            bool bad = false;
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
                if (s[e] == '\r' || s[e] == '\n') {
                    size_t nl = e;
                    if (s[nl] == '\r') {
                        if (nl + 1 >= s.size() || s[nl + 1] != '\n') { bad = true; break; }
                        ++nl;
                    }
                    size_t p = nl + 1;
                    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
                    if (p >= s.size() || s[p] != '|') { bad = true; break; }
                    value.push_back('\n');
                    e = p + 1;
                    continue;
                }
                value.push_back(s[e]);
                ++e;
            }
            if (closed && !bad && IsValidUtf8NoNul(value)) emit(Bt::String, e - i, value);
            else emit(Bt::Unknown, e - i);
            continue;
        }
        if (c == '\'') {
            // Date literal: only digits, 8 (date), 12 (date + h:m) or 14 (date + h:m:s) of them.
            size_t e = i + 1;
            while (e < s.size() && IsDigit(static_cast<unsigned char>(s[e]))) ++e;
            const size_t digits = e - i - 1;
            if (e < s.size() && s[e] == '\'' && (digits == 8 || digits == 12 || digits == 14)) {
                emit(Bt::Date, e + 1 - i, s.substr(i + 1, digits));
            } else {
                while (e < s.size() && s[e] != '\'' && s[e] != '\n' && s[e] != '\r') ++e;
                if (e < s.size() && s[e] == '\'') ++e;
                emit(Bt::Unknown, e - i);
            }
            continue;
        }
        if (c == '<') {
            const bool two = i + 1 < s.size() && (s[i + 1] == '>' || s[i + 1] == '=');
            emit(Bt::Op, two ? 2 : 1);
            continue;
        }
        if (c == '>') {
            const bool two = i + 1 < s.size() && s[i + 1] == '=';
            emit(Bt::Op, two ? 2 : 1);
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            emit(Bt::Unknown, 2);   // BSL has no block comments
            continue;
        }
        switch (c) {
            case '=': case '+': case '-': case '*': case '/': case '%': emit(Bt::Op, 1); continue;
            case '.': emit(Bt::Dot, 1); continue;
            case ',': emit(Bt::Comma, 1); continue;
            case '(': emit(Bt::LParen, 1); continue;
            case ')': emit(Bt::RParen, 1); continue;
            case '[': emit(Bt::LBracket, 1); continue;
            case ']': emit(Bt::RBracket, 1); continue;
            case ';': emit(Bt::Semicolon, 1); continue;
            case '?': emit(Bt::Question, 1); continue;
            default: break;
        }
        // Everything else (#, &, ~, :, |, backtick, braces, !, foreign letters, NBSP, BOM, controls, invalid
        // bytes) is foreign: one whole UTF-8 sequence (or one byte) becomes a single Unknown token.
        size_t len = 1;
        if (c >= 0xC0) {
            while (i + len < s.size() && (static_cast<unsigned char>(s[i + len]) & 0xC0) == 0x80) ++len;
        }
        emit(Bt::Unknown, len);
    }

    Token end;
    end.kind = Bt::End;
    end.pos = s.size();
    end.line = line;
    end.col = col;
    out.push_back(std::move(end));
    return out;
}

}  // namespace lineage::bsl
