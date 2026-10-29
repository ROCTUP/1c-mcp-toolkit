#include "test_framework.h"
#include "strict/strict_tokenizer.h"

#include <string>
#include <vector>

using namespace lineage::strict;

namespace {

std::vector<Tk> Kinds(const std::string& src) {
    std::vector<Tk> k;
    for (const auto& t : Tokenize(src)) k.push_back(t.kind);
    return k;
}

bool Same(const std::vector<Tk>& a, const std::vector<Tk>& b) { return a == b; }

}  // namespace

TEST(StrictTokenizer, KeywordsBothLanguagesCaseInsensitive) {
    auto t = Tokenize("выбрать SELECT Из from ПО by ON");
    CHECK(t[0].kind == Tk::Keyword && t[0].kw == Kw::Select);
    CHECK(t[1].kw == Kw::Select);
    CHECK(t[2].kw == Kw::From && t[3].kw == Kw::From);
    CHECK(t[4].kw == Kw::Po && t[5].kw == Kw::Po && t[6].kw == Kw::Po);
}

TEST(StrictTokenizer, StringWithDoubledQuote) {
    auto t = Tokenize("\"a\"\"b\"");
    CHECK(t[0].kind == Tk::String);
    CHECK_EQ(t[0].value, std::string("a\"b"));   // unescaped value
    CHECK(t[1].kind == Tk::End);
}

TEST(StrictTokenizer, UnterminatedStringIsUnknown) {
    CHECK(Tokenize("\"abc")[0].kind == Tk::Unknown);
}

TEST(StrictTokenizer, CommentDroppedButNotInsideString) {
    auto t = Tokenize("\"//x\" // tail\n1");
    CHECK(t[0].kind == Tk::String && t[0].value == "//x");
    CHECK(t[1].kind == Tk::Number);
}

TEST(StrictTokenizer, ForeignCharactersAreUnknown) {
    const Tk I = Tk::Identifier, U = Tk::Unknown, E = Tk::End;
    CHECK(Same(Kinds("'x'"), {U, I, U, E}));
    CHECK(Same(Kinds("{Поле}"), {U, I, U, E}));
    CHECK(Same(Kinds("a|b"), {I, U, I, E}));
    CHECK(Same(Kinds("a\xC2\xA0" "b"), {I, U, I, E}));
    CHECK(Same(Kinds("a\xE2\x80\x8B" "b"), {I, U, I, E}));
    for (const char* s : {"\\", "@", "?", "$"}) CHECK(Same(Kinds(s), {U, E}));
}

TEST(StrictTokenizer, YoFoldingAmbiguityIsUnknown) {
    // "ПЁРВЫЕ" would equal ПЕРВЫЕ (TOP) after ё->е folding, but is not a keyword as spelled.
    CHECK(Tokenize("ПЁРВЫЕ")[0].kind == Tk::Unknown);
    CHECK(Tokenize("пёрвые")[0].kind == Tk::Unknown);
    CHECK(Tokenize("Расчёты")[0].kind == Tk::Identifier);
}

TEST(StrictTokenizer, StringUtf8AndNulValidation) {
    for (const char* s : {"\"a\xC0\x80\"", "\"a\xED\xA0\x80\"", "\"a\x80\"", "\"a\xF4\x90\x80\x80\"",
                          "\"a\xD0\"", "\"a\xE2\x82\""}) {
        CHECK(Tokenize(s)[0].kind == Tk::Unknown);
    }
    CHECK(Tokenize(std::string("\"a\0b\"", 5))[0].kind == Tk::Unknown);
    CHECK(Tokenize("\"Привет \xE2\x82\xAC\"")[0].kind == Tk::String);
}

TEST(StrictTokenizer, InvalidBytesOutsideStringsAreUnknown) {
    CHECK(Tokenize("\xC0\x80")[0].kind == Tk::Unknown);
    CHECK(Tokenize("\xED\xA0\x80")[0].kind == Tk::Unknown);
    CHECK(Tokenize("\x80")[0].kind == Tk::Unknown);
    CHECK(Tokenize("a \xD0")[1].kind == Tk::Unknown);   // truncated tail sequence
    CHECK(Tokenize("\xEF\xBB\xBF" "a")[0].kind == Tk::Unknown);   // BOM
    CHECK(Tokenize(std::string("\0", 1))[0].kind == Tk::Unknown);
    CHECK(Tokenize(std::string("a\0b", 3))[1].kind == Tk::Unknown);
}

TEST(StrictTokenizer, HomoglyphAndFullwidthAndNumberForms) {
    CHECK(Tokenize("SELECТ")[0].kind == Tk::Identifier);   // Cyrillic Т
    CHECK(Tokenize("\xEF\xBC\x91")[0].kind == Tk::Unknown);   // fullwidth digit 1
    CHECK(Tokenize("1e5")[0].kind == Tk::Unknown);
    CHECK(Tokenize("1.5.2")[0].kind == Tk::Unknown);
}

TEST(StrictTokenizer, OperatorEdgeCases) {
    const Tk U = Tk::Unknown, O = Tk::Op, E = Tk::End;
    CHECK(Same(Kinds("<>="), {O, O, E}));
    CHECK(Same(Kinds("!="), {U, O, E}));
    CHECK(Same(Kinds("||"), {U, U, E}));
}

TEST(StrictTokenizer, CommentsAndLineEndings) {
    auto s = Tokenize("\"/* x */\"");
    CHECK(s[0].kind == Tk::String && s[0].value == "/* x */");
    auto c = Tokenize("a // tail");
    CHECK(c.size() == 2 && c[1].kind == Tk::End);
    auto t = Tokenize("a\r\nb");
    CHECK_EQ(t[1].line, 2u);
    CHECK_EQ(t[1].col, 1u);
}

TEST(StrictTokenizer, SizeCap) {
    auto t = Tokenize(std::string(kMaxSourceBytes + 1, ' '));
    CHECK(t.size() == 2 && t[0].kind == Tk::Unknown && t[0].text == "<size>" && t[1].kind == Tk::End);
    CHECK(Tokenize(std::string(kMaxSourceBytes, ' ')).size() == 1);
}

TEST(StrictTokenizer, OperatorsAndParameters) {
    auto t = Tokenize("&П1 <> 1 <= 2 >= 3 = 4 < 5 > 6 + - * /");
    CHECK(t[0].kind == Tk::Parameter && t[0].text == "&П1");
    CHECK(t[1].kind == Tk::Op && t[1].text == "<>");
    CHECK(t[3].text == "<=" && t[5].text == ">=");
}

TEST(StrictTokenizer, PositionsLineAndColumn) {
    auto t = Tokenize("ВЫБРАТЬ\n  Поле");
    CHECK_EQ(t[1].line, 2u);
    CHECK_EQ(t[1].col, 3u);
}

TEST(StrictTokenizer, NumbersStrict) {
    CHECK(Tokenize("1.5")[0].kind == Tk::Number);
    CHECK(Tokenize("1..2")[0].kind == Tk::Unknown);   // no lenient "digits and dots"
}

TEST(StrictTokenizer, BlockCommentTrailingDotAndBareAmpersandAreUnknown) {
    CHECK(Tokenize("/* x */")[0].kind == Tk::Unknown);
    CHECK(Tokenize("1.")[0].kind == Tk::Unknown);
    CHECK(Tokenize("& x")[0].kind == Tk::Unknown);
}

TEST(StrictTokenizer, SpecialKeywordsAndPunctuation) {
    auto t = Tokenize("ЕСТЬNULL isnull УНИЧТОЖИТЬ drop КОЛИЧЕСТВО(a.b, c);");
    CHECK(t[0].kw == Kw::IsNull && t[1].kw == Kw::IsNull);
    CHECK(t[2].kw == Kw::Drop && t[3].kw == Kw::Drop);
    CHECK(t[4].kind == Tk::Identifier);
    CHECK(t[5].kind == Tk::LParen && t[7].kind == Tk::Dot && t[9].kind == Tk::Comma);
    CHECK(t[11].kind == Tk::RParen && t[12].kind == Tk::Semicolon && t[13].kind == Tk::End);
}

TEST(StrictTokenizer, TokenLimitBoundary) {
    std::string src;
    for (int i = 0; i < 200000; ++i) src += "a ";
    auto t = Tokenize(src);
    CHECK_EQ(t.size(), static_cast<size_t>(200001));
    for (const auto& tok : t) CHECK(tok.kind != Tk::Unknown);
    src += "a ";
    auto u = Tokenize(src);
    bool limit = false;
    for (const auto& tok : u) limit = limit || (tok.kind == Tk::Unknown && tok.text == "<limit>");
    CHECK(limit);
    CHECK(u.back().kind == Tk::End);
}
