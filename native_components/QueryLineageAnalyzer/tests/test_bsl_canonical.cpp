#include "test_framework.h"
#include "bsl/bsl_canonical.h"
#include "bsl/bsl_parser.h"

#include <memory>
#include <string>

using namespace lineage::bsl;

namespace {

Body ParseOk(const std::string& src) {
    const ParseResult r = ParseBsl(src);
    if (!r.ok) qla_test::Fail("test source does not parse: " + r.error.code + " / " + r.error.detail, __FILE__, __LINE__);
    return r.body;
}

// Print(Parse(x)) parses again to the same tree and is a fixed point.
void CheckRoundTrip(const std::string& src) {
    const Body b = ParseOk(src);
    const PrintResult p = PrintBslCanonicalChecked(b);
    if (!p.ok) qla_test::Fail("refused: " + p.error.code + " / " + p.error.detail + "\nsource: " + src, __FILE__, __LINE__);
    const ParseResult again = ParseBsl(p.text);
    CHECK(again.ok);
    CHECK_EQ(DumpBsl(again.body), DumpBsl(b));
    const PrintResult p2 = PrintBslCanonicalChecked(again.body);
    CHECK(p2.ok);
    CHECK_EQ(p2.text, p.text);
}

std::string PrintedOrEmpty(const std::string& src) { return PrintBslCanonical(ParseOk(src)); }

ExprP Mk(Expr::K k, const std::string& text = std::string(), std::vector<ExprP> args = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = k;
    e->text = text;
    e->args = std::move(args);
    return e;
}

ExprP StrLit(const std::string& v) {
    ExprP e = Mk(Expr::K::Lit, v);
    e->lit = Expr::L::String;
    return e;
}

Body AssignBody(ExprP value) {
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::K::Assign;
    s->exprs = {Mk(Expr::K::Var, "Результат"), std::move(value)};
    Body b;
    b.stmts.push_back(s);
    return b;
}

std::string CodeOfPrint(const Body& b, const PrintOptions& o = PrintOptions()) {
    const PrintResult p = PrintBslCanonicalChecked(b, o);
    return p.ok ? std::string("OK") : p.error.code;
}

}  // namespace

TEST(BslCanonical, RoundTripStatements) {
    CheckRoundTrip("А = 1; Б = А + 2 * 3;");
    CheckRoundTrip("Если А > 1 Тогда Б = 1; ИначеЕсли А = 0 Тогда Б = 2; Иначе Б = 3; КонецЕсли;");
    CheckRoundTrip("Для Каждого С Из Список Цикл Если С = 1 Тогда Продолжить; КонецЕсли; Прервать; КонецЦикла;");
    CheckRoundTrip("Для Инд = 1 По 10 Цикл Х = Х + Инд; КонецЦикла;");
    CheckRoundTrip("Пока Истина Цикл Прервать; КонецЦикла;");
    CheckRoundTrip("Попытка А = 1; Исключение Б = 2; КонецПопытки;");
    CheckRoundTrip("Если А Тогда КонецЕсли;");
    CheckRoundTrip("");
    CheckRoundTrip("// only a comment\nА = 1; // trailing\n");
}

TEST(BslCanonical, RoundTripExpressions) {
    CheckRoundTrip("Р = (А + Б) * В - Г / (Д % Е);");
    CheckRoundTrip("Р = А - (Б - В);");
    CheckRoundTrip("Р = А - Б - В;");
    CheckRoundTrip("Р = -А * Б;");
    CheckRoundTrip("Р = -(А * Б);");
    CheckRoundTrip("Р = - -А;");
    CheckRoundTrip("Р = НЕ А = Б;");
    CheckRoundTrip("Р = (НЕ А) = Б;");
    CheckRoundTrip("Р = НЕ НЕ А;");
    CheckRoundTrip("Р = А ИЛИ Б И В ИЛИ Г;");
    CheckRoundTrip("Р = (А ИЛИ Б) И (В ИЛИ Г);");
    CheckRoundTrip("Р = (А = Б) = (В <> Г);");
    CheckRoundTrip("Р = А < Б ИЛИ А >= В;");
    CheckRoundTrip("Р = ?(А > 1, ?(Б, 1, 2), (В + 1) * 2);");
    CheckRoundTrip("Р = ?(А, Б, В) + 1;");
    CheckRoundTrip("Р = ?(А, Б, В).Свойство;");
    CheckRoundTrip("Р = Новый Массив + 1;");
    CheckRoundTrip("Р = Новый Структура(\"А\", Новый Массив);");
    CheckRoundTrip("Р = Т[0].Колонка[А + 1].Метод(1, \"x\", '20240131', NULL, Неопределено, Ложь);");
    CheckRoundTrip("Р = (А + Б).Метод();");
    CheckRoundTrip("Р = \"abc\".Метод() + (1).Метод();");
    CheckRoundTrip("Р = 1.5 + 0.25;");
    CheckRoundTrip("Р = '20240131120000';");
    CheckRoundTrip("А.Б.В = Г(Д)[1];");
}

TEST(BslCanonical, FixedLayout) {
    CHECK_EQ(PrintedOrEmpty("Если А Тогда Б = 1; КонецЕсли;"), "Если А Тогда\n    Б = 1;\nКонецЕсли;\n");
    CHECK_EQ(PrintedOrEmpty("if a then b = a+1; endif"), "Если a Тогда\n    b = a + 1;\nКонецЕсли;\n");
    CHECK_EQ(PrintedOrEmpty("Р = (А + Б) + В;"), "Р = А + Б + В;\n");
    CHECK_EQ(PrintedOrEmpty("Р = А + (Б + В);"), "Р = А + (Б + В);\n");
    CHECK_EQ(PrintedOrEmpty("Р = А + Б * В;"), "Р = А + (Б * В);\n");
    CHECK_EQ(PrintedOrEmpty("Р = НЕ А = Б;"), "Р = НЕ (А = Б);\n");
    CHECK_EQ(PrintedOrEmpty("Р = -А * Б;"), "Р = (-А) * Б;\n");
}

TEST(BslCanonical, LongAdditiveChainDoesNotNestParentheses) {
    std::string src = "Р = А";
    for (int i = 0; i < 150; ++i) src += " + \"x\"";
    src += ";";
    CheckRoundTrip(src);
}

TEST(BslCanonical, KeywordMemberNames) {
    CheckRoundTrip("Р = З.Выполнить().Выбрать();");
    CheckRoundTrip("З.Выполнить();");
    CheckRoundTrip("Р = Объект.Если + Объект.Новый;");
    CHECK_EQ(PrintedOrEmpty("Р = З.ВЫПОЛНИТЬ();"), "Р = З.ВЫПОЛНИТЬ();\n");
}

TEST(BslCanonical, Strings) {
    CheckRoundTrip("Р = \"a\"\"b\";");
    CheckRoundTrip("Р = \"\";");
    CheckRoundTrip("Р = \"line1\n|line2\n|\n|line4\";");
    CheckRoundTrip("Р = \"a\n|  indented\n||pipe\";");
    CheckRoundTrip("Р = \"tab\there\";");
    CHECK_EQ(PrintedOrEmpty("Р = \"a\n   |b\";"), "Р = \"a\n|b\";\n");
    CheckRoundTrip("Р = \"привет, мир\";");
}

TEST(BslCanonical, RefusesUnsafeStringValues) {
    const char* bad[] = {
        "a\x01z", "a\rz", "a\x7f" "z", "a\xe2\x80\xae" "z",   // control, CR, DEL, RLO
        "a\xe2\x80\x8b" "z", "a\xe2\x80\xa8" "z",               // zero-width space, line separator
        "a\xef\xbb\xbf" "z", "a\xc2\xa0" "z", "a\xc3\x28z",     // BOM, NBSP, invalid UTF-8
        "a\xe2\x81\xa6" "z",                                     // isolate
    };
    for (const char* v : bad) {
        const Body b = AssignBody(StrLit(v));
        CHECK_EQ(CodeOfPrint(b), std::string("C_PRINT_STRING"));
        CHECK(PrintBslCanonical(b).empty());
    }
}

TEST(BslCanonical, RefusesInvalidTrees) {
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Unset))), std::string("C_INTERNAL"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Var, "HTTPЗапрос"))), std::string("C_INTERNAL"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Var, "Если"))), std::string("C_INTERNAL"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Call, "Вычислить", {StrLit("1")}))), std::string("C_INTERNAL"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Binary, "&&", {StrLit("a"), StrLit("b")}))), std::string("C_INTERNAL"));
    ExprP bad_num = Mk(Expr::K::Lit, "1e5");
    bad_num->lit = Expr::L::Number;
    CHECK_EQ(CodeOfPrint(AssignBody(bad_num)), std::string("C_INTERNAL"));
    ExprP bad_date = Mk(Expr::K::Lit, "2024");
    bad_date->lit = Expr::L::Date;
    CHECK_EQ(CodeOfPrint(AssignBody(bad_date)), std::string("C_INTERNAL"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Member, "x y", {Mk(Expr::K::Var, "А")}))), std::string("C_INTERNAL"));
    CHECK(PrintBslCanonical(AssignBody(Mk(Expr::K::Unset))).empty());
}

TEST(BslCanonical, RefusesWhatTheParserCannotReadBack) {
    // Member access on "Новый ..." has no textual form the parser accepts.
    ExprP n = Mk(Expr::K::New, "Массив");
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Member, "Размер", {n}))), std::string("C_PRINT_REPARSE"));
    // A statement that would start with a string literal.
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::K::CallStmt;
    s->exprs = {Mk(Expr::K::MethodCall, "Метод", {StrLit("a")})};
    Body b;
    b.stmts.push_back(s);
    CHECK_EQ(CodeOfPrint(b), std::string("C_PRINT_REPARSE"));
}

TEST(BslCanonical, ChainOnNewQueryPrintsWithoutParentheses) {
    CheckRoundTrip("А = Новый Запрос(\"ВЫБРАТЬ 1\").Выполнить().Выгрузить();");
    CHECK_EQ(PrintedOrEmpty("А = (Новый Запрос(\"ВЫБРАТЬ 1\")).Выполнить();"),
             std::string("А = Новый Запрос(\"ВЫБРАТЬ 1\").Выполнить();\n"));
    // A hand-built index on "Новый Запрос(...)" has no textual form the parser accepts.
    ExprP n = Mk(Expr::K::New, "Запрос");
    n->has_parens = true;
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Index, "", {n, Mk(Expr::K::Var, "Б")}))), std::string("C_PRINT_REPARSE"));
    ExprP bare = Mk(Expr::K::New, "Запрос");
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::MethodCall, "Выполнить", {bare}))), std::string("C_PRINT_REPARSE"));
}

TEST(BslCanonical, DepthAndSizeCaps) {
    ExprP e = Mk(Expr::K::Var, "А");
    for (int i = 0; i < 300; ++i) e = Mk(Expr::K::Unary, "-", {e});
    CHECK_EQ(CodeOfPrint(AssignBody(e)), std::string("C_LIMIT"));
    // 20 nested negations print (parenthesised) within the caps.
    ExprP ok = Mk(Expr::K::Var, "А");
    for (int i = 0; i < 20; ++i) ok = Mk(Expr::K::Unary, "-", {ok});
    CHECK_EQ(CodeOfPrint(AssignBody(ok)), std::string("OK"));
}

TEST(BslCanonical, QueryReplacementUsesConcatenation) {
    const Body b = ParseOk("Запрос.Текст = \"x\"; Результат = 1;");
    PrintOptions o;
    o.query_replacements[b.stmts[0]->exprs[1].get()] = "ВЫБРАТЬ\n    1 КАК А\nИЗ\n    Т \"q\"";
    const PrintResult p = PrintBslCanonicalChecked(b, o);
    CHECK(p.ok);
    CHECK(p.text.find('|') == std::string::npos);
    CHECK_EQ(p.text,
             "Запрос.Текст = \"ВЫБРАТЬ\" + (Символы.ПС + \"    1 КАК А\") + (Символы.ПС + \"ИЗ\" + (Символы.ПС + \"    Т \"\"q\"\"\"));\n"
             "Результат = 1;\n");
    CHECK(ParseBsl(p.text, true).ok);
}

TEST(BslCanonical, QueryReplacementEdgeCases) {
    const Body b = ParseOk("Т = \"x\";");
    PrintOptions o;
    o.query_replacements[b.stmts[0]->exprs[1].get()] = "";
    CHECK_EQ(PrintBslCanonical(b, o), "Т = \"\";\n");
    o.query_replacements[b.stmts[0]->exprs[1].get()] = "\nA\n";
    CHECK_EQ(PrintBslCanonical(b, o), "Т = Символы.ПС + (\"A\" + Символы.ПС);\n");
    o.query_replacements[b.stmts[0]->exprs[1].get()] = "a\rb";
    CHECK_EQ(CodeOfPrint(b, o), std::string("C_PRINT_STRING"));
    o.query_replacements[b.stmts[0]->exprs[1].get()] = "a\xe2\x80\xae";
    CHECK_EQ(CodeOfPrint(b, o), std::string("C_PRINT_STRING"));
}

TEST(BslCanonical, BoundTokensPrintAsArrayElements) {
    const Body b = ParseOk("Р = \"[STR-1]\" + \"[STR-2]\";");
    PrintOptions o;
    const Expr& sum = *b.stmts[0]->exprs[1];
    o.bound_tokens[sum.args[0].get()] = 0;
    o.bound_tokens[sum.args[1].get()] = 7;
    CHECK_EQ(PrintBslCanonical(b, o), "Р = МСПЗначенияТокенов[0] + МСПЗначенияТокенов[7];\n");
}

TEST(BslCanonical, BadReplacementsAreRefused) {
    const Body b = ParseOk("Р = \"a\";");
    const Expr* lit = b.stmts[0]->exprs[1].get();
    const Body other = ParseOk("Р = \"a\";");
    PrintOptions stray;
    stray.bound_tokens[other.stmts[0]->exprs[1].get()] = 0;
    CHECK_EQ(CodeOfPrint(b, stray), std::string("C_PRINT_REPLACEMENT"));
    PrintOptions both;
    both.bound_tokens[lit] = 0;
    both.query_replacements[lit] = "x";
    CHECK_EQ(CodeOfPrint(b, both), std::string("C_PRINT_REPLACEMENT"));
    PrintOptions neg;
    neg.bound_tokens[lit] = -1;
    CHECK_EQ(CodeOfPrint(b, neg), std::string("C_PRINT_REPLACEMENT"));
    CHECK(PrintBslCanonical(b, neg).empty());
}

TEST(BslCanonical, ReplacementDoesNotMutateInput) {
    const Body b = ParseOk("Р = \"a\";");
    const std::string before = DumpBsl(b);
    PrintOptions o;
    o.bound_tokens[b.stmts[0]->exprs[1].get()] = 3;
    CHECK(PrintBslCanonicalChecked(b, o).ok);
    CHECK_EQ(DumpBsl(b), before);
}

TEST(BslCanonical, LongQueryReplacementRoundTrips) {
    const Body b = ParseOk("Т = \"x\";");
    std::string q;
    for (int i = 0; i < 2000; ++i) {
        if (i != 0) q += '\n';
        q += "    Поле" + std::to_string(i) + " КАК П" + std::to_string(i);
    }
    PrintOptions o;
    o.query_replacements[b.stmts[0]->exprs[1].get()] = q;
    const PrintResult p = PrintBslCanonicalChecked(b, o);
    if (!p.ok) qla_test::Fail("refused: " + p.error.code, __FILE__, __LINE__);
    CHECK(p.text.find('|') == std::string::npos);
    CHECK(ParseBsl(p.text, true).ok);
}

TEST(BslCanonical, ReservedNamesAreRejectedByParser) {
    const char* bad[] = {
        "МСПЗначенияТокенов = 1;", "мспзначенияТОКЕНОВ[0] = 1;", "МСПЗначенияТокенов[0] = 1;", "Символы = 1;",
        "символы.ПС = 1;", "Chars = 1;", "CHARS[1] = 2;", "Р = Символы;", "Для Символы = 1 По 2 Цикл КонецЦикла;",
        "Для Каждого chars Из Х Цикл КонецЦикла;", "Р = Новый Символы;", "Р = Chars(1);",
        "МСПЗнач\xd1\x91нияТокенов = 1;",   // yo-folded spelling
        "\xd0\xbc\xd1\x81\xd0\xbf\xd0\x97\xd0\xbd\xd0\xb0\xd1\x87\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f\xd0\xa2\xd0\xbe\xd0\xba\xd0\xb5\xd0\xbd\xd0\xbe\xd0\xb2 = 1;",
    };
    for (const char* src : bad) {
        const ParseResult r = ParseBsl(src);
        CHECK(!r.ok);
        CHECK_EQ(r.error.code, std::string("C_RESERVED"));
    }
    CHECK(ParseBsl("Р = Х.Символы + Х.МСПЗначенияТокенов;").ok);   // member names are not reserved
    CHECK(ParseBsl("Символы = 1;", true).ok);                       // printer re-parse only
}

TEST(BslCanonical, ReservedNamesInHandBuiltTreesAreRefused) {
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Var, "Символы"))), std::string("C_PRINT_RESERVED"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::Call, "МСПЗначенияТокенов"))), std::string("C_PRINT_RESERVED"));
    CHECK_EQ(CodeOfPrint(AssignBody(Mk(Expr::K::New, "chars"))), std::string("C_PRINT_RESERVED"));
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::K::Assign;
    s->exprs = {Mk(Expr::K::Index, "", {Mk(Expr::K::Var, "МСПЗначенияТокенов"), StrLit("a")}), StrLit("b")};
    Body b;
    b.stmts.push_back(s);
    CHECK_EQ(CodeOfPrint(b), std::string("C_PRINT_RESERVED"));
    auto f = std::make_shared<Stmt>();
    f->kind = Stmt::K::ForEach;
    f->var = "Символы";
    f->exprs = {Mk(Expr::K::Var, "Х")};
    f->blocks.emplace_back();
    Body fb;
    fb.stmts.push_back(f);
    CHECK_EQ(CodeOfPrint(fb), std::string("C_PRINT_RESERVED"));
}

TEST(BslCanonical, ReplacementInAssignmentTargetIsRefused) {
    const Body b = ParseOk("Х[0].Св = 1;");
    const Expr* member = b.stmts[0]->exprs[0].get();
    const Expr* index = member->args[0].get();
    const Expr* var = index->args[0].get();
    for (const Expr* target : {member, index, var}) {
        PrintOptions o;
        o.bound_tokens[target] = 0;
        CHECK_EQ(CodeOfPrint(b, o), std::string("C_PRINT_REPLACEMENT"));
        PrintOptions q;
        q.query_replacements[target] = "x";
        CHECK_EQ(CodeOfPrint(b, q), std::string("C_PRINT_REPLACEMENT"));
    }
    PrintOptions value;   // the right-hand side and the index expression are fine
    value.bound_tokens[b.stmts[0]->exprs[1].get()] = 0;
    value.bound_tokens[index->args[1].get()] = 1;
    CHECK_EQ(CodeOfPrint(b, value), std::string("OK"));
}

TEST(BslCanonical, OutputOverOneMiBIsRefusedWithLimitCode) {
    const Body b = AssignBody(StrLit(std::string(1024 * 1024 + 10, 'a')));
    CHECK_EQ(CodeOfPrint(b), std::string("C_PRINT_LIMIT"));
    CHECK(PrintBslCanonical(b).empty());
}

TEST(BslCanonical, InvisibleFillerCharactersInStrings) {
    const char* bad[] = {"a\xcd\x8f" "z", "a\xef\xb8\x8f" "z", "a\xe1\x85\x9f" "z", "a\xe3\x85\xa4" "z",
                         "a\xef\xbe\xa0" "z", "a\xe2\xa0\x80" "z", "a\xe1\x9a\x80" "z"};
    for (const char* v : bad) CHECK_EQ(CodeOfPrint(AssignBody(StrLit(v))), std::string("C_PRINT_STRING"));
}

TEST(BslCanonical, NegationAndNotOperands) {
    CheckRoundTrip("Р = А - (-1);");
    CheckRoundTrip("Р = А - -1;");
    CheckRoundTrip("Р = НЕ А И Б;");
    CheckRoundTrip("Р = А И НЕ Б;");
    CheckRoundTrip("Р = А = (НЕ Б);");
    CheckRoundTrip("Р = (НЕ А) + 1;");
    CHECK_EQ(PrintedOrEmpty("Р = А - (-1);"), "Р = А - (-1);\n");
    CHECK_EQ(PrintedOrEmpty("Р = НЕ А И Б;"), "Р = (НЕ А) И Б;\n");
}
