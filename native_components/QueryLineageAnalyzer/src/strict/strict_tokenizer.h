#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lineage::strict {

enum class Tk { End, Identifier, String, Number, Parameter, Dot, Comma, LParen, RParen, Semicolon, Op, Keyword, Unknown };

enum class Kw {
    None, Select, Allowed, Distinct, Top, As, From, Into, Drop, Where, Group, Po, Having, Order, Asc, Desc,
    Autoorder, Totals, Overall, Hierarchy, Only, Periods, Union, All, Join, Left, Right, Inner, Full, Outer,
    And, Or, Not, In, Between, Like, Escape, Is, Null, True, False, Undefined, Case, When, Then, Else, End,
    Refs, Cast, For, Update, Index, Value, Type, ValueType, Presentation, RefPresentation, IsNull
};

// Sources longer than this are rejected as a single Unknown token with text "<size>".
constexpr size_t kMaxSourceBytes = 1024 * 1024;

// Parser/schema responsibilities (not decided here): identifiers mixing Latin and Cyrillic letters, and the
// two-word forms such as "В ИЕРАРХИИ" (tokenized as separate identifiers/keywords).
struct Token {
    Tk kind = Tk::End;
    Kw kw = Kw::None;
    std::string text;    // original spelling (for diagnostics only; never printed back)
    std::string value;   // unescaped string literal value
    size_t pos = 0;      // byte offset in the original text
    uint32_t line = 1;
    uint32_t col = 1;    // 1-based, counted in code points
};

// Fail-closed lexer: anything outside the strict lexical grammar becomes Tk::Unknown.
// The result always ends with a Tk::End token. When the token limit is exceeded, an Unknown token
// with text "<limit>" is appended before End.
std::vector<Token> Tokenize(const std::string& utf8);
const char* CanonicalKeyword(Kw kw);   // Russian canonical spelling, used by the printer

// True when the whole of `name` is spelled in the alphabet of an identifier and of a parameter after '&': a start
// character (ASCII letter, '_', Russian Cyrillic letter U+0410-U+044F, U+0401, U+0451), then start characters or ASCII digits.
// Empty names, spaces, invisible characters and look-alikes from other scripts are false.
bool IsIdentifierName(const std::string& name);

}  // namespace lineage::strict
