#include "test_framework.h"
#include "bsl/bsl_tokenizer.h"

#include <string>
#include <vector>

using namespace lineage::bsl;

namespace {

bool HasUnknown(const std::string& src) {
    for (const auto& tok : Tokenize(src)) {
        if (tok.kind == Bt::Unknown) return true;
    }
    return false;
}

}  // namespace

TEST(BslTokenizer, MultilineStringWithPipes) {
    auto t = Tokenize("Т = \"ВЫБРАТЬ\n\t|  Поле\n\t|ИЗ Справочник.А\";");
    CHECK(t[2].kind == Bt::String);
    CHECK_EQ(t[2].value, std::string("ВЫБРАТЬ\n  Поле\nИЗ Справочник.А"));   // platform semantics of '|'
}

TEST(BslTokenizer, MultilineStringCrLf) {
    auto t = Tokenize("Т = \"а\r\n  |б\";");
    CHECK(t[2].kind == Bt::String);
    CHECK_EQ(t[2].value, std::string("а\nб"));
    CHECK(HasUnknown("Т = \"а\rб\";"));
}

TEST(BslTokenizer, DoubledQuoteAndDateLiteral) {
    auto t = Tokenize("А = \"x\"\"y\"; Д = '20240131';");
    CHECK_EQ(t[2].value, std::string("x\"y"));
    CHECK(t[6].kind == Bt::Date);
    CHECK_EQ(t[6].value, std::string("20240131"));
}

TEST(BslTokenizer, DateLiteralStrict) {
    CHECK(Tokenize("'20240131120000'")[0].kind == Bt::Date);
    CHECK(Tokenize("'202401311200'")[0].kind == Bt::Date);
    for (const char* bad : {"'2024-01-31'", "'2024'", "''", "'20240131", "'2024013a'", "'20240131\n'"}) {
        CHECK(HasUnknown(bad));
    }
}

TEST(BslTokenizer, CommentsAndPreprocessorAndLabels) {
    CHECK(Tokenize("// x\nА = 1;")[0].kind == Bt::Identifier);
    for (const char* bad : {"#Если Сервер Тогда", "&НаСервере", "~Метка:", "А = 1 /* x */;", "А = `x`;",
                            "|abc", "А = {1};", "А = !Б;"}) {
        CHECK(HasUnknown(bad));
    }
}

TEST(BslTokenizer, StringLineWithoutPipeIsUnknown) {
    CHECK(HasUnknown("Т = \"a\nb\";"));   // the platform rejects this too; we must not guess
    CHECK(HasUnknown("Т = \"a\n  // c\n |b\";"));
    CHECK(HasUnknown("Т = \"abc"));
}

TEST(BslTokenizer, CommentAfterClosingQuoteAndInsideString) {
    auto t = Tokenize("А = \"a // b\"; // tail\nБ = 2;");
    CHECK_EQ(t[2].value, std::string("a // b"));
    CHECK(!HasUnknown("А = \"a // b\"; // tail\nБ = 2;"));
    CHECK(t[4].kind == Bt::Identifier);
}

TEST(BslTokenizer, KeywordsBothLanguagesCaseInsensitive) {
    auto t = Tokenize("если IF Выполнить execute null НЕОПРЕДЕЛЕНО КонецПроцедуры");
    CHECK(t[0].kw == Bk::If && t[1].kw == Bk::If);
    CHECK(t[2].kind == Bt::Keyword && t[2].kw == Bk::Execute && t[3].kw == Bk::Execute);
    CHECK(t[4].kw == Bk::Null && t[5].kw == Bk::Undefined && t[6].kw == Bk::EndProcedure);
    CHECK(Tokenize("Новый")[0].kw == Bk::New);
    CHECK(Tokenize("Неопределенно")[0].kind == Bt::Identifier);
}

TEST(BslTokenizer, YoFoldedKeywordIsUnknown) {
    // "ИначеЕсли" spelled with Ё instead of Е matches a keyword only after folding.
    CHECK(HasUnknown("\xD0\x98\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\x81\xD1\x81\xD0\xBB\xD0\xB8"));
    CHECK(!HasUnknown("\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x86\xD0\xB0"));
}

TEST(BslTokenizer, OperatorsAndPunctuation) {
    auto t = Tokenize("А<>Б<=В>=Г<Д>Е%Ж; ?(А, Б, В) М[0].П");
    CHECK_EQ(t[1].text, std::string("<>"));
    CHECK_EQ(t[3].text, std::string("<="));
    CHECK_EQ(t[5].text, std::string(">="));
    CHECK(t[11].kind == Bt::Op && t[11].text == "%");
    CHECK(t[14].kind == Bt::Question && t[15].kind == Bt::LParen);
    CHECK(!HasUnknown("А<>Б<=В>=Г<Д>Е%Ж; ?(А, Б, В) М[0].П"));
}

TEST(BslTokenizer, Numbers) {
    CHECK(Tokenize("12.5")[0].kind == Bt::Number);
    for (const char* bad : {"1.", "1..2", "1.5.2", "1abc"}) CHECK(HasUnknown(bad));
}

TEST(BslTokenizer, ForeignCharactersAreUnknown) {
    CHECK(HasUnknown("\xC2\xA0" "А = 1;"));                 // NBSP
    CHECK(HasUnknown("А\xE2\x80\x8B" "Б"));                  // zero-width space inside a name
    CHECK(HasUnknown("\xEF\xBB\xBF" "А = 1;"));              // BOM
    CHECK(HasUnknown("\xCE\xB1 = 1;"));                      // Greek alpha (homoglyph)
    CHECK(HasUnknown(std::string("А = 1;\0Б = 2;", 13)));    // NUL
    CHECK(HasUnknown("А = 1;\x01"));                         // control
    CHECK(HasUnknown("А = 1;\f"));
}

TEST(BslTokenizer, InvalidUtf8IsUnknownEverywhere) {
    CHECK(HasUnknown("\xC0\xAF"));                // overlong
    CHECK(HasUnknown("\xED\xA0\x80"));            // surrogate
    CHECK(HasUnknown("\x80"));                    // stray continuation
    CHECK(HasUnknown("\xD0"));                    // truncated
    CHECK(HasUnknown("Т = \"\xC0\xAF\";"));       // inside a string
    CHECK(HasUnknown("Т = \"\xED\xA0\x80\";"));
    CHECK(HasUnknown("Т = \"\xD0\";"));
    CHECK(HasUnknown(std::string("Т = \"a\0b\";", 11)));
}

TEST(BslTokenizer, SuspiciousCommentIsUnknownNotDropped) {
    CHECK(HasUnknown("// x\rВыполнить(А)\n"));                  // bare CR could be a line end for the platform
    CHECK(HasUnknown("// x\xE2\x80\xA8" "Выполнить(А)\n"));      // U+2028
    CHECK(HasUnknown("// x\xC2\x85" "Выполнить(А)\n"));          // U+0085
    CHECK(HasUnknown("// x\xE2\x80\xAE" "y\n"));                 // bidi override
    CHECK(HasUnknown("// \xC0\xAF\n"));
    CHECK(!HasUnknown("// обычный комментарий\r\nА = 1;"));
}

TEST(BslTokenizer, BareCarriageReturnIsUnknown) {
    CHECK(HasUnknown("А = 1;\rБ = 2;"));
    CHECK(!HasUnknown("А = 1;\r\nБ = 2;"));
}

TEST(BslTokenizer, MixedScriptFlag) {
    auto t = Tokenize("Test\xD0\x90 Обычное Plain");
    CHECK(t[0].mixed_script);
    CHECK(!t[1].mixed_script);
    CHECK(!t[2].mixed_script);
}

TEST(BslTokenizer, PositionsInCodePoints) {
    auto t = Tokenize("Ааа = 1;\n  Б = 2;");
    CHECK_EQ(t[1].col, 5u);
    CHECK(t[4].line == 2 && t[4].col == 3);
}

TEST(BslTokenizer, SizeAndTokenLimits) {
    auto big = Tokenize(std::string(kMaxSourceBytes + 1, ' '));
    CHECK(big.size() == 2 && big[0].kind == Bt::Unknown && big[0].text == "<size>");
    std::string many;
    for (int i = 0; i < 200100; ++i) many += "; ";
    auto t = Tokenize(many);
    CHECK(t[t.size() - 2].kind == Bt::Unknown && t[t.size() - 2].text == "<limit>");
    CHECK(t.back().kind == Bt::End);
}

TEST(BslTokenizer, AlwaysEndsWithEnd) {
    CHECK(Tokenize("").back().kind == Bt::End);
    CHECK(Tokenize("А = \"").back().kind == Bt::End);
}
