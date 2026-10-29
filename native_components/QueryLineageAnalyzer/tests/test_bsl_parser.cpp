#include "test_framework.h"
#include "bsl/bsl_parser.h"

#include <memory>
#include <string>

using namespace lineage::bsl;

namespace {

std::string CodeOf(const std::string& src) {
    const ParseResult r = ParseBsl(src);
    return r.ok ? std::string("OK") : r.error.code;
}

ExprP Mk(Expr::K k) {
    auto e = std::make_shared<Expr>();
    e->kind = k;
    return e;
}

ExprP VarNode(const std::string& name) {
    ExprP e = Mk(Expr::K::Var);
    e->text = name;
    return e;
}

StmtP AssignStmt(ExprP lv, ExprP v) {
    auto s = std::make_shared<Stmt>();
    s->kind = Stmt::K::Assign;
    s->exprs = {std::move(lv), std::move(v)};
    return s;
}

}  // namespace

TEST(BslParser, AcceptsSubset) {
    const char* code =
        "Запрос = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А\");\n"
        "Выборка = Запрос.Выполнить().Выбрать();\n"
        "Массив = Новый Массив;\n"
        "Пока Выборка.Следующий() Цикл\n  Массив.Добавить(Выборка.С);\nКонецЦикла;\n"
        "Результат = Новый Структура(\"Ссылки\", Массив);";
    CHECK(ParseBsl(code).ok);
}

TEST(BslParser, RejectsOutsideSubset) {
    for (const char* bad : {"Выполнить(\"А=1\");", "А = Вычислить(\"1\");", "Перейти ~М;", "Процедура П() КонецПроцедуры",
                            "А = Новый(\"Запрос\");", "ВызватьИсключение Т;", "Если А Тогда", "А = ;"})
        CHECK(!ParseBsl(bad).ok);
}

TEST(BslParser, ReasonCodes) {
    CHECK_EQ(CodeOf("Выполнить(\"А=1\");"), std::string("C_EVAL"));
    CHECK_EQ(CodeOf("Execute(\"А=1\");"), std::string("C_EVAL"));
    CHECK_EQ(CodeOf("А = Вычислить(\"1\");"), std::string("C_EVAL"));
    CHECK_EQ(CodeOf("А = eval(\"1\");"), std::string("C_EVAL"));
    CHECK_EQ(CodeOf("А = Новый(\"Запрос\");"), std::string("C_DYNAMIC_NEW"));
    CHECK_EQ(CodeOf("ВызватьИсключение Т;"), std::string("C_RAISE"));
    CHECK_EQ(CodeOf("ДобавитьОбработчик А.Б, В.Г;"), std::string("C_HANDLER"));
    CHECK_EQ(CodeOf("УдалитьОбработчик А.Б, В.Г;"), std::string("C_HANDLER"));
    for (const char* bad : {"Перейти ~М;", "~М: А = 1;", "Процедура П() КонецПроцедуры", "Функция Ф() КонецФункции",
                            "Перем А;", "Возврат 1;", "Асинх А = 1;", "А = Ждать Б;", "#Если Сервер Тогда", "&НаСервере",
                            "А = 1 /* x */;", "А = {1};"})
        CHECK_EQ(CodeOf(bad), std::string("C_PARSE"));
}

TEST(BslParser, KeywordAfterDotIsMemberName) {
    CHECK(ParseBsl("Р = З.Выполнить().Выбрать(); Пока Р.Следующий() Цикл КонецЦикла;").ok);
    CHECK(ParseBsl("А = Б.Новый; В = Г.Если; Д.Для = 1; Е.Выполнить();").ok);
    ParseResult r = ParseBsl("Т.Если = 1;");
    CHECK(r.ok);
    CHECK_EQ(r.body.stmts.size(), static_cast<size_t>(1));
    CHECK(r.body.stmts[0]->kind == Stmt::K::Assign);
    CHECK_EQ(r.body.stmts[0]->exprs[0]->text, std::string("Если"));   // source spelling kept
    // Even a keyword member must not change structure: no block opens.
    CHECK(!ParseBsl("Т.Если А Тогда").ok);
    CHECK_EQ(DumpBsl(ParseBsl("Т.Если = Т.ЕСЛИ;").body), DumpBsl(ParseBsl("Т.Если = Т.ЕСЛИ;").body));
}

TEST(BslParser, KeywordAsFirstSegmentOrVariableIsRejected) {
    for (const char* bad : {"Новый = 1;", "Если = 1;", "Для = 2;", "Знач = 1;", "А = Если;", "А = Тогда;"})
        CHECK(!ParseBsl(bad).ok);
}

TEST(BslParser, MixedScriptIdentifiersAreRejected) {
    CHECK(!ParseBsl("Aб = 1;").ok);                 // Latin A + Cyrillic б
    CHECK(!ParseBsl("А = Бa.Имя;").ok);
    CHECK(!ParseBsl("А = Б.Иmя;").ok);              // mixed member name
    CHECK(!ParseBsl("Для Кaждый Из А Цикл КонецЦикла;").ok);
    CHECK(ParseBsl("Abc = 1; Абв = Abc;").ok);      // each name is single-script
}

TEST(BslParser, Statements) {
    CHECK(ParseBsl("Если А Тогда Б = 1; ИначеЕсли В Тогда Б = 2; Иначе Б = 3; КонецЕсли;").ok);
    CHECK(ParseBsl("Если А Тогда Б = 1 КонецЕсли").ok);   // last statement before a terminator may omit ';'
    CHECK(ParseBsl("Для Каждого Э Из М Цикл Если Э = 1 Тогда Продолжить; КонецЕсли; Прервать; КонецЦикла;").ok);
    CHECK(ParseBsl("Для Н = 1 По 10 Цикл А.Добавить(Н); КонецЦикла;").ok);
    CHECK(ParseBsl("Попытка А = 1; Исключение Б = 2; КонецПопытки;").ok);
    CHECK(ParseBsl("").ok);
    CHECK(ParseBsl("// only a comment\n").ok);
    CHECK(!ParseBsl("Прервать;").ok);                       // outside a loop
    CHECK(!ParseBsl("Если А Тогда Продолжить; КонецЕсли;").ok);
    CHECK(!ParseBsl("Попытка А = 1; КонецПопытки;").ok);    // no Исключение
    CHECK(!ParseBsl("А = 1 Б = 2;").ok);                    // missing ';'
    CHECK(!ParseBsl("А = 1;;").ok);                         // empty statement
    CHECK(!ParseBsl("КонецЕсли;").ok);
    CHECK(!ParseBsl("А.Б() = 1;").ok);                      // call inside an lvalue
    CHECK(!ParseBsl("А = 1;\n1 + 2;").ok);
    CHECK(!ParseBsl("А;").ok);                              // a bare name is not a call statement
    CHECK(!ParseBsl("Если А = 1 = 2 Тогда КонецЕсли;").ok); // comparison is not chainable
}

TEST(BslParser, Expressions) {
    CHECK(ParseBsl("А = -Б + В * (Г - 1) / 2 % 3;").ok);
    CHECK(ParseBsl("А = Не Б Или В И Г <> Д;").ok);
    CHECK(ParseBsl("А = ?(Б > 1, \"x\", Неопределено);").ok);
    CHECK(ParseBsl("А = Новый Структура(); Б = Новый Массив(5); В = '20240131' = Дата;").ok);
    CHECK(ParseBsl("А = Б[0].В[Г.Д]; А.Б[1] = NULL; А = Истина; А = Ложь;").ok);
    CHECK(!ParseBsl("А = +1;").ok);
    CHECK(!ParseBsl("А = Ф(,1);").ok);                      // omitted argument
    CHECK(!ParseBsl("А = Ф(1,);").ok);
    CHECK(!ParseBsl("А = Ф()();").ok);                      // callee is not a plain name
    CHECK(!ParseBsl("А = Б[0](1);").ok);
    CHECK(!ParseBsl("А = ?(1, 2);").ok);
    CHECK(!ParseBsl("А = Новый Массив.Количество();").ok);  // no postfix directly on Новый
    // ... except a member chain on "Новый Запрос(...)" (the object is never bound, so it cannot escape).
    CHECK(ParseBsl("А = Новый Запрос(\"ВЫБРАТЬ 1\").Выполнить().Выгрузить();").ok);
    CHECK(ParseBsl("А = Новый Query(\"ВЫБРАТЬ 1\").Execute().Select().Следующий();").ok);
    CHECK_EQ(DumpBslExpr(*ParseBsl("А = Новый Запрос(\"x\").Выполнить();").body.stmts[0]->exprs[1]),
             DumpBslExpr(*ParseBsl("А = Новый Запрос(\"x\").Выполнить();").body.stmts[0]->exprs[1]));
    for (const char* bad : {"А = Новый Запрос.Выполнить();", "А = Новый Массив(1).Количество();",
                            "А = Новый Запрос(\"x\")[0];", "А = Новый Запрос(\"x\")(1);",
                            "Новый Запрос(\"x\").Выполнить();", "А = Новый Структура(\"К\", 1).К;"}) {
        CHECK(!ParseBsl(bad).ok);
    }
    CHECK(!ParseBsl("А = (1;").ok);
    CHECK(!ParseBsl("А = 1 2;").ok);
}

TEST(BslParser, UnknownTokensAndLimits) {
    for (const char* bad : {"А = 1; ~М:", "Т = \"a\nb\";", "А = `x`;", "А = 'не дата';", "А = 1 // \xC2\xA0 x\n;"})
        CHECK(!ParseBsl(bad).ok);
    CHECK_EQ(CodeOf(std::string(1024 * 1024 + 1, ' ')), std::string("C_LIMIT"));

    std::string deepParen = "А = " + std::string(100, '(') + "1" + std::string(100, ')') + ";";
    CHECK_EQ(CodeOf(deepParen), std::string("C_LIMIT"));

    std::string deepIf;
    for (int i = 0; i < 40; ++i) deepIf += "Если А Тогда ";
    for (int i = 0; i < 40; ++i) deepIf += "КонецЕсли; ";
    CHECK_EQ(CodeOf(deepIf), std::string("C_LIMIT"));

    std::string many;
    for (int i = 0; i < 2001; ++i) many += "А = 1;";
    CHECK_EQ(CodeOf(many), std::string("C_LIMIT"));
    std::string exact;
    for (int i = 0; i < 2000; ++i) exact += "А = 1;";
    CHECK(ParseBsl(exact).ok);

    std::string chain = "А = Б";
    for (int i = 0; i < 400; ++i) chain += ".В";
    CHECK_EQ(CodeOf(chain + ";"), std::string("C_LIMIT"));
    std::string sum = "А = 1";
    for (int i = 0; i < 400; ++i) sum += " + 1";
    CHECK_EQ(CodeOf(sum + ";"), std::string("C_LIMIT"));
}

TEST(BslParser, ErrorsCarryPositionAndNeverEchoInput) {
    const ParseResult r = ParseBsl("А = 1;\nБ = СекретноеЗначение123 +;");
    CHECK(!r.ok);
    CHECK_EQ(r.error.pos.line, static_cast<uint32_t>(2));
    CHECK(r.error.detail.find("Секретное") == std::string::npos);
    CHECK(r.error.detail.find("123") == std::string::npos);
    CHECK(r.body.stmts.empty());                            // no partial tree on failure
}

TEST(BslParser, ValidatorRejectsMalformedTrees) {
    ParseError err;
    Body b;
    CHECK(ValidateBody(b, err));                            // empty body is fine

    auto unset = std::make_shared<Stmt>();                  // kind Unset
    Body unsetBody;
    unsetBody.stmts.push_back(unset);
    CHECK(!ValidateBody(unsetBody, err));

    Body nullStmt;
    nullStmt.stmts.push_back(nullptr);
    CHECK(!ValidateBody(nullStmt, err));

    Body okBody;
    okBody.stmts.push_back(AssignStmt(VarNode("А"), VarNode("Б")));
    CHECK(ValidateBody(okBody, err));

    Body unsetExpr;
    unsetExpr.stmts.push_back(AssignStmt(VarNode("А"), Mk(Expr::K::Unset)));
    CHECK(!ValidateBody(unsetExpr, err));

    Body badName;
    badName.stmts.push_back(AssignStmt(VarNode("A б"), VarNode("Б")));
    CHECK(!ValidateBody(badName, err));
    Body mixed;
    mixed.stmts.push_back(AssignStmt(VarNode("Aб"), VarNode("Б")));
    CHECK(!ValidateBody(mixed, err));

    ExprP badNum = Mk(Expr::K::Lit);
    badNum->lit = Expr::L::Number;
    badNum->text = "1e5";
    Body numBody;
    numBody.stmts.push_back(AssignStmt(VarNode("А"), badNum));
    CHECK(!ValidateBody(numBody, err));

    ExprP flagged = VarNode("Б");
    flagged->has_parens = true;                             // flag on a kind that does not use it
    Body flagBody;
    flagBody.stmts.push_back(AssignStmt(VarNode("А"), flagged));
    CHECK(!ValidateBody(flagBody, err));

    ExprP litOnVar = VarNode("Б");
    litOnVar->lit = Expr::L::String;
    Body litBody;
    litBody.stmts.push_back(AssignStmt(VarNode("А"), litOnVar));
    CHECK(!ValidateBody(litBody, err));

    ExprP badArity = Mk(Expr::K::Binary);
    badArity->text = "+";
    badArity->args = {VarNode("А")};
    Body arityBody;
    arityBody.stmts.push_back(AssignStmt(VarNode("А"), badArity));
    CHECK(!ValidateBody(arityBody, err));

    ExprP nullChild = Mk(Expr::K::Member);
    nullChild->text = "М";
    nullChild->args = {nullptr};
    Body nullBody;
    nullBody.stmts.push_back(AssignStmt(VarNode("А"), nullChild));
    CHECK(!ValidateBody(nullBody, err));

    auto brk = std::make_shared<Stmt>();
    brk->kind = Stmt::K::Break;
    Body brkBody;
    brkBody.stmts.push_back(brk);
    CHECK(!ValidateBody(brkBody, err));

    ExprP callLv = Mk(Expr::K::Call);
    callLv->text = "Ф";
    Body lvBody;
    lvBody.stmts.push_back(AssignStmt(callLv, VarNode("Б")));
    CHECK(!ValidateBody(lvBody, err));
}

TEST(BslParser, ValidatorBoundsDeepAndSharedTrees) {
    ParseError err;
    // Deep chain built by hand, far deeper than the budget: rejected without recursing.
    ExprP deep = VarNode("А");
    for (int i = 0; i < 100000; ++i) {
        ExprP m = Mk(Expr::K::Member);
        m->text = "Б";
        m->args = {deep};
        deep = m;
    }
    Body body;
    body.stmts.push_back(AssignStmt(VarNode("А"), deep));
    CHECK(!ValidateBody(body, err));
    CHECK_EQ(err.code, std::string("C_LIMIT"));
    // Tear the chain down iteratively so the test does not overflow the stack in the destructor.
    ExprP cur = std::move(deep);
    body.stmts.clear();
    while (cur && cur.use_count() == 1) {
        ExprP next = cur->args.empty() ? nullptr : cur->args[0];
        cur->args.clear();
        cur = std::move(next);
    }

    // Shared-child DAG: 2^40 visits if walked naively; the node budget stops it.
    ExprP node = VarNode("А");
    for (int i = 0; i < 40; ++i) {
        ExprP bin = Mk(Expr::K::Binary);
        bin->text = "+";
        bin->args = {node, node};
        node = bin;
    }
    Body dag;
    dag.stmts.push_back(AssignStmt(VarNode("А"), node));
    CHECK(!ValidateBody(dag, err));
}

TEST(BslParser, DumpIsDeterministicAndDistinguishesShapes) {
    CHECK_EQ(DumpBsl(ParseBsl("А = Б + В * Г;").body), DumpBsl(ParseBsl("А=Б+В*Г;").body));
    CHECK(DumpBsl(ParseBsl("А = (Б + В) * Г;").body) != DumpBsl(ParseBsl("А = Б + В * Г;").body));
    CHECK(DumpBsl(ParseBsl("А = Новый Массив;").body) != DumpBsl(ParseBsl("А = Новый Массив();").body));
    CHECK(DumpBsl(ParseBsl("А = \"1\";").body) != DumpBsl(ParseBsl("А = 1;").body));
    CHECK(DumpBsl(ParseBsl("А = Б.В();").body) != DumpBsl(ParseBsl("А = Б.В;").body));
}

TEST(BslParser, StringValuesAreDecoded) {
    ParseResult r = ParseBsl("А = \"x\"\"y\n  |z\";");
    CHECK(r.ok);
    const Expr& lit = *r.body.stmts[0]->exprs[1];
    CHECK(lit.kind == Expr::K::Lit && lit.lit == Expr::L::String);
    CHECK_EQ(lit.text, std::string("x\"y\nz"));
}

TEST(BslParser, PlainNameRules) {
    CHECK_EQ(CodeOf("А = Новый Вычислить;"), std::string("C_EVAL"));
    CHECK_EQ(CodeOf("Для Каждого Вычислить Из М Цикл КонецЦикла;"), std::string("C_EVAL"));
    CHECK_EQ(CodeOf("Для Eval = 1 По 2 Цикл КонецЦикла;"), std::string("C_EVAL"));
    CHECK(!ParseBsl("Для Каждого Если Из М Цикл КонецЦикла;").ok);
    CHECK(ParseBsl("А = Б.Вычислить(); Б.Выполнить();").ok);
}

TEST(BslParser, ValidatorPlainNamesAndPositions) {
    ParseError err;
    for (const char* bad : {"Вычислить", "Выполнить", "Execute", "ДобавитьОбработчик", "УдалитьОбработчик", "Если",
                            "ЕСЛИ", "Новый", "Цикл"}) {
        Body v;
        v.stmts.push_back(AssignStmt(VarNode("А"), VarNode(bad)));
        CHECK(!ValidateBody(v, err));
        Body c;
        ExprP call = Mk(Expr::K::Call);
        call->text = bad;
        c.stmts.push_back(AssignStmt(VarNode("А"), call));
        CHECK(!ValidateBody(c, err));
        Body n;
        ExprP nw = Mk(Expr::K::New);
        nw->text = bad;
        n.stmts.push_back(AssignStmt(VarNode("А"), nw));
        CHECK(!ValidateBody(n, err));
        Body f;
        auto fe = std::make_shared<Stmt>();
        fe->kind = Stmt::K::ForEach;
        fe->var = bad;
        fe->exprs = {VarNode("М")};
        fe->blocks.emplace_back();
        f.stmts.push_back(fe);
        CHECK(!ValidateBody(f, err));
    }
    Body m;
    ExprP mem = Mk(Expr::K::Member);
    mem->text = "Выполнить";
    mem->args = {VarNode("Б")};
    m.stmts.push_back(AssignStmt(VarNode("А"), mem));
    CHECK(ValidateBody(m, err));

    Body p;
    ExprP bad = VarNode("Если");
    bad->pos = Pos{42, 3, 7};
    p.stmts.push_back(AssignStmt(VarNode("А"), bad));
    CHECK(!ValidateBody(p, err));
    CHECK_EQ(err.pos.line, static_cast<uint32_t>(3));
    CHECK_EQ(err.pos.col, static_cast<uint32_t>(7));
}

TEST(BslParser, BlockDepthCountsCompoundStatementsOnly) {
    std::string ok;
    for (int i = 0; i < 32; ++i) ok += "Если А Тогда ";
    for (int i = 0; i < 100; ++i) ok += "Б = 1; ";
    for (int i = 0; i < 32; ++i) ok += "КонецЕсли; ";
    CHECK(ParseBsl(ok).ok);
    std::string over;
    for (int i = 0; i < 33; ++i) over += "Если А Тогда ";
    for (int i = 0; i < 33; ++i) over += "КонецЕсли; ";
    CHECK_EQ(CodeOf(over), std::string("C_LIMIT"));
}
