#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lineage::bsl {

enum class Bt {
    End, Identifier, Keyword, Number, String, Date, Op,
    LParen, RParen, LBracket, RBracket, Comma, Semicolon, Dot, Question, Unknown
};

enum class Bk {
    None, If, Then, ElsIf, Else, EndIf, For, Each, In, To, Do, EndDo, While, Break, Continue, Return, New,
    True, False, Undefined, Null, And, Or, Not, Try, Except, EndTry, Raise, Var, Procedure, Function,
    EndProcedure, EndFunction, Export, Val, Execute, Goto, AddHandler, RemoveHandler, Async, Await
};

// Sources longer than this are rejected as a single Unknown token with text "<size>".
constexpr size_t kMaxSourceBytes = 1024 * 1024;

// Parser responsibilities (not decided here): a Keyword token after a Dot (member access by a reserved
// word, e.g. "Объект.Выполнить") must be rejected or explicitly allowlisted; identifiers flagged
// mixed_script must not be matched against allowlists as if they were plain names.
struct Token {
    Bt kind = Bt::End;
    Bk kw = Bk::None;
    std::string text;     // original spelling (diagnostics only; never printed back)
    std::string value;    // String: decoded value ('|' continuation joined with "\n"); Date: the digits
    bool mixed_script = false;   // Identifier mixing ASCII letters and Cyrillic letters (homoglyph risk)
    size_t pos = 0;       // byte offset in the original text
    uint32_t line = 1;
    uint32_t col = 1;     // 1-based, counted in code points
};

// Fail-closed lexer: anything outside the accepted lexical grammar becomes Bt::Unknown. Block comments,
// preprocessor lines (#), directives (&), labels (~ and :), backticks, braces, stray '|' and '!' are Unknown.
// Comments (//) are dropped only when they are valid UTF-8 without control, line-separator, bidi or
// zero-width characters; otherwise the comment becomes one Unknown token. A bare CR (not part of CRLF)
// anywhere is Unknown, because line-end interpretation must not differ from the platform's.
// The result always ends with a Bt::End token. When the token limit is exceeded, an Unknown token
// with text "<limit>" is appended before End.
std::vector<Token> Tokenize(const std::string& utf8);
const char* CanonicalKeyword(Bk kw);   // Russian canonical spelling

}  // namespace lineage::bsl
