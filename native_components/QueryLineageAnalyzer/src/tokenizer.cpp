#include "tokenizer.h"
#include "utf_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>

namespace lineage {

namespace {

bool IsIdentifierByte(unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == '#' || ch >= 0x80;
}

const std::unordered_map<std::string, TokenKind>& KeywordMap() {
    static const std::unordered_map<std::string, TokenKind> keywords = {
        {"SELECT", TokenKind::KeywordSelect}, {"ВЫБРАТЬ", TokenKind::KeywordSelect},
        {"FROM", TokenKind::KeywordFrom}, {"ИЗ", TokenKind::KeywordFrom},
        {"AS", TokenKind::KeywordAs}, {"КАК", TokenKind::KeywordAs},
        {"LEFT", TokenKind::KeywordLeft}, {"ЛЕВОЕ", TokenKind::KeywordLeft},
        {"RIGHT", TokenKind::KeywordRight}, {"ПРАВОЕ", TokenKind::KeywordRight},
        {"INNER", TokenKind::KeywordInner}, {"ВНУТРЕННЕЕ", TokenKind::KeywordInner},
        {"FULL", TokenKind::KeywordFull}, {"ПОЛНОЕ", TokenKind::KeywordFull},
        {"JOIN", TokenKind::KeywordJoin}, {"СОЕДИНЕНИЕ", TokenKind::KeywordJoin},
        {"UNION", TokenKind::KeywordUnion}, {"ОБЪЕДИНИТЬ", TokenKind::KeywordUnion},
        {"ALL", TokenKind::KeywordAll}, {"ВСЕ", TokenKind::KeywordAll},
        {"INTO", TokenKind::KeywordInto}, {"ПОМЕСТИТЬ", TokenKind::KeywordInto},
        {"DESTROY", TokenKind::KeywordDestroy}, {"УНИЧТОЖИТЬ", TokenKind::KeywordDestroy},
        {"ISNULL", TokenKind::KeywordIsNull}, {"ЕСТЬNULL", TokenKind::KeywordIsNull},
        {"CAST", TokenKind::KeywordCast}, {"ВЫРАЗИТЬ", TokenKind::KeywordCast},
        {"PRESENTATION", TokenKind::KeywordPresentation}, {"ПРЕДСТАВЛЕНИЕ", TokenKind::KeywordPresentation},
        {"CASE", TokenKind::KeywordCase}, {"ВЫБОР", TokenKind::KeywordCase},
        {"WHEN", TokenKind::KeywordWhen}, {"КОГДА", TokenKind::KeywordWhen},
        {"THEN", TokenKind::KeywordThen}, {"ТОГДА", TokenKind::KeywordThen},
        {"ELSE", TokenKind::KeywordElse}, {"ИНАЧЕ", TokenKind::KeywordElse},
        {"END", TokenKind::KeywordEnd}, {"КОНЕЦ", TokenKind::KeywordEnd},
        {"DISTINCT", TokenKind::KeywordDistinct}, {"РАЗЛИЧНЫЕ", TokenKind::KeywordDistinct},
        {"ALLOWED", TokenKind::KeywordAllowed}, {"РАЗРЕШЕННЫЕ", TokenKind::KeywordAllowed},
        {"TOP", TokenKind::KeywordTop}, {"ПЕРВЫЕ", TokenKind::KeywordTop},
        {"WHERE", TokenKind::KeywordWhere}, {"ГДЕ", TokenKind::KeywordWhere},
        {"GROUP", TokenKind::KeywordGroup}, {"СГРУППИРОВАТЬ", TokenKind::KeywordGroup},
        {"ORDER", TokenKind::KeywordOrder}, {"УПОРЯДОЧИТЬ", TokenKind::KeywordOrder},
        {"ПО", TokenKind::KeywordPo}, {"BY", TokenKind::KeywordBy}, {"ON", TokenKind::KeywordOn},
        {"HAVING", TokenKind::KeywordHaving}, {"ИМЕЮЩИЕ", TokenKind::KeywordHaving},
        {"TOTALS", TokenKind::KeywordTotals}, {"ИТОГИ", TokenKind::KeywordTotals},
        {"AND", TokenKind::KeywordAnd}, {"И", TokenKind::KeywordAnd},
        {"OR", TokenKind::KeywordOr}, {"ИЛИ", TokenKind::KeywordOr},
        {"NOT", TokenKind::KeywordNot}, {"НЕ", TokenKind::KeywordNot},
        {"IN", TokenKind::KeywordIn}, {"В", TokenKind::KeywordIn},
        {"NULL", TokenKind::KeywordNull}, {"TRUE", TokenKind::KeywordTrue}, {"ИСТИНА", TokenKind::KeywordTrue},
        {"FALSE", TokenKind::KeywordFalse}, {"ЛОЖЬ", TokenKind::KeywordFalse},
        {"REFS", TokenKind::KeywordRefs}, {"ССЫЛКА", TokenKind::KeywordRefs},
    };
    return keywords;
}

}  // namespace

std::vector<Token> Tokenize(const std::string& query) {
    std::vector<Token> tokens;
    tokens.reserve(query.size() / 3 + 8);

    size_t i = 0;
    while (i < query.size()) {
        unsigned char ch = static_cast<unsigned char>(query[i]);
        if (std::isspace(ch)) {
            ++i;
            continue;
        }
        // Strings are consumed in full below, so this branch only sees comments
        // outside literals. Keep offsets into the original query unchanged.
        if (ch == '/' && i + 1 < query.size() && query[i + 1] == '/') {
            i += 2;
            while (i < query.size() && query[i] != '\r' && query[i] != '\n') ++i;
            continue;
        }
        if (ch == '.') { tokens.push_back({TokenKind::Dot, ".", i++}); continue; }
        if (ch == ',') { tokens.push_back({TokenKind::Comma, ",", i++}); continue; }
        if (ch == '(') { tokens.push_back({TokenKind::LParen, "(", i++}); continue; }
        if (ch == ')') { tokens.push_back({TokenKind::RParen, ")", i++}); continue; }
        if (ch == ';') { tokens.push_back({TokenKind::Semicolon, ";", i++}); continue; }
        if (ch == '&') {
            size_t start = i++;
            while (i < query.size() && IsIdentifierByte(static_cast<unsigned char>(query[i]))) ++i;
            tokens.push_back({TokenKind::Parameter, query.substr(start, i - start), start});
            continue;
        }
        if (ch == '\'' || ch == '"') {
            size_t start = i;
            unsigned char quote = ch;
            ++i;
            while (i < query.size()) {
                unsigned char cur = static_cast<unsigned char>(query[i]);
                if (cur == quote) {
                    ++i;
                    if (i < query.size() && static_cast<unsigned char>(query[i]) == quote) {
                        ++i;
                        continue;
                    }
                    break;
                }
                ++i;
            }
            tokens.push_back({TokenKind::StringLiteral, query.substr(start, i - start), start});
            continue;
        }
        if (std::isdigit(ch)) {
            size_t start = i++;
            while (i < query.size()) {
                unsigned char cur = static_cast<unsigned char>(query[i]);
                if (!(std::isdigit(cur) || cur == '.')) break;
                ++i;
            }
            tokens.push_back({TokenKind::NumberLiteral, query.substr(start, i - start), start});
            continue;
        }
        if (std::strchr("+-*/=%<>", ch) != nullptr) {
            size_t start = i++;
            if (i < query.size()) {
                unsigned char next = static_cast<unsigned char>(query[i]);
                if ((ch == '<' || ch == '>') && (next == '=' || next == '>')) ++i;
            }
            tokens.push_back({TokenKind::Operator, query.substr(start, i - start), start});
            continue;
        }

        size_t start = i;
        while (i < query.size() && IsIdentifierByte(static_cast<unsigned char>(query[i]))) ++i;
        if (i == start) {
            tokens.push_back({TokenKind::Operator, query.substr(i, 1), i});
            ++i;
            continue;
        }
        std::string text = query.substr(start, i - start);
        std::string upper = NormalizeIdentifierKey(text);
        auto it = KeywordMap().find(upper);
        if (it != KeywordMap().end()) {
            tokens.push_back({it->second, text, start});
        } else {
            tokens.push_back({TokenKind::Identifier, text, start});
        }
    }

    tokens.push_back({TokenKind::End, "", query.size()});
    return tokens;
}

bool IsKeyword(TokenKind kind) {
    return kind >= TokenKind::KeywordSelect && kind <= TokenKind::KeywordRefs;
}

}  // namespace lineage
