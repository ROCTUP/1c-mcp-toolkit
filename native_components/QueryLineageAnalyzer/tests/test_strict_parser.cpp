#include "test_framework.h"
#include "strict/strict_parser.h"
#include "strict/ast_dump.h"

#include <string>

using namespace lineage::strict;

namespace {

std::string ExprDump(const std::string& text) {
    auto r = ParseExpressionForTest(text);
    return r.ok ? DumpExpr(*r.expr) : "ERR:" + r.error.code;
}

std::string Code(const std::string& text) {
    auto r = ParseExpressionForTest(text);
    return r.ok ? "OK" : r.error.code;
}

ExprP Make(Expr::K kind) {
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    return e;
}

}  // namespace

TEST(StrictParser, PrecedenceOrAndNotComparison) {
    CHECK_EQ(ExprDump("А = 1 ИЛИ НЕ Б > 2 И Г"),
             std::string("(ИЛИ (= (f А) 1) (И (НЕ (> (f Б) 2)) (f Г)))"));
}
TEST(StrictParser, BareВIsTheInKeywordNotAField) {
    // The plan text used "В" as a field name; it is the В (IN) keyword and must never parse as a field.
    CHECK(Code("А И В") != "OK");
}
TEST(StrictParser, ArithmeticPrecedence) {
    CHECK_EQ(ExprDump("1 + 2 * -Х"), std::string("(+ 1 (* 2 (- (f Х))))"));
    CHECK_EQ(ExprDump("А - -Б"), std::string("(- (f А) (- (f Б)))"));
    CHECK_EQ(ExprDump("(1 + 2) * 3"), std::string("(* (+ 1 2) 3)"));
    CHECK_EQ(ExprDump("1 - 2 - 3"), std::string("(- (- 1 2) 3)"));
}
TEST(StrictParser, Predicates) {
    CHECK_EQ(ExprDump("Х ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"~\""), std::string("(like (f Х) \"а%\" \"~\")"));
    CHECK_EQ(ExprDump("Х НЕ МЕЖДУ 1 И 2"), std::string("(!between (f Х) 1 2)"));
    CHECK_EQ(ExprDump("Х ЕСТЬ НЕ NULL"), std::string("(!isnull (f Х))"));
    CHECK_EQ(ExprDump("Х ЕСТЬ NULL"), std::string("(isnull (f Х))"));
    CHECK_EQ(ExprDump("Х В (1, 2)"), std::string("(in (f Х) 1 2)"));
    CHECK_EQ(ExprDump("Х НЕ В (&П)"), std::string("(!in (f Х) (param &П))"));
    CHECK_EQ(ExprDump("Х ССЫЛКА Справочник.Контрагенты"), std::string("(refs (f Х) Справочник.Контрагенты)"));
    CHECK_EQ(ExprDump("Х НЕ ПОДОБНО \"а\""), std::string("(!like (f Х) \"а\")"));
}
TEST(StrictParser, BetweenBoundsAreAdditiveAndConsumeOneAnd) {
    CHECK_EQ(ExprDump("Х МЕЖДУ 1 + 1 И 3 И Y"), std::string("(И (between (f Х) (+ 1 1) 3) (f Y))"));
}
TEST(StrictParser, InHierarchy) {
    CHECK_EQ(ExprDump("Х В ИЕРАРХИИ (&Р)"), std::string("(in-h (f Х) (param &Р))"));
    CHECK_EQ(ExprDump("Х НЕ в иерархии (&Р, &Q)"), std::string("(!in-h (f Х) (param &Р) (param &Q))"));
    CHECK_EQ(ExprDump("Х IN HIERARCHY (1)"), std::string("(in-h (f Х) 1)"));
    CHECK(Code("Х В ИЕРАРХИЯ (1)") != "OK");     // wrong word
    CHECK(Code("Х В ИЕРАРXИИ (1)") != "OK");     // Latin X inside: not the word
}
TEST(StrictParser, FunctionsCaseCast) {
    CHECK_EQ(ExprDump("ПОДСТРОКА(Т.Имя, 1, 3)"), std::string("(call ПОДСТРОКА (f Т Имя) 1 3)"));
    CHECK_EQ(ExprDump("КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Т.Х)"), std::string("(call! КОЛИЧЕСТВО (f Т Х))"));
    CHECK_EQ(ExprDump("КОЛИЧЕСТВО(*)"), std::string("(call КОЛИЧЕСТВО *)"));
    CHECK_EQ(ExprDump("ВЫБОР КОГДА А ТОГДА 1 ИНАЧЕ 2 КОНЕЦ"), std::string("(case (f А) 1 2)"));
    CHECK_EQ(ExprDump("ВЫБОР Х КОГДА 1 ТОГДА 2 КОГДА 3 ТОГДА 4 КОНЕЦ"), std::string("(case-op (f Х) 1 2 3 4)"));
    CHECK_EQ(ExprDump("ВЫРАЗИТЬ(Х КАК Строка(10))"), std::string("(cast (f Х) Строка 10)"));
    CHECK_EQ(ExprDump("ВЫРАЗИТЬ(Х КАК Число(15, 2))"), std::string("(cast (f Х) Число 15 2)"));
    CHECK_EQ(ExprDump("ЗНАЧЕНИЕ(Перечисление.Виды.Основной)"), std::string("(value Перечисление.Виды.Основной)"));
    CHECK_EQ(ExprDump("ТИП(Число)"), std::string("(type Число)"));
    CHECK_EQ(ExprDump("ЕСТЬNULL(А, 0)"), std::string("(call ЕСТЬNULL (f А) 0)"));
    CHECK_EQ(ExprDump("isnull(А, 0)"), std::string("(call ЕСТЬNULL (f А) 0)"));
    CHECK_EQ(ExprDump("ТИПЗНАЧЕНИЯ(А)"), std::string("(call ТИПЗНАЧЕНИЯ (f А))"));
    CHECK_EQ(ExprDump("ПРЕДСТАВЛЕНИЕ(А)"), std::string("(call ПРЕДСТАВЛЕНИЕ (f А))"));
    CHECK_EQ(ExprDump("ДАТАВРЕМЯ(2020, 1, 31)"), std::string("(call ДАТАВРЕМЯ 2020 1 31)"));
}
TEST(StrictParser, LiteralsParametersAndPaths) {
    CHECK_EQ(ExprDump("\"a\" + \"b\""), std::string("(+ \"a\" \"b\")"));
    CHECK_EQ(ExprDump("\"a\"\"b\""), std::string("\"a\\\"b\""));
    CHECK_EQ(ExprDump("ИСТИНА И ЛОЖЬ"), std::string("(И ИСТИНА ЛОЖЬ)"));
    CHECK_EQ(ExprDump("НЕОПРЕДЕЛЕНО = NULL"), std::string("(= НЕОПРЕДЕЛЕНО NULL)"));
    CHECK_EQ(ExprDump("1.5"), std::string("1.5"));
    CHECK_EQ(ExprDump("&Имя"), std::string("(param &Имя)"));
    CHECK_EQ(ExprDump("Т.Ссылка = Т.Представление"), std::string("(= (f Т Ссылка) (f Т Представление))"));
    CHECK_EQ(ExprDump("X and not Y or Z"), std::string("(ИЛИ (И (f X) (НЕ (f Y))) (f Z))"));
    CHECK_EQ(ExprDump("Расчёты.Ёмкость"), std::string("(f Расчёты Ёмкость)"));
}
TEST(StrictParser, RowOnlyBeforeSubqueryIn) {
    CHECK_EQ(Code("(1, 2) В (1, 2)"), std::string("Q_PARSE_ROW_POSITION"));
    CHECK_EQ(Code("(1, 2) = 3"), std::string("Q_PARSE_INVALID_NODE"));
    CHECK_EQ(Code("Х = (1, 2)"), std::string("Q_PARSE_INVALID_NODE"));
    CHECK_EQ(Code("(ВЫБРАТЬ 1)"), std::string("Q_PARSE_SUBQUERY_POSITION"));
    CHECK_EQ(Code("Х = (ВЫБРАТЬ 1)"), std::string("Q_PARSE_SUBQUERY_POSITION"));
}
TEST(StrictParser, FailClosed) {
    for (const char* bad : {"Х +", "(Х", "Х КАК", "ПОДСТРОКА(Х,", "ВЫБОР КОГДА А ТОГДА 1", "'x'", "{Х}", "Х ПОДОБНО"})
        CHECK(ExprDump(bad).rfind("ERR:", 0) == 0);
}
TEST(StrictParser, FailClosedBattery) {
    for (const char* bad : {
             "", "1 2", "1 )", "Х;", "+Х", "Х = ", "НЕ", "Х НЕ = 1", "Х НЕ ЕСТЬ NULL", "Х ЕСТЬ", "Х ЕСТЬ НЕ",
             "Х В", "Х В ()", "Х В (1,", "Х В (1 2)", "Х МЕЖДУ 1", "Х МЕЖДУ 1 И", "Х ССЫЛКА", "Х ССЫЛКА Справочник",
             "ЗНАЧЕНИЕ(Х)", "ЗНАЧЕНИЕ()", "ТИП()", "ВЫРАЗИТЬ(Х КАК)", "ВЫРАЗИТЬ(Х КАК Строка(1.5))",
             "ВЫРАЗИТЬ(Х КАК Строка(1,2,3))", "ВЫРАЗИТЬ(Х Строка)", "ВЫБОР КОНЕЦ", "ВЫБОР Х КОНЕЦ",
             "ВЫБОР КОГДА А ТОГДА 1 ИНАЧЕ КОНЕЦ", "ВЫБРАТЬ", "Значение", "ССЫЛКА.Х", "Т.", "Т..Х", "Т.1", "Т.Х(1)",
             "КОЛИЧЕСТВО()", "КОЛИЧЕСТВО(РАЗЛИЧНЫЕ *)", "СУММА(*)", "СУММА(РАЗЛИЧНЫЕ Х)",
             "КОЛИЧЕСТВО(РАЗЛИЧНЫЕ А, Б)", "ЕСТЬNULL(А)", "ЕСТЬNULL(А, Б, В)", "ТИПЗНАЧЕНИЯ(А, Б)", "ПРЕДСТАВЛЕНИЕ",
             "&", "& Х", "&Х.Y", "1abc", "1..2", "Х <> = 1", "Х ! 1", "Х == 1", "Х /* c */", "Х -- ", "$Х",
             "ПЁРВЫЕ", "(,)", "(1,)", "ИЛИ Х", "Х И", "Х ТОГДА"})
        CHECK(Code(bad) != "OK");
}
TEST(StrictParser, ChainedPredicatesAreErrors) {
    CHECK_EQ(Code("А = Б = В"), std::string("Q_PARSE_CHAIN"));
    CHECK_EQ(Code("А < Б > В"), std::string("Q_PARSE_CHAIN"));
    CHECK_EQ(Code("А = Б В (1)"), std::string("Q_PARSE_CHAIN"));
    CHECK_EQ(Code("А В (1) = 2"), std::string("Q_PARSE_CHAIN"));
    CHECK_EQ(Code("А В (1) В (2)"), std::string("Q_PARSE_CHAIN"));
    CHECK_EQ(Code("А ЕСТЬ NULL ЕСТЬ NULL"), std::string("Q_PARSE_CHAIN"));
    CHECK_EQ(Code("А МЕЖДУ 1 И 2 НЕ ПОДОБНО \"x\""), std::string("Q_PARSE_CHAIN"));
}
TEST(StrictParser, MixedScriptIdentifiersRejected) {
    CHECK_EQ(Code("Aб"), std::string("Q_PARSE_MIXED_SCRIPT"));      // Latin A + Cyrillic б
    CHECK_EQ(Code("Т.Aб"), std::string("Q_PARSE_MIXED_SCRIPT"));
    CHECK_EQ(Code("&Aб"), std::string("Q_PARSE_MIXED_SCRIPT"));
    CHECK_EQ(Code("Aб(1)"), std::string("Q_PARSE_MIXED_SCRIPT"));
    CHECK_EQ(Code("ВЫРАЗИТЬ(Х КАК Aб.Ц)"), std::string("Q_PARSE_MIXED_SCRIPT"));
    CHECK_EQ(Code("ЗНАЧЕНИЕ(Перечисление.Bид.Основной)"), std::string("Q_PARSE_MIXED_SCRIPT"));
    CHECK_EQ(Code("Х ССЫЛКА Справочник.Aб"), std::string("Q_PARSE_MIXED_SCRIPT"));
    CHECK_EQ(Code("Abc_1"), std::string("OK"));
    CHECK_EQ(Code("Абв_1"), std::string("OK"));
}
TEST(StrictParser, ErrorsCarryPosition) {
    auto r = ParseExpressionForTest("Х +");
    CHECK(!r.ok);
    CHECK_EQ(r.error.pos.line, 1u);
    CHECK_EQ(r.error.pos.col, 4u);
    r = ParseExpressionForTest("А =\n  'x'");
    CHECK_EQ(r.error.code, std::string("Q_PARSE_TOKEN"));
    CHECK_EQ(r.error.pos.line, 2u);
    CHECK_EQ(r.error.pos.col, 3u);
}
TEST(StrictParser, DepthLimitIsError) {
    std::string deep(200, '(');
    deep += "1" + std::string(200, ')');
    CHECK_EQ(ExprDump(deep), std::string("ERR:Q_PARSE_LIMIT"));
    std::string nots;
    for (int i = 0; i < 100; ++i) nots += "НЕ ";
    CHECK_EQ(ExprDump(nots + "Х"), std::string("ERR:Q_PARSE_LIMIT"));
    std::string minus(100, '-');
    CHECK_EQ(ExprDump(minus + "1"), std::string("ERR:Q_PARSE_LIMIT"));
    std::string ok(30, '(');
    ok += "1" + std::string(30, ')');
    CHECK_EQ(ExprDump(ok), std::string("1"));
}
TEST(StrictParser, LeftDeepChainsAreBoundedByTreeDepth) {
    auto chain = [](int terms, const char* op) {
        std::string s = "1";
        for (int i = 1; i < terms; ++i) s += std::string(" ") + op + " 1";
        return s;
    };
    CHECK_EQ(Code(chain(100, "+")), std::string("OK"));
    CHECK_EQ(Code(chain(100, "ИЛИ")), std::string("OK"));
    CHECK_EQ(Code(chain(300, "+")), std::string("Q_PARSE_LIMIT"));
    CHECK_EQ(Code(chain(150000, "+")), std::string("Q_PARSE_LIMIT"));
}
TEST(StrictParser, UnsetNodeIsRejectedByValidator) {
    CHECK(Expr().kind == Expr::K::Unset);
    ParseError err;
    CHECK(!ValidateExpr(std::make_shared<Expr>(), err));
    CHECK_EQ(err.code, std::string("Q_PARSE_INVALID_NODE"));
    CHECK(!ValidateExpr(nullptr, err));

    // An Unset node buried in an otherwise valid tree.
    auto bin = Make(Expr::K::Binary);
    bin->text = "+";
    auto one = Make(Expr::K::NumberLit);
    one->text = "1";
    bin->args = {one, std::make_shared<Expr>()};
    CHECK(!ValidateExpr(bin, err));
    bin->args[1] = one;
    CHECK(ValidateExpr(bin, err));

    // Null child, wrong arity, Row misuse, bad payload.
    bin->args[1] = nullptr;
    CHECK(!ValidateExpr(bin, err));
    auto un = Make(Expr::K::Unary);
    un->text = "-";
    CHECK(!ValidateExpr(un, err));   // no operand
    auto row = Make(Expr::K::Row);
    row->args = {one, one};
    CHECK(!ValidateExpr(row, err));  // Row outside subquery В
    auto lit = Make(Expr::K::BoolLit);
    lit->text = "MAYBE";
    CHECK(!ValidateExpr(lit, err));
}
TEST(StrictParser, KeywordPathSegmentsRejected) {
    CHECK_EQ(ExprDump("Т.Ссылка.Наименование"), std::string("(f Т Ссылка Наименование)"));
    CHECK_EQ(ExprDump("Т.ПредставлениеСсылки"), std::string("(f Т ПредставлениеСсылки)"));
    CHECK_EQ(ExprDump("Х.Значение"), std::string("(f Х Значение)"));
    CHECK(Code("Ссылка.Х") != "OK");   // allowlisted words are never the first segment
    for (const char* bad : {"Т.И", "Т.НЕ", "Т.ВЫБРАТЬ", "Т.ТИП", "ТИП(Х.И)", "Т.А.В", "ВЫРАЗИТЬ(Х КАК Т.Тип)", "Х ССЫЛКА Справочник.Все",
                            "ЗНАЧЕНИЕ(Перечисление.Виды.Для)"})
        CHECK_EQ(Code(bad), std::string("Q_PARSE_KEYWORD_SEGMENT"));
}
TEST(StrictParser, KeywordColumnAliasOnlyAfterKak) {
    auto r = ParseBatchStrict("ВЫБРАТЬ Т.Ссылка КАК Ссылка, Т.А КАК представление, Т.Б AS ПредставлениеСсылки, "
                              "Т.Г КАК Значение ИЗ Справочник.А КАК Т");
    CHECK(r.ok);
    const auto& items = r.batch.statements[0].select->items;
    CHECK_EQ(items.size(), 4u);
    CHECK(items[0].alias_explicit && items[0].alias == "Ссылка");
    CHECK(items[1].alias == "представление");
    CHECK(items[2].alias == "ПредставлениеСсылки");
    CHECK(items[3].alias == "Значение");
    // Only the four path keywords, only as the alias of a select item, only after КАК.
    for (const char* bad : {"ВЫБРАТЬ Т.А КАК Тип ИЗ Справочник.А КАК Т", "ВЫБРАТЬ Т.А КАК ВЫБРАТЬ ИЗ Справочник.А КАК Т",
                            "ВЫБРАТЬ Т.А Ссылка ИЗ Справочник.А КАК Т", "ВЫБРАТЬ Т.А КАК А ИЗ Справочник.А КАК Ссылка",
                            "ВЫБРАТЬ Т.А КАК А ПОМЕСТИТЬ Значение ИЗ Справочник.А КАК Т",
                            "ВЫБРАТЬ Т.А КАК А ИЗ Справочник.А КАК Т ИТОГИ ПО Т.А КАК Ссылка",
                            "ВЫБРАТЬ Т.А КАК А ИЗ Справочник.А КАК Т УПОРЯДОЧИТЬ ПО Ссылка",
                            "ВЫБРАТЬ Т.А КАК Ссылка.Х ИЗ Справочник.А КАК Т"}) {
        CHECK(!ParseBatchStrict(bad).ok);
    }
}
TEST(StrictParser, NotLikeWithEscape) {
    CHECK_EQ(ExprDump("Х НЕ ПОДОБНО \"а\" СПЕЦСИМВОЛ \"~\""), std::string("(!like (f Х) \"а\" \"~\")"));
}
TEST(StrictParser, LimitThroughCaseAndCallNesting) {
    std::string cs, ce, call, callend;
    for (int i = 0; i < 100; ++i) {
        cs += "ВЫБОР КОГДА ";
        ce += " ТОГДА 1 КОНЕЦ";
        call += "Ф(";
        callend += ")";
    }
    CHECK_EQ(Code(cs + "1" + ce), std::string("Q_PARSE_LIMIT"));
    CHECK_EQ(Code(call + "1" + callend), std::string("Q_PARSE_LIMIT"));
}
TEST(StrictParser, ValidatorRejectsMalformedHandBuiltNodes) {
    ParseError err;
    auto num = [] {
        auto e = Make(Expr::K::NumberLit);
        e->text = "1";
        return e;
    };
    auto with = [&](Expr::K k, std::vector<ExprP> args) {
        auto e = Make(k);
        e->args = std::move(args);
        return e;
    };
    // Control: a well-formed case passes.
    auto good = with(Expr::K::Case, {num(), num()});
    CHECK(ValidateExpr(good, err));
    // Case shape.
    CHECK(!ValidateExpr(with(Expr::K::Case, {num()}), err));
    CHECK(!ValidateExpr(with(Expr::K::Case, {num(), num(), num()}), err));
    auto op = with(Expr::K::Case, {num(), num()});
    op->has_operand = true;
    CHECK(!ValidateExpr(op, err));
    // Cast.
    auto cast = with(Expr::K::Cast, {num()});
    CHECK(!ValidateExpr(cast, err));                 // no type
    cast->type_name = "Строка";
    cast->type_args = {"10"};
    CHECK(ValidateExpr(cast, err));
    cast->type_args = {"1", "2", "3"};
    CHECK(!ValidateExpr(cast, err));
    cast->type_args = {"1 OR 1"};
    CHECK(!ValidateExpr(cast, err));
    cast->type_args.clear();
    cast->type_name = "Строка\") ; DROP";
    CHECK(!ValidateExpr(cast, err));
    // InList / Between / Like arity.
    CHECK(!ValidateExpr(with(Expr::K::InList, {num()}), err));
    CHECK(!ValidateExpr(with(Expr::K::Between, {num(), num()}), err));
    CHECK(!ValidateExpr(with(Expr::K::Like, {num()}), err));
    CHECK(ValidateExpr(with(Expr::K::Like, {num(), num()}), err));
    // Call + distinct.
    auto call = with(Expr::K::Call, {num()});
    call->text = "СУММА";
    CHECK(ValidateExpr(call, err));
    call->distinct = true;
    CHECK(!ValidateExpr(call, err));
    call->text = "КОЛИЧЕСТВО";
    CHECK(ValidateExpr(call, err));
    call->text = "Ф(); --";
    call->distinct = false;
    CHECK(!ValidateExpr(call, err));
    // Subquery misuse.
    auto sub = with(Expr::K::InList, {num(), num()});
    sub->subquery = std::make_shared<Select>();
    CHECK(!ValidateExpr(sub, err));
    CHECK(!ValidateExpr(with(Expr::K::InSubquery, {num()}), err));   // no subquery
    // Flags on the wrong kind.
    auto n1 = num();
    n1->negated = true;
    CHECK(!ValidateExpr(n1, err));
    n1 = num();
    n1->has_else = true;
    CHECK(!ValidateExpr(n1, err));
    n1 = num();
    n1->distinct = true;
    CHECK(!ValidateExpr(n1, err));
    n1 = num();
    n1->type_name = "X";
    CHECK(!ValidateExpr(n1, err));
    n1 = num();
    n1->parts = {"X"};
    CHECK(!ValidateExpr(n1, err));
    // Injected payload text.
    for (const char* t : {"1e5", "-1", "1.", ".5", "1.2.3", "1 2", "", "0x10"}) {
        n1 = num();
        n1->text = t;
        CHECK(!ValidateExpr(n1, err));
    }
    auto fld = Make(Expr::K::Field);
    fld->parts = {"Т", "Х; DROP"};
    CHECK(!ValidateExpr(fld, err));
    fld->parts = {"Т", "Aб"};   // mixed script
    CHECK(!ValidateExpr(fld, err));
    fld->parts = {"Т", "1X"};
    CHECK(!ValidateExpr(fld, err));
    fld->parts = {"Т", "Имя_1"};
    CHECK(ValidateExpr(fld, err));
    auto prm = Make(Expr::K::Param);
    prm->text = "Х) OR (1=1";
    CHECK(!ValidateExpr(prm, err));
    auto txt = num();
    txt = Make(Expr::K::IsNullTest);
    txt->args = {num()};
    txt->text = "junk";
    CHECK(!ValidateExpr(txt, err));
}
TEST(StrictParser, ValidatorNodeBudgetStopsSharedChildDag) {
    auto cur = Make(Expr::K::NumberLit);
    cur->text = "1";
    for (int i = 0; i < 60; ++i) {
        auto b = Make(Expr::K::Binary);
        b->text = "+";
        b->args = {cur, cur};   // 2^60 paths, depth 61
        cur = b;
    }
    ParseError err;
    CHECK(!ValidateExpr(cur, err));
    CHECK_EQ(err.code, std::string("Q_PARSE_LIMIT"));
}
TEST(StrictParser, ValidatorBoundsDepth) {
    auto cur = Make(Expr::K::NumberLit);
    cur->text = "1";
    for (int i = 0; i < kMaxExprDepth + 10; ++i) {
        auto u = Make(Expr::K::Unary);
        u->text = "-";
        u->args = {cur};
        cur = u;
    }
    ParseError err;
    CHECK(!ValidateExpr(cur, err));
    CHECK_EQ(err.code, std::string("Q_PARSE_LIMIT"));
    // Unwind iteratively so the test itself does not rely on recursive destruction depth.
    while (cur && !cur->args.empty()) {
        ExprP next = cur->args[0];
        cur->args.clear();
        cur = next;
    }
}
// ---- statements, clauses, batches ----

namespace {

std::string BatchDump(const std::string& text) {
    auto r = ParseBatchStrict(text);
    return r.ok ? DumpBatch(r.batch) : "ERR:" + r.error.code;
}

bool Bad(const std::string& text) { return BatchDump(text).rfind("ERR:", 0) == 0; }

std::string BatchCode(const std::string& text) {
    auto r = ParseBatchStrict(text);
    return r.ok ? "OK" : r.error.code;
}

}  // namespace

TEST(StrictParser, SelectAllClauses) {
    const char* q =
        "ВЫБРАТЬ РАЗРЕШЕННЫЕ РАЗЛИЧНЫЕ ПЕРВЫЕ 10 Т.Контрагент КАК К, СУММА(Т.Сумма) КАК С "
        "ИЗ Документ.Реализация КАК Т "
        "ЛЕВОЕ СОЕДИНЕНИЕ Справочник.Контрагенты КАК Контрагенты ПО Т.Контрагент = Контрагенты.Ссылка "
        "ГДЕ Т.Проведен СГРУППИРОВАТЬ ПО Т.Контрагент ИМЕЮЩИЕ СУММА(Т.Сумма) > 0 "
        "УПОРЯДОЧИТЬ ПО С УБЫВ ИТОГИ СУММА(С) ПО ОБЩИЕ, К ИЕРАРХИЯ";
    const std::string d = BatchDump(q);
    CHECK(d.rfind("ERR:", 0) != 0);
    CHECK(d.find("allowed") != std::string::npos);
    CHECK(d.find("distinct") != std::string::npos);
    CHECK(d.find("top 10") != std::string::npos);
    CHECK(d.find("(join left") != std::string::npos);
    CHECK(d.find("(totals") != std::string::npos);
}
TEST(StrictParser, VirtualTableWithParams) {
    CHECK(BatchDump("ВЫБРАТЬ О.СуммаОстаток ИЗ РегистрНакопления.Взаиморасчеты.Остатки(&Дата, Контрагент = &К) КАК О")
              .find("(vt РегистрНакопления.Взаиморасчеты.Остатки") != std::string::npos);
    CHECK(BatchDump("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Цены.СрезПоследних(, ) КАК О").rfind("ERR:", 0) != 0);
    auto r = ParseBatchStrict("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Цены.СрезПоследних(, ) КАК О");
    CHECK(r.ok);
    CHECK_EQ(r.batch.statements[0].select->from[0].vt_params.size(), 2u);
    CHECK(r.batch.statements[0].select->from[0].vt_params[0] == nullptr);
}
TEST(StrictParser, TempTablesBatchesDrop) {
    CHECK_EQ(ParseBatchStrict("ВЫБРАТЬ 1 КАК Х ПОМЕСТИТЬ ВТ; ВЫБРАТЬ ВТ.Х ИЗ ВТ КАК ВТ; УНИЧТОЖИТЬ ВТ").batch.statements.size(), 3u);
    CHECK_EQ(ParseBatchStrict("ВЫБРАТЬ 1;").batch.statements.size(), 1u);
}
TEST(StrictParser, UnionOrderTotalsAttachToWholeStatement) {
    auto r = ParseBatchStrict("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ Б.Х ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО Х");
    CHECK(r.ok);
    CHECK_EQ(r.batch.statements[0].select->unions.size(), 1u);
    CHECK_EQ(r.batch.statements[0].select->order_by.size(), 1u);
    CHECK(r.batch.statements[0].select->unions[0].all);
}
TEST(StrictParser, SubqueryInFromAndIn) {
    CHECK(ParseBatchStrict("ВЫБРАТЬ П.Х ИЗ (ВЫБРАТЬ Т.Х ИЗ Справочник.Т КАК Т) КАК П ГДЕ П.Х В (ВЫБРАТЬ Р.Х ИЗ Справочник.Р КАК Р)").ok);
}
TEST(StrictParser, KeywordВAsSourceAliasAfterKak) {
    auto r = ParseBatchStrict("ВЫБРАТЬ В.Выполнено КАК Выполнено, КОЛИЧЕСТВО(*) КАК Строк, МИНИМУМ(В.Порядок) КАК Мин "
                              "ИЗ РегистрСведений.Р КАК В ГДЕ В.Порядок > 1 И НЕ В.Пометка И В.Ссылка В (&А, &Б) "
                              "СГРУППИРОВАТЬ ПО В.Выполнено");
    CHECK(r.ok);
    const Select& s = *r.batch.statements[0].select;
    CHECK_EQ(s.from[0].alias, std::string("В"));
    CHECK_EQ(DumpExpr(*s.items[0].expr), std::string("(f В Выполнено)"));
    CHECK_EQ(DumpExpr(*s.where), std::string("(И (И (> (f В Порядок) 1) (НЕ (f В Пометка))) (in (f В Ссылка) (param &А) "
                                             "(param &Б)))"));
    for (const char* ok : {"ВЫБРАТЬ в.Х КАК Х ИЗ (ВЫБРАТЬ 1 КАК Х) КАК в",
                           "ВЫБРАТЬ А.Х КАК Х ИЗ Справочник.А КАК А ЛЕВОЕ СОЕДИНЕНИЕ Справочник.Б КАК В ПО В.Ссылка = А.Х",
                           "SELECT in.X AS X FROM Catalog.A AS in WHERE in.Y IN (1)"}) {
        CHECK_EQ(BatchCode(ok), std::string("OK"));
    }
    // A select item named В (defined only: it cannot be referred to).
    auto item = ParseBatchStrict("ВЫБРАТЬ Т.Х = 1 КАК В, Т.Х КАК Х ИЗ Справочник.А КАК Т");
    CHECK(item.ok);
    CHECK(item.batch.statements[0].select->items[0].alias_explicit && item.batch.statements[0].select->items[0].alias == "В");
    // Only after an explicit КАК, only В, only as the first segment of a path to a source.
    for (const char* bad : {"ВЫБРАТЬ В.Х КАК Х ИЗ Справочник.А В", "ВЫБРАТЬ Т.Х В ИЗ Справочник.А КАК Т",
                            "ВЫБРАТЬ В КАК Х ИЗ Справочник.А КАК В", "ВЫБРАТЬ Т.В КАК Х ИЗ Справочник.А КАК Т",
                            "ВЫБРАТЬ Т.Х КАК В ИЗ Справочник.А КАК Т УПОРЯДОЧИТЬ ПО В",
                            "ВЫБРАТЬ П.В КАК Х ИЗ (ВЫБРАТЬ 1 КАК В) КАК П",
                            "ВЫБРАТЬ И.Х КАК Х ИЗ Справочник.А КАК И", "ВЫБРАТЬ 1 КАК Х ПОМЕСТИТЬ В",
                            "ВЫБРАТЬ А.Х КАК Х ИЗ Справочник.А КАК А ГДЕ А.Х В.У", "ВЫБРАТЬ В.Х() КАК Х ИЗ Справочник.А КАК В"}) {
        CHECK(Bad(bad));
    }
}
TEST(StrictParser, StatementFailClosed) {
    for (const char* bad : {
             "ВЫБРАТЬ Х ИЗ",
             "ВЫБРАТЬ Х ИЗ Справочник.А КАК",
             "ВЫБРАТЬ Х ИЗ Справочник.А ГДЕ",
             "ВЫБРАТЬ Х ИЗ Справочник.А ЛЕВОЕ СОЕДИНЕНИЕ Справочник.Б КАК Б",
             "ВЫБРАТЬ Х ИЗ Справочник.А КАК А ГДЕ А.Х = 1 ЛИШНЕЕ",
             "ВЫБРАТЬ Х ИЗ Справочник.А {ГДЕ А.Х}",
             "ВЫБРАТЬ ПЕРВЫЕ &Н Х ИЗ Справочник.А",
             "ВЫБРАТЬ Х ИЗ Справочник.А ДЛЯ ИЗМЕНЕНИЯ Справочник.Б ЛИШНЕЕ"})
        CHECK(BatchDump(bad).rfind("ERR:", 0) == 0);
}
TEST(StrictParser, StatementLimit) {
    std::string many;
    for (int i = 0; i < 65; ++i) many += "ВЫБРАТЬ 1;";
    CHECK_EQ(BatchDump(many), std::string("ERR:Q_PARSE_LIMIT"));
    std::string ok;
    for (int i = 0; i < 64; ++i) ok += "ВЫБРАТЬ 1;";
    CHECK(!Bad(ok));
}
TEST(StrictParser, UnlistedConstructsAreExplicitNodesOrErrors) {
    auto r = ParseBatchStrict("ВЫБРАТЬ А.Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ДЛЯ ИЗМЕНЕНИЯ Справочник.А ИНДЕКСИРОВАТЬ ПО А.Х");
    CHECK(r.ok);
    CHECK(r.batch.statements[0].select->for_update);
    CHECK_EQ(r.batch.statements[0].select->for_update_tables.size(), 1u);
    CHECK_EQ(r.batch.statements[0].select->index_by.size(), 1u);
    auto p = ParseBatchStrict("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ИТОГИ ПО А.Х ПЕРИОДАМИ(ДЕНЬ, &Н, &К)");
    CHECK(p.ok);
    CHECK(p.batch.statements[0].select->totals_by[0].periods);
    CHECK_EQ(p.batch.statements[0].select->totals_by[0].periods_args.size(), 3u);
}
TEST(StrictParser, JoinForms) {
    for (const char* j : {"ПОЛНОЕ СОЕДИНЕНИЕ", "ПОЛНОЕ ВНЕШНЕЕ СОЕДИНЕНИЕ", "ЛЕВОЕ ВНЕШНЕЕ СОЕДИНЕНИЕ",
                          "ПРАВОЕ СОЕДИНЕНИЕ", "ВНУТРЕННЕЕ СОЕДИНЕНИЕ", "СОЕДИНЕНИЕ"}) {
        CHECK(!Bad(std::string("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ") + j + " Справочник.Б КАК Б ПО А.Х = Б.Х"));
    }
    for (const char* j : {"ВНЕШНЕЕ СОЕДИНЕНИЕ", "ВНУТРЕННЕЕ ВНЕШНЕЕ СОЕДИНЕНИЕ", "ЛЕВОЕ ЛЕВОЕ СОЕДИНЕНИЕ"}) {
        CHECK(Bad(std::string("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ") + j + " Справочник.Б КАК Б ПО А.Х = Б.Х"));
    }
    CHECK(!Bad("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А, Справочник.Б КАК Б ГДЕ А.Х = Б.Х"));
}
TEST(StrictParser, WildcardsAndAliases) {
    auto r = ParseBatchStrict("ВЫБРАТЬ *, А.*, А.Б.* , А.Х Алиас ИЗ Справочник.А КАК А");
    CHECK(r.ok);
    const auto& items = r.batch.statements[0].select->items;
    CHECK_EQ(items.size(), 4u);
    CHECK(items[0].wildcard && items[0].wildcard_prefix.empty());
    CHECK_EQ(items[2].wildcard_prefix.size(), 2u);
    CHECK(!items[3].alias_explicit && items[3].alias == "Алиас");
}
TEST(StrictParser, StatementStructureRejections) {
    CHECK(Bad(""));
    CHECK(Bad(";"));
    CHECK(Bad("ВЫБРАТЬ 1;;ВЫБРАТЬ 2"));
    CHECK(Bad("УНИЧТОЖИТЬ Справочник.А"));
    CHECK(Bad("УНИЧТОЖИТЬ"));
    CHECK(Bad("ВЫБРАТЬ 1 ПОМЕСТИТЬ А.Б"));
    CHECK(Bad("ВЫБРАТЬ 1 ОБЪЕДИНИТЬ ВЫБРАТЬ 2 ПОМЕСТИТЬ ВТ"));
    CHECK(Bad("ВЫБРАТЬ 1 ИЗ Справочник.А КАК А ГДЕ А.Х В (ВЫБРАТЬ 1 ПОМЕСТИТЬ ВТ)"));
    CHECK(Bad("ВЫБРАТЬ 1 УПОРЯДОЧИТЬ ПО А ИЗ Справочник.А"));
    CHECK(Bad("ВЫБРАТЬ 1 ПЕРВЫЕ 5"));
    CHECK(Bad("ВЫБРАТЬ 1 ИЗ Справочник.А КАК А ИТОГИ ПО А.Х ТОЛЬКО"));
    CHECK(Bad("ВЫБРАТЬ 1 ИЗ (ВЫБРАТЬ 2)"));
    CHECK(Bad("ВЫБРАТЬ 1 ИЗ Справочник.А(1,"));
    CHECK(Bad("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ГДЕ А.Х В (ВЫБРАТЬ 1) ГДЕ 1"));
    CHECK(Bad("ВЫБРАТЬ 1 ИЗ Справочник.А КАК Б.В"));
    CHECK(Bad("ВЫБРАТЬ 1 ИЗ Справочник.А КАК Latinаа"));
}
TEST(StrictParser, NestedSubqueryDepthIsLimited) {
    std::string q = "ВЫБРАТЬ 1 КАК Х";
    for (int i = 0; i < 100; ++i) q = "ВЫБРАТЬ Х ИЗ (" + q + ") КАК П";
    CHECK_EQ(BatchDump(q), std::string("ERR:Q_PARSE_LIMIT"));
}
TEST(StrictParser, ValidateBatchRejectsMalformedTrees) {
    ParseError err;
    Batch b;
    CHECK(!ValidateBatch(b, err));
    Statement st;
    st.kind = Statement::K::Select;
    b.statements.push_back(st);
    CHECK(!ValidateBatch(b, err));
    b.statements[0].select = std::make_shared<Select>();
    CHECK(!ValidateBatch(b, err));
    SelectItem it;
    it.expr = std::make_shared<Expr>();
    b.statements[0].select->items.push_back(it);
    CHECK(!ValidateBatch(b, err));
    b.statements[0].select->items[0].expr = Make(Expr::K::NullLit);
    b.statements[0].select->items[0].expr->text = "NULL";
    CHECK(ValidateBatch(b, err));
    b.statements[0].select->top = "x";
    CHECK(!ValidateBatch(b, err));
    b.statements[0].select->top.clear();
    b.statements[0].select->joins.emplace_back();
    CHECK(!ValidateBatch(b, err));
    b.statements[0].select->joins.clear();
    Statement drop;
    drop.kind = Statement::K::Drop;
    drop.drop_name = "А.Б";
    b.statements.push_back(drop);
    CHECK(!ValidateBatch(b, err));
}
TEST(StrictParser, ValidateWalksSubqueryBodies) {
    auto r = ParseExpressionForTest("Х В (ВЫБРАТЬ 1 КАК А)");
    CHECK(r.ok);
    ParseError err;
    CHECK(ValidateExpr(r.expr, err));
    r.expr->subquery->items[0].expr->kind = Expr::K::Unset;
    CHECK(!ValidateExpr(r.expr, err));
    r.expr->subquery->items.clear();
    CHECK(!ValidateExpr(r.expr, err));
    r.expr->subquery->items.push_back(SelectItem{});
    r.expr->subquery->items[0].wildcard = true;
    CHECK(ValidateExpr(r.expr, err));
    r.expr->subquery->into = "А.Б";
    CHECK(!ValidateExpr(r.expr, err));
}
TEST(StrictParser, BatchDumpShape) {
    CHECK_EQ(BatchDump("ВЫБРАТЬ 1 КАК Х"), std::string("(batch (select (items (item 1 КАК Х))))"));
    CHECK_EQ(BatchDump("УНИЧТОЖИТЬ ВТ"), std::string("(batch (drop ВТ))"));
}

// ---- review fixes ----

namespace {

ParseError BatchErr(const std::string& text) { return ParseBatchStrict(text).error; }

std::shared_ptr<Select> OneSelect(ParseResult& r) { return r.batch.statements[0].select; }

}  // namespace

TEST(StrictParser, AmbiguousClauseBeforeUnionIsRejected) {
    for (const char* q : {
             "ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ДЛЯ ИЗМЕНЕНИЯ ОБЪЕДИНИТЬ ВЫБРАТЬ 1",
             "ВЫБРАТЬ А.Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ИНДЕКСИРОВАТЬ ПО А.Х ОБЪЕДИНИТЬ ВЫБРАТЬ 1"}) {
        const ParseError e = BatchErr(q);
        CHECK_EQ(e.code, std::string("Q_PARSE_AMBIGUOUS_CLAUSE"));
        CHECK(e.pos.line >= 1 && e.pos.offset > 0);
    }
}
TEST(StrictParser, IndexNeedsIntoAndIsNotForUnionParts) {
    CHECK(Bad("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ИНДЕКСИРОВАТЬ ПО А.Х"));
    CHECK(Bad("ВЫБРАТЬ 1 ПОМЕСТИТЬ ВТ ОБЪЕДИНИТЬ ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ИНДЕКСИРОВАТЬ ПО А.Х"));
    auto r = ParseBatchStrict("ВЫБРАТЬ А.Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ИНДЕКСИРОВАТЬ ПО А.Х");
    CHECK(r.ok);
    ParseError err;
    OneSelect(r)->into.clear();
    CHECK(!ValidateBatch(r.batch, err));
    auto u = ParseBatchStrict("ВЫБРАТЬ 1 ПОМЕСТИТЬ ВТ ОБЪЕДИНИТЬ ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А");
    CHECK(u.ok);
    auto part = OneSelect(u)->unions[0].select;
    part->index_by.push_back(Make(Expr::K::NullLit));
    part->index_by[0]->text = "NULL";
    CHECK(!ValidateBatch(u.batch, err));
    part->index_by.clear();
    part->into = "ВТ2";
    CHECK(!ValidateBatch(u.batch, err));
}
TEST(StrictParser, UnionLimit) {
    std::string q = "ВЫБРАТЬ 1";
    for (int i = 0; i < 256; ++i) q += " ОБЪЕДИНИТЬ ВЫБРАТЬ 1";
    auto r = ParseBatchStrict(q);
    CHECK(r.ok);
    CHECK_EQ(OneSelect(r)->unions.size(), 256u);
    CHECK_EQ(BatchDump(q + " ОБЪЕДИНИТЬ ВЫБРАТЬ 1"), std::string("ERR:Q_PARSE_LIMIT"));
    OneSelect(r)->unions.push_back(OneSelect(r)->unions[0]);
    ParseError err;
    CHECK(!ValidateBatch(r.batch, err));
    CHECK_EQ(err.code, std::string("Q_PARSE_LIMIT"));
}
TEST(StrictParser, ValidatorEnforcesParserLimits) {
    ParseError err;
    auto t = ParseBatchStrict("ВЫБРАТЬ ПЕРВЫЕ 5 1");
    CHECK(t.ok);
    OneSelect(t)->top = "1234567890123456789";   // 19 digits
    CHECK(!ValidateBatch(t.batch, err));
    auto v = ParseBatchStrict("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Цены.Срез(1) КАК О");
    CHECK(v.ok);
    for (int i = 0; i < 40; ++i) OneSelect(v)->from[0].vt_params.push_back(nullptr);
    CHECK(!ValidateBatch(v.batch, err));
    CHECK(err.pos.line >= 1);
    auto p = ParseBatchStrict("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ИТОГИ ПО А.Х ПЕРИОДАМИ(1, 2, 3)");
    CHECK(p.ok);
    auto& item = OneSelect(p)->totals_by[0];
    for (int i = 0; i < 3; ++i) item.periods_args.push_back(item.periods_args[0]);
    CHECK(!ValidateBatch(p.batch, err));
    CHECK(Bad("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Цены.Срез(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33) КАК О"));
    CHECK(Bad("ВЫБРАТЬ ПЕРВЫЕ 1234567890123456789 1"));
}
TEST(StrictParser, ValidateBatchNegativesOnTamperedTrees) {
    const char* base =
        "ВЫБРАТЬ А.Х ПОМЕСТИТЬ ВТ ИЗ РегистрСведений.Р.Срез(&Д) КАК А "
        "ЛЕВОЕ СОЕДИНЕНИЕ Справочник.Б КАК Б ПО А.Х = Б.Ссылка "
        "ГДЕ А.Х = 1 СГРУППИРОВАТЬ ПО А.Х ИМЕЮЩИЕ А.Х > 0 УПОРЯДОЧИТЬ ПО А.Х "
        "ИТОГИ СУММА(А.Х) ПО ОБЩИЕ, А.Х ИЕРАРХИЯ";
    ParseError err;
    auto fresh = [&]() {
        auto r = ParseBatchStrict(base);
        CHECK(r.ok);
        return r;
    };
    { auto r = fresh(); OneSelect(r)->from[0].vt_params[0].reset(); CHECK(ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->from[0].vt_params[0] = std::make_shared<Expr>(); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->from[0].subquery = std::make_shared<Select>(); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->from[0].kind = Source::K::Table; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->from[0].kind = Source::K::Subquery; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->joins[0][0].on.reset(); CHECK(!ValidateBatch(r.batch, err)); CHECK(err.pos.line >= 1); CHECK(err.pos.offset > 0); }
    { auto r = fresh(); OneSelect(r)->joins[0][0].source.name = "1bad"; CHECK(!ValidateBatch(r.batch, err)); CHECK(err.pos.offset > 0); }
    { auto r = fresh(); OneSelect(r)->order_by[0].expr.reset(); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->group_by[0].reset(); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->having = std::make_shared<Expr>(); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->where.reset(); CHECK(ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->totals_overall = false; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->totals_by[0].expr = Make(Expr::K::NullLit); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->totals_by[1].only_hierarchy = true; OneSelect(r)->totals_by[1].hierarchy = false; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->totals_by[1].periods = true; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->has_totals = false; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->totals_aggregates[0].reset(); CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->items[0].alias = "1x"; CHECK(!ValidateBatch(r.batch, err)); }
    { auto r = fresh(); OneSelect(r)->for_update_tables.push_back("Т"); CHECK(!ValidateBatch(r.batch, err)); }
}
TEST(StrictParser, ValidateBatchBudgetAndCycles) {
    ParseError err;
    SelectItem w;
    w.wildcard = true;
    Batch b;
    Statement st;
    st.select = std::make_shared<Select>();
    st.select->items.assign(kMaxExprNodes + 10, w);
    b.statements.push_back(st);
    CHECK(!ValidateBatch(b, err));
    CHECK_EQ(err.code, std::string("Q_PARSE_LIMIT"));
    // A Select that is its own FROM subquery must terminate with an error.
    auto s = std::make_shared<Select>();
    s->items.push_back(w);
    Source src;
    src.kind = Source::K::Subquery;
    src.subquery = s;
    src.alias = "П";
    s->from.push_back(src);
    s->joins.emplace_back();
    Batch c;
    Statement cs;
    cs.select = s;
    c.statements.push_back(cs);
    CHECK(!ValidateBatch(c, err));
    CHECK_EQ(err.code, std::string("Q_PARSE_LIMIT"));
    s->from.clear();
    s->joins.clear();
    // A union part that is the statement itself is malformed and must not loop.
    Select::UnionPart up;
    up.select = s;
    s->unions.push_back(up);
    CHECK(!ValidateBatch(c, err));
    s->unions.clear();   // break the cycles so the shared_ptrs are released
}
TEST(StrictParser, FailuresCarryPositions) {
    for (const char* q : {"", ";", "ВЫБРАТЬ", "ВЫБРАТЬ Х ИЗ", "ВЫБРАТЬ 1 ПОМЕСТИТЬ А.Б", "ВЫБРАТЬ Х ИЗ А {Б}",
                          "ВЫБРАТЬ Х ИЗ А ЛЕВОЕ СОЕДИНЕНИЕ Б КАК Б", "ВЫБРАТЬ 1 ОБЪЕДИНИТЬ ВЫБРАТЬ 2 ПОМЕСТИТЬ В"}) {
        auto r = ParseBatchStrict(q);
        CHECK(!r.ok);
        CHECK(r.error.pos.line >= 1);
        CHECK(r.error.pos.col >= 1);
    }
}
TEST(StrictParser, SemicolonInsideStringAndComment) {
    auto r = ParseBatchStrict("ВЫБРАТЬ \"а;б\" КАК Х; ВЫБРАТЬ 2");
    CHECK(r.ok);
    CHECK_EQ(r.batch.statements.size(), 2u);
    auto c = ParseBatchStrict("ВЫБРАТЬ 1 // комментарий; УНИЧТОЖИТЬ ВТ\n; ВЫБРАТЬ 2");
    CHECK(c.ok);
    CHECK_EQ(c.batch.statements.size(), 2u);
    CHECK(c.batch.statements[1].kind == Statement::K::Select);
}
TEST(StrictParser, PerPartModifiersInUnion) {
    auto r = ParseBatchStrict("ВЫБРАТЬ РАЗЛИЧНЫЕ ПЕРВЫЕ 5 1 КАК Х ОБЪЕДИНИТЬ ВЫБРАТЬ РАЗРЕШЕННЫЕ ПЕРВЫЕ 7 2 ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ 3");
    CHECK(r.ok);
    auto s = OneSelect(r);
    CHECK(s->distinct && s->top == "5" && !s->allowed);
    CHECK(!s->unions[0].select->distinct && s->unions[0].select->top == "7" && s->unions[0].select->allowed);
    CHECK(s->unions[1].all && s->unions[1].select->top.empty());
}
TEST(StrictParser, SubqueryInVirtualTableParamIsWalked) {
    auto r = ParseBatchStrict("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Р.Срез(, Х В (ВЫБРАТЬ 1 КАК А)) КАК О");
    CHECK(r.ok);
    const auto& params = OneSelect(r)->from[0].vt_params;
    CHECK_EQ(params.size(), 2u);
    CHECK(params[0] == nullptr);
    CHECK(params[1] && params[1]->kind == Expr::K::InSubquery && params[1]->subquery);
    ParseError err;
    params[1]->subquery->items[0].expr->kind = Expr::K::Unset;
    CHECK(!ValidateBatch(r.batch, err));
}

// ---- fail-closed limits and error reporting ----

TEST(StrictParser, ErrorCarriesPosition) {
    auto r = ParseBatchStrict("ВЫБРАТЬ\n  Х ИЗ Справочник.А КАК А ГДЕ А.Х ПОДОБНО");
    CHECK(!r.ok);
    CHECK_EQ(r.error.pos.line, 2u);
    CHECK(r.error.code.rfind("Q_PARSE_", 0) == 0);
}

TEST(StrictParser, NeverThrowsOnGarbage) {
    const std::string seeds[] = {"", ";", ";;;", "ВЫБРАТЬ", ")", "\"", "&", "ВЫБРАТЬ (((((", "\xFF\xFE"};
    for (const auto& s : seeds) {
        auto r = ParseBatchStrict(s);
        CHECK(!r.ok);
        CHECK(r.error.code.rfind("Q_PARSE_", 0) == 0);
    }
}

TEST(StrictParser, EmptyBatchIsError) {
    CHECK_EQ(BatchDump(""), std::string("ERR:Q_PARSE_EMPTY"));
    CHECK_EQ(BatchDump("   \n// только комментарий\n"), std::string("ERR:Q_PARSE_EMPTY"));
}

TEST(StrictParser, InvalidUtf8IsTokenError) {
    CHECK_EQ(BatchDump("ВЫБРАТЬ \xFF КАК Х"), std::string("ERR:Q_PARSE_TOKEN"));
}

TEST(StrictParser, ErrorDetailNeverEchoesInput) {
    const std::string secret = "СекретноеИмя123";
    const std::string inputs[] = {
        "ВЫБРАТЬ " + secret + " ГДЕ", "ВЫБРАТЬ 1 КАК " + secret + secret + " ИЗ", "ВЫБРАТЬ \"" + secret + "\" ПОДОБНО",
        "ВЫБРАТЬ " + secret + "\xFF", "ВЫБРАТЬ " + secret + ".ВЫБРАТЬ", "ВЫБРАТЬ " + secret + ".A = = 1",
        "ВЫБРАТЬ Latin" + std::string("\xD0\xB0") + secret};
    for (const auto& in : inputs) {
        auto r = ParseBatchStrict(in);
        CHECK(!r.ok);
        CHECK(r.error.detail.find(secret) == std::string::npos);
        CHECK(r.error.detail.find("СекретноеИмя") == std::string::npos);
    }
}
