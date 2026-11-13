#include "test_framework.h"
#include "strict/canonical_printer.h"
#include "strict/strict_parser.h"
#include "strict/ast_dump.h"

#include <memory>
#include <string>
#include <vector>

using namespace lineage::strict;

namespace {

// Parse -> print -> parse -> print. The printer names unaliased select items, so the first tree is compared
// with its names filled in; the second print must equal the first one (idempotence).
void RoundTrip(const std::string& q) {
    auto a = ParseBatchStrict(q);
    if (!a.ok) CHECK_EQ(a.error.code + "@" + std::to_string(a.error.pos.offset), std::string("OK"));
    const PrintResult p1 = PrintCanonicalChecked(a.batch);
    if (!p1.ok) CHECK_EQ(p1.error.code + ": " + p1.error.detail, std::string("OK"));
    auto b = ParseBatchStrict(p1.text);
    if (!b.ok) CHECK_EQ(b.error.code + "@" + std::to_string(b.error.pos.offset) + " " + p1.text, std::string("OK"));
    CHECK_EQ(DumpBatch(b.batch), DumpBatch(a.batch, true));   // same tree, subquery bodies included
    CHECK_EQ(PrintCanonical(b.batch), p1.text);               // idempotent
}

std::string Canon(const std::string& q) {
    auto a = ParseBatchStrict(q);
    return a.ok ? PrintCanonical(a.batch) : "PARSE-ERR:" + a.error.code;
}

ExprP Node(Expr::K kind, std::string text = "") {
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    e->text = std::move(text);
    return e;
}
ExprP FieldOf(const std::string& name) {
    ExprP e = Node(Expr::K::Field);
    e->parts.push_back(name);
    return e;
}

// A one-statement batch whose only select item is `expr`.
Batch BatchOf(const ExprP& expr, const std::string& alias = "") {
    auto sel = std::make_shared<Select>();
    SelectItem it;
    it.expr = expr;
    it.alias = alias;
    it.alias_explicit = !alias.empty();
    sel->items.push_back(it);
    Statement st;
    st.select = sel;
    Batch b;
    b.statements.push_back(st);
    return b;
}

bool Refuses(const Batch& b) {
    const PrintResult r = PrintCanonicalChecked(b);
    return !r.ok && r.text.empty() && !r.error.code.empty() && PrintCanonical(b).empty();
}

}  // namespace

TEST(Canonical, RoundTripCorpusOfShapes) {
    RoundTrip("select Т.Х as Y from Справочник.А as Т where Т.Х = &П");
    RoundTrip("ВЫБРАТЬ \"a\"\"b\" КАК С");
    RoundTrip("ВЫБРАТЬ \"//не комментарий\" КАК С // а это комментарий\n");
    RoundTrip("ВЫБРАТЬ \"строка\nс переводом\" КАК С");
    RoundTrip("ВЫБРАТЬ 1 КАК Х ПОМЕСТИТЬ ВТ; ВЫБРАТЬ ВТ.Х ИЗ ВТ КАК ВТ; УНИЧТОЖИТЬ ВТ");
    RoundTrip("ВЫБРАТЬ (1 + 2) * 3 КАК Х, -(-1) КАК Y, НЕ (А И Б) КАК Z ИЗ Справочник.Т КАК Т");
    RoundTrip("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Цены.СрезПоследних(, Номенклатура = &Н) КАК О");
}

TEST(Canonical, RoundTripEveryClauseAndNodeKind) {
    RoundTrip("ВЫБРАТЬ РАЗРЕШЕННЫЕ РАЗЛИЧНЫЕ ПЕРВЫЕ 10 Т.А, Т.Б.К.Г, СУММА(Т.Ц) КАК Итого, КОЛИЧЕСТВО(*), "
              "КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Т.А) КАК К, Т.* ИЗ Справочник.А КАК Т "
              "ЛЕВОЕ СОЕДИНЕНИЕ РегистрНакопления.Р.Остатки(&Д, Склад = &С) КАК О ПО Т.А = О.А И О.Б > 0 "
              "ПОЛНОЕ ВНЕШНЕЕ СОЕДИНЕНИЕ (ВЫБРАТЬ 1 КАК Х) КАК П ПО ИСТИНА, Справочник.Б КАК Б "
              "ГДЕ Т.А В (1, 2, 3) И Т.Б НЕ МЕЖДУ 1 И 5 ИЛИ Т.Ф ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"~\" "
              "СГРУППИРОВАТЬ ПО Т.А, Т.Б ИМЕЮЩИЕ СУММА(Т.Ц) > 0 ДЛЯ ИЗМЕНЕНИЯ Справочник.А "
              "УПОРЯДОЧИТЬ ПО Т.А УБЫВ, Т.Б АВТОУПОРЯДОЧИВАНИЕ ИТОГИ СУММА(Т.Ц) ПО ОБЩИЕ, Т.А ИЕРАРХИЯ КАК Н");
    RoundTrip("ВЫБРАТЬ ВЫБОР КОГДА Т.А > 0 ТОГДА \"+\" КОГДА Т.А < 0 ТОГДА \"-\" ИНАЧЕ \"0\" КОНЕЦ КАК З, "
              "ВЫБОР Т.А КОГДА 1 ТОГДА 2 КОНЕЦ КАК З2, ВЫРАЗИТЬ(Т.А КАК Число(15, 2)) КАК Ч, "
              "ВЫРАЗИТЬ(Т.А КАК Справочник.Б) КАК С, ЗНАЧЕНИЕ(Справочник.А.Пустая) КАК ПС, "
              "ТИП(Справочник.А) КАК ТС, ЕСТЬNULL(Т.А, 0) КАК ЕН, ТИПЗНАЧЕНИЯ(Т.А) КАК ТЗ, "
              "ПРЕДСТАВЛЕНИЕ(Т.А) КАК ПР, ПРЕДСТАВЛЕНИЕССЫЛКИ(Т.А) КАК ПСС, НЕОПРЕДЕЛЕНО КАК Н, NULL КАК НН, "
              "ЛОЖЬ КАК Л, 1.5 КАК Д ИЗ Справочник.А КАК Т "
              "ГДЕ Т.А ЕСТЬ NULL И Т.Б ЕСТЬ НЕ NULL И Т.Ф ССЫЛКА Справочник.Б И Т.Г НЕ ССЫЛКА Справочник.Б "
              "И Т.Д НЕ В ИЕРАРХИИ (&Р) И (Т.Е, Т.Ж) В (ВЫБРАТЬ Б.А, Б.Б ИЗ Справочник.Б КАК Б)");
    RoundTrip("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ Б.Х ИЗ Справочник.Б КАК Б "
              "ОБЪЕДИНИТЬ ВЫБРАТЬ Ж.Х КАК Х ИЗ Справочник.Г КАК Ж УПОРЯДОЧИТЬ ПО Х");
    RoundTrip("ВЫБРАТЬ Т.А ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК Т ИНДЕКСИРОВАТЬ ПО Т.А");
    RoundTrip("ВЫБРАТЬ 1 КАК Х ГДЕ ЛОЖЬ ИТОГИ ПО Х ТОЛЬКО ИЕРАРХИЯ КАК Н");
    RoundTrip("ВЫБРАТЬ 1 КАК Х ИТОГИ ПО Х ПЕРИОДАМИ(ДЕНЬ, 1, 2) КАК Н");
}

TEST(Canonical, SubqueryBodiesAreVisibleToTheDump) {
    auto a = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х ГДЕ 1 В (ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т)");
    CHECK(a.ok);
    CHECK(DumpBatch(a.batch).find("Справочник.А") != std::string::npos);
    // Two trees that differ only inside the subquery must dump differently.
    auto b = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х ГДЕ 1 В (ВЫБРАТЬ Т.А ИЗ Справочник.Б КАК Т)");
    CHECK(b.ok);
    CHECK(DumpBatch(a.batch) != DumpBatch(b.batch));
}

TEST(Canonical, CommentsDroppedKeywordsNormalized) {
    auto a = ParseBatchStrict("select /*x*/ 1");          // block comment -> parse error, not dropped
    CHECK(!a.ok);
    auto b = ParseBatchStrict("select 1 as x // comment");
    CHECK(b.ok);
    CHECK_EQ(PrintCanonical(b.batch), std::string("ВЫБРАТЬ\n\t1 КАК x"));
    auto c = ParseBatchStrict("ВЫБРАТЬ Т.Ссылка.Имя ИЗ Справочник.А КАК Т");
    CHECK_EQ(PrintCanonical(c.batch), std::string("ВЫБРАТЬ\n\tТ.Ссылка.Имя КАК СсылкаИмя\nИЗ\n\tСправочник.А КАК Т"));
}

TEST(Canonical, PipeAndBraceNeverReachExecution) {
    CHECK(!ParseBatchStrict("ВЫБРАТЬ 1\n|ИЗ Справочник.А").ok);
    CHECK(!ParseBatchStrict("ВЫБРАТЬ 1 {ГДЕ 1 = 1}").ok);
}

TEST(Canonical, ParenthesesPreserveMeaning) {
    auto a = ParseBatchStrict("ВЫБРАТЬ 1 - (2 - 3) КАК Х");
    CHECK_EQ(PrintCanonical(a.batch), std::string("ВЫБРАТЬ\n\t1 - (2 - 3) КАК Х"));
    CHECK_EQ(Canon("ВЫБРАТЬ 1 - 2 - 3 КАК Х"), std::string("ВЫБРАТЬ\n\t1 - 2 - 3 КАК Х"));
    CHECK_EQ(Canon("ВЫБРАТЬ (1 + 2) * 3 КАК Х"), std::string("ВЫБРАТЬ\n\t(1 + 2) * 3 КАК Х"));
    CHECK_EQ(Canon("ВЫБРАТЬ 1 + (2 * 3) КАК Х"), std::string("ВЫБРАТЬ\n\t1 + 2 * 3 КАК Х"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ А ИЛИ (Б ИЛИ Г)"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ А ИЛИ (Б ИЛИ Г)"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ (А ИЛИ Б) И Г"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ (А ИЛИ Б) И Г"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ НЕ А И НЕ (Б = 1)"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ НЕ А И НЕ (Б = 1)"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ (А = 1) = (Б = 2)"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ (А = 1) = (Б = 2)"));
    CHECK_EQ(Canon("ВЫБРАТЬ -(1 + 2) КАК Х, -А КАК Y"), std::string("ВЫБРАТЬ\n\t-(1 + 2) КАК Х,\n\t-А КАК Y"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ (А + 1) В (1) И (А МЕЖДУ 1 И 2)"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ А + 1 В (1) И (А МЕЖДУ 1 И 2)"));
}

TEST(Canonical, ResultNamesAreExplicitAndPositional) {
    CHECK_EQ(Canon("ВЫБРАТЬ Т.А, Т.Б.Г, СУММА(Т.Ц), КОЛИЧЕСТВО(*), 1, Т.* ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.А КАК А,\n\tТ.Б.Г КАК БГ,\n\tСУММА(Т.Ц) КАК Поле3,\n\tКОЛИЧЕСТВО(*) КАК Поле4,"
                         "\n\t1 КАК Поле5,\n\tТ.*\nИЗ\n\tСправочник.А КАК Т"));
    CHECK_EQ(Canon("ВЫБРАТЬ Т.А + 1, ВЫБОР КОГДА Т.А ТОГДА 1 КОНЕЦ, \"s\", NULL ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.А + 1 КАК Поле1,\n\tВЫБОР КОГДА Т.А ТОГДА 1 КОНЕЦ КАК Поле2,\n\t\"s\" КАК Поле3,"
                         "\n\tNULL КАК Поле4\nИЗ\n\tСправочник.А КАК Т"));
}

TEST(Canonical, KeywordDerivedNamesKeepTheKeyword) {
    // An allowlisted path keyword is a valid column name after КАК, so the platform's own name is kept.
    CHECK_EQ(Canon("ВЫБРАТЬ Т.Ссылка, Т.Представление ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.Ссылка КАК Ссылка,\n\tТ.Представление КАК Представление\nИЗ\n\tСправочник.А КАК Т"));
    // Uniqueness: a second Ссылка gets a suffix, an explicit alias keeps its name wherever it stands.
    CHECK_EQ(Canon("ВЫБРАТЬ А.Ссылка, Б.ссылка, 1 КАК Ссылка2 ИЗ Справочник.А КАК А, Справочник.Б КАК Б"),
             std::string("ВЫБРАТЬ\n\tА.Ссылка КАК Ссылка,\n\tБ.ссылка КАК ссылка1,\n\t1 КАК Ссылка2\nИЗ\n\t"
                         "Справочник.А КАК А,\n\tСправочник.Б КАК Б"));
    CHECK_EQ(Canon("ВЫБРАТЬ Т.Ссылка, 1 КАК Ссылка ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.Ссылка КАК Ссылка1,\n\t1 КАК Ссылка\nИЗ\n\tСправочник.А КАК Т"));
    RoundTrip("ВЫБРАТЬ Т.Ссылка, Т.Значение, Т.ПредставлениеСсылки ИЗ Справочник.А КАК Т");
    // A keyword that is not a path keyword never becomes a name (it cannot be a segment anyway).
    // Latin + Cyrillic derived name: positional name instead of a refusal.
    CHECK_EQ(Canon("ВЫБРАТЬ Т.Ссылка.abc ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.Ссылка.abc КАК Поле1\nИЗ\n\tСправочник.А КАК Т"));
}

TEST(Canonical, KeywordColumnAliasIsPrintedAsWritten) {
    CHECK_EQ(Canon("ВЫБРАТЬ Т.Ссылка КАК Ссылка, Т.А КАК представление ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.Ссылка КАК Ссылка,\n\tТ.А КАК представление\nИЗ\n\tСправочник.А КАК Т"));
    RoundTrip("ВЫБРАТЬ Т.Ссылка КАК Ссылка, Т.Б КАК ПредставлениеСсылки, Т.Г КАК Значение ИЗ Справочник.А КАК Т");
    RoundTrip("ВЫБРАТЬ 1 КАК А ОБЪЕДИНИТЬ ВЫБРАТЬ 2 КАК Значение");
    RoundTrip("ВЫБРАТЬ Т.Ссылка КАК Ссылка ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК Т; ВЫБРАТЬ ВТ.Ссылка КАК Ссылка ИЗ ВТ КАК ВТ");
    // A keyword alias collides with an equal explicit name like any other.
    auto a = ParseBatchStrict("ВЫБРАТЬ 1 КАК Ссылка, 2 КАК ссылка");
    CHECK(a.ok);
    CHECK_EQ(PrintCanonicalChecked(a.batch).error.code, std::string("Q_PRINT_DUPLICATE_COLUMN"));
    // A hand-built alias that is any other keyword is refused.
    auto b = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х");
    CHECK(b.ok);
    b.batch.statements[0].select->items[0].alias = "Тип";
    CHECK_EQ(PrintCanonicalChecked(b.batch).error.code, std::string("Q_PRINT_IDENT"));
}

TEST(Canonical, DuplicateDerivedNamesGetSuffixes) {
    CHECK_EQ(Canon("ВЫБРАТЬ А.Имя, Б.Имя, В1.имя ИЗ Справочник.А КАК А, Справочник.Б КАК Б, Справочник.Г КАК В1"),
             std::string("ВЫБРАТЬ\n\tА.Имя КАК Имя,\n\tБ.Имя КАК Имя1,\n\tВ1.имя КАК имя2\nИЗ\n\tСправочник.А КАК А,"
                         "\n\tСправочник.Б КАК Б,\n\tСправочник.Г КАК В1"));
    CHECK_EQ(Canon("ВЫБРАТЬ СУММА(А.Х), СУММА(А.Y) ИЗ Справочник.А КАК А"),
             std::string("ВЫБРАТЬ\n\tСУММА(А.Х) КАК Поле1,\n\tСУММА(А.Y) КАК Поле2\nИЗ\n\tСправочник.А КАК А"));
    CHECK_EQ(Canon("ВЫБРАТЬ Т.А.Б, Т.АБ ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.А.Б КАК АБ,\n\tТ.АБ КАК АБ1\nИЗ\n\tСправочник.А КАК Т"));
    // ё and е collide.
    CHECK_EQ(Canon("ВЫБРАТЬ Т.Ёж, Т.Еж ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.Ёж КАК Ёж,\n\tТ.Еж КАК Еж1\nИЗ\n\tСправочник.А КАК Т"));
    // Derived vs explicit: the explicit alias keeps its name wherever it stands.
    CHECK_EQ(Canon("ВЫБРАТЬ Т.Имя, Т.Б КАК Имя ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.Имя КАК Имя1,\n\tТ.Б КАК Имя\nИЗ\n\tСправочник.А КАК Т"));
    // A suffixed name never collides with a later natural one.
    CHECK_EQ(Canon("ВЫБРАТЬ А.Х, Б.Х, В1.Х1 ИЗ Справочник.А КАК А, Справочник.Б КАК Б, Справочник.Г КАК В1"),
             std::string("ВЫБРАТЬ\n\tА.Х КАК Х,\n\tБ.Х КАК Х1,\n\tВ1.Х1 КАК Х11\nИЗ\n\tСправочник.А КАК А,"
                         "\n\tСправочник.Б КАК Б,\n\tСправочник.Г КАК В1"));
}

TEST(Canonical, DuplicateExplicitNamesRefused) {
    auto a = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х, 2 КАК х");
    CHECK(a.ok);
    const PrintResult r = PrintCanonicalChecked(a.batch);
    CHECK(!r.ok);
    CHECK_EQ(r.error.code, std::string("Q_PRINT_DUPLICATE_COLUMN"));
    auto b = ParseBatchStrict("ВЫБРАТЬ 1 КАК Ёж, 2 КАК Еж");
    CHECK(b.ok);
    CHECK(PrintCanonical(b.batch).empty());
}

TEST(Canonical, UnionNamesComeFromTheFirstPart) {
    CHECK_EQ(Canon("ВЫБРАТЬ А.Х ИЗ Справочник.А КАК А ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ Б.Х ИЗ Справочник.Б КАК Б"),
             std::string("ВЫБРАТЬ\n\tА.Х КАК Х\nИЗ\n\tСправочник.А КАК А\nОБЪЕДИНИТЬ ВСЕ\nВЫБРАТЬ\n\tБ.Х\nИЗ\n\tСправочник.Б КАК Б"));
    // A later part keeps an alias only if the client wrote one (it is not checked against the first part).
    CHECK_EQ(Canon("ВЫБРАТЬ 1 ОБЪЕДИНИТЬ ВЫБРАТЬ 2 КАК Y"),
             std::string("ВЫБРАТЬ\n\t1 КАК Поле1\nОБЪЕДИНИТЬ\nВЫБРАТЬ\n\t2 КАК Y"));
    auto a = ParseBatchStrict("ВЫБРАТЬ А.Х, Б.Х ИЗ Справочник.А КАК А, Справочник.Б КАК Б");
    CHECK(a.ok);
    std::vector<std::vector<std::string>> names;
    CHECK(StatementResultNames(a.batch, names));
    CHECK_EQ(names.size(), static_cast<size_t>(1));
    CHECK_EQ(names[0].size(), static_cast<size_t>(2));
    CHECK_EQ(names[0][0], std::string("Х"));
    CHECK_EQ(names[0][1], std::string("Х1"));
}

TEST(Canonical, StatementResultNamesPerStatement) {
    auto a = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х, Т.*, 3 ИЗ Справочник.А КАК Т; УНИЧТОЖИТЬ ВТ");
    CHECK(a.ok);
    std::vector<std::vector<std::string>> names;
    CHECK(StatementResultNames(a.batch, names));
    CHECK_EQ(names.size(), static_cast<size_t>(2));
    CHECK_EQ(names[0].size(), static_cast<size_t>(3));
    CHECK_EQ(names[0][0], std::string("Х"));
    CHECK_EQ(names[0][1], std::string(""));
    CHECK_EQ(names[0][2], std::string("Поле3"));
    CHECK(names[1].empty());
    auto d = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х, 2 КАК х");
    CHECK(!StatementResultNames(d.batch, names));
}

TEST(Canonical, BetweenIsParenthesisedUnderLogic) {
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ А МЕЖДУ 1 И 2 И Б"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ (А МЕЖДУ 1 И 2) И Б"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ Б И А МЕЖДУ 1 И 2"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ Б И (А МЕЖДУ 1 И 2)"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ А МЕЖДУ 1 И 2 ИЛИ А НЕ МЕЖДУ 3 И 4"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ (А МЕЖДУ 1 И 2) ИЛИ (А НЕ МЕЖДУ 3 И 4)"));
    CHECK_EQ(Canon("ВЫБРАТЬ А КАК Х ГДЕ НЕ (А МЕЖДУ 1 И 2)"),
             std::string("ВЫБРАТЬ\n\tА КАК Х\nГДЕ НЕ (А МЕЖДУ 1 И 2)"));
    RoundTrip("ВЫБРАТЬ А КАК Х ГДЕ А МЕЖДУ 1 И 2 И Б И В1 МЕЖДУ 3 И 4");
}

TEST(Canonical, RemainingPredicateAndClauseShapes) {
    RoundTrip("ВЫБРАТЬ КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Т.А) КАК К, КОЛИЧЕСТВО(*) КАК К2 ИЗ Справочник.А КАК Т");
    CHECK_EQ(Canon("ВЫБРАТЬ КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Т.А) ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tКОЛИЧЕСТВО(РАЗЛИЧНЫЕ Т.А) КАК Поле1\nИЗ\n\tСправочник.А КАК Т"));
    RoundTrip("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т ГДЕ Т.А НЕ ССЫЛКА Справочник.Б");
    RoundTrip("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т ГДЕ Т.А НЕ ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"~\"");
    CHECK_EQ(Canon("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т ГДЕ Т.А НЕ ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"~\""),
             std::string("ВЫБРАТЬ\n\tТ.А КАК А\nИЗ\n\tСправочник.А КАК Т\nГДЕ Т.А НЕ ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"~\""));
    // Single-element row is just parentheses; a row needs a subquery on the right.
    CHECK_EQ(Canon("ВЫБРАТЬ (Т.А) КАК Х ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.А КАК Х\nИЗ\n\tСправочник.А КАК Т"));
    CHECK(Canon("ВЫБРАТЬ 1 КАК Х ГДЕ (Т.А, Т.Б) В (1, 2)").rfind("PARSE-ERR", 0) == 0);
    RoundTrip("ВЫБРАТЬ 1 КАК Х ГДЕ (Т.А, Т.Б) НЕ В (ВЫБРАТЬ Б.А, Б.Б ИЗ Справочник.Б КАК Б)");
    // ИТОГИ edge cases.
    RoundTrip("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т ИТОГИ ПО ОБЩИЕ");
    RoundTrip("ВЫБРАТЬ Т.А, Т.Б ИЗ Справочник.А КАК Т ИТОГИ СУММА(Т.Б), МАКСИМУМ(Т.Б) ПО Т.А КАК Гр, ОБЩИЕ");
    RoundTrip("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т ИТОГИ ПО Т.А ТОЛЬКО ИЕРАРХИЯ, Т.А ИЕРАРХИЯ");
    RoundTrip("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т УПОРЯДОЧИТЬ ПО Т.А АВТОУПОРЯДОЧИВАНИЕ ИТОГИ ПО Т.А ПЕРИОДАМИ(ДЕНЬ, &Н, &К)");
    CHECK_EQ(Canon("ВЫБРАТЬ Т.А ИЗ Справочник.А КАК Т ИТОГИ ПО ОБЩИЕ"),
             std::string("ВЫБРАТЬ\n\tТ.А КАК А\nИЗ\n\tСправочник.А КАК Т\nИТОГИ ПО ОБЩИЕ"));
}

TEST(Canonical, KeywordPathSegmentsKeepStoredSpelling) {
    CHECK_EQ(Canon("ВЫБРАТЬ Т.ссылка.Имя КАК Х, Т.Представление КАК П, Т.ПредставлениеСсылки КАК ПС, "
                   "Т.Значение КАК З ИЗ Справочник.А КАК Т"),
             std::string("ВЫБРАТЬ\n\tТ.ссылка.Имя КАК Х,\n\tТ.Представление КАК П,\n\tТ.ПредставлениеСсылки КАК ПС,"
                         "\n\tТ.Значение КАК З\nИЗ\n\tСправочник.А КАК Т"));
}

TEST(Canonical, StringLiteralsReEscaped) {
    CHECK_EQ(Canon("ВЫБРАТЬ \"a\"\"b\" КАК С"), std::string("ВЫБРАТЬ\n\t\"a\"\"b\" КАК С"));
    CHECK_EQ(Canon("ВЫБРАТЬ \"\"\"\" КАК С"), std::string("ВЫБРАТЬ\n\t\"\"\"\" КАК С"));
    CHECK_EQ(Canon("ВЫБРАТЬ \"\" КАК С"), std::string("ВЫБРАТЬ\n\t\"\" КАК С"));
    CHECK_EQ(Canon("ВЫБРАТЬ \"|{ ГДЕ /* // \" КАК С"), std::string("ВЫБРАТЬ\n\t\"|{ ГДЕ /* // \" КАК С"));
}

TEST(Canonical, StatementsJoinedByNewlineSemicolon) {
    CHECK_EQ(Canon("ВЫБРАТЬ 1 КАК Х ПОМЕСТИТЬ ВТ; УНИЧТОЖИТЬ ВТ;"),
             std::string("ВЫБРАТЬ\n\t1 КАК Х\nПОМЕСТИТЬ ВТ;\nУНИЧТОЖИТЬ ВТ"));
}

TEST(Canonical, VirtualTableEmptySlotsPrintedEmpty) {
    CHECK_EQ(Canon("ВЫБРАТЬ О.Х КАК Х ИЗ РегистрСведений.Цены.СрезПоследних(, Номенклатура = &Н) КАК О"),
             std::string("ВЫБРАТЬ\n\tО.Х КАК Х\nИЗ\n\tРегистрСведений.Цены.СрезПоследних(, Номенклатура = &Н) КАК О"));
    CHECK_EQ(Canon("ВЫБРАТЬ О.Х КАК Х ИЗ РегистрСведений.Цены.СрезПоследних() КАК О"),
             std::string("ВЫБРАТЬ\n\tО.Х КАК Х\nИЗ\n\tРегистрСведений.Цены.СрезПоследних() КАК О"));
    RoundTrip("ВЫБРАТЬ О.Х ИЗ РегистрСведений.Цены.СрезПоследних(&Д, , ) КАК О");
}

// ---- fail closed on hand-built trees -------------------------------------------------------------------------

TEST(Canonical, RefusesUnvalidatedTrees) {
    CHECK(Refuses(BatchOf(Node(Expr::K::Unset))));
    CHECK(Refuses(Batch{}));
    Batch b = BatchOf(FieldOf("А"));
    b.statements[0].select->items[0].expr = nullptr;
    CHECK(Refuses(b));
}

TEST(Canonical, KeywordВAliasRoundTrip) {
    RoundTrip("ВЫБРАТЬ В.Выполнено, МИНИМУМ(В.Порядок) ИЗ РегистрСведений.Р КАК В "
              "ЛЕВОЕ СОЕДИНЕНИЕ (ВЫБРАТЬ 1 КАК Х) КАК in ПО in.Х = В.Х "
              "ГДЕ НЕ В.Пометка И В.Ссылка В (&А) СГРУППИРОВАТЬ ПО В.Выполнено");
    RoundTrip("ВЫБРАТЬ Т.А = 1 КАК В, Т.Б КАК in ИЗ Справочник.А КАК Т ОБЪЕДИНИТЬ ВЫБРАТЬ 1 КАК В, 2");
    CHECK_EQ(Canon("ВЫБРАТЬ КОЛИЧЕСТВО(*) ИЗ Справочник.А КАК В ГДЕ В.Х = 1"),
             std::string("ВЫБРАТЬ\n\tКОЛИЧЕСТВО(*) КАК Поле1\nИЗ\n\tСправочник.А КАК В\nГДЕ В.Х = 1"));
    // В as an alias and as the first segment of a longer path; never a bare field or a later segment.
    CHECK(Refuses(BatchOf(FieldOf("В"), "Х")));
    CHECK(PrintCanonicalChecked(BatchOf(FieldOf("А"), "В")).ok);
    ExprP g = FieldOf("А");
    g->parts.push_back("В");
    CHECK(Refuses(BatchOf(g, "Х")));
}

TEST(Canonical, RefusesKeywordsAndHomoglyphsAsNames) {
    CHECK(Refuses(BatchOf(FieldOf("ВЫБРАТЬ"))));                 // keyword as the first path segment
    CHECK(Refuses(BatchOf(FieldOf("select"))));
    CHECK(Refuses(BatchOf(FieldOf("Ssылка"))));                  // Latin + Cyrillic mix
    CHECK(Refuses(BatchOf(FieldOf("А"), "КАК")));                // keyword alias
    CHECK(Refuses(BatchOf(FieldOf("А"), "Ssылка")));
    ExprP f = FieldOf("А");
    f->parts.push_back("ИЗ");                                    // keyword segment that is not allowlisted
    CHECK(Refuses(BatchOf(f)));
    ExprP g = FieldOf("А");
    g->parts.push_back("Ссылка");                                // allowlisted keyword segment
    CHECK(PrintCanonicalChecked(BatchOf(g, "Х")).ok);
    ExprP prm = Node(Expr::K::Param, "ВЫБРАТЬ");
    CHECK(PrintCanonicalChecked(BatchOf(prm, "Х")).ok);          // &ВЫБРАТЬ is one Parameter token: fine
    CHECK(Refuses(BatchOf(Node(Expr::K::Param, "Sп"))));
    ExprP call = Node(Expr::K::Call, "ВЫБРАТЬ");
    CHECK(Refuses(BatchOf(call, "Х")));
    ExprP fn = Node(Expr::K::Call, "ПРЕДСТАВЛЕНИЕ");             // keyword call needs its fixed arity
    CHECK(Refuses(BatchOf(fn, "Х")));
    fn->args.push_back(FieldOf("А"));
    CHECK(PrintCanonicalChecked(BatchOf(fn, "Х")).ok);
}

TEST(Canonical, RefusesBadStringsAndNumbers) {
    CHECK(Refuses(BatchOf(Node(Expr::K::StringLit, std::string("a\0b", 3)), "Х")));
    CHECK(Refuses(BatchOf(Node(Expr::K::StringLit, "\xFF\xFE"), "Х")));
    CHECK(Refuses(BatchOf(Node(Expr::K::StringLit, "\xC0\xAF"), "Х")));
    CHECK(Refuses(BatchOf(Node(Expr::K::StringLit, "\xED\xA0\x80"), "Х")));
    CHECK(Refuses(BatchOf(Node(Expr::K::NumberLit, "1abc"), "Х")));
    CHECK(Refuses(BatchOf(Node(Expr::K::NumberLit, "1."), "Х")));
    CHECK(PrintCanonicalChecked(BatchOf(Node(Expr::K::StringLit, "\"\n\xD0\x96"), "Х")).ok);
}

TEST(Canonical, RefusesShapesThatDoNotRoundTrip) {
    // RefsTest needs a qualified type; ValidateBatch alone does not require it.
    ExprP refs = Node(Expr::K::RefsTest);
    refs->args.push_back(FieldOf("А"));
    refs->type_name = "Справочник";
    CHECK(Refuses(BatchOf(refs, "Х")));
    refs->type_name = "Справочник.А";
    CHECK(PrintCanonicalChecked(BatchOf(refs, "Х")).ok);
    // FOR UPDATE / INDEX BY before a union cannot be parsed back.
    auto a = ParseBatchStrict("ВЫБРАТЬ 1 КАК Х ОБЪЕДИНИТЬ ВЫБРАТЬ 2");
    CHECK(a.ok);
    a.batch.statements[0].select->for_update = true;
    CHECK(Refuses(a.batch));
}

TEST(Canonical, RefusesRunawayDepth) {
    // A unary chain far beyond the parser's own nesting limit is refused, never printed.
    ExprP e = FieldOf("А");
    for (int i = 0; i < 3000; ++i) {
        ExprP n = Node(Expr::K::Unary, "-");
        n->args.push_back(e);
        e = n;
    }
    CHECK(Refuses(BatchOf(e, "Х")));   // ValidateBatch (depth > kMaxExprDepth) or the printer limit
}

// ---- parser-differential corpus: nothing hostile survives to the executed text -------------------------------

TEST(Canonical, HostileInputsNeverPrint) {
    const std::vector<std::string> hostile = {
        "ВЫБРАТЬ 1 КАК Х\n|ИЗ Справочник.А",           // continuation line
        "ВЫБРАТЬ 1 {ГДЕ 1 = 1}",                         // brace section
        "ВЫБРАТЬ 1 /* c */ КАК Х",                       // block comment
        "ВЫБРАТЬ \"незакрытая КАК Х",                    // unterminated string
        "ВЫБРАТЬ А\xC2\xA0КАК Х",                        // NBSP
        "ВЫБРАТЬ А\xE2\x80\x8B КАК Х",                   // zero-width space
        "ВЫБРАТЬ А\xE2\x80\x8BБ КАК Х",                  // zero-width inside an identifier
        "ВЫБРАТЬ Ssылка КАК Х",                          // homoglyph: Latin+Cyrillic identifier
        "ВЫБРАТЬ 1 КАК Х; ; ВЫБРАТЬ 2",                  // empty statement
        "ВЫБРАТЬ 1 КАК Х ИЗ Справочник.А КАК Т ИЗ Справочник.Б",
        "ВЫБРАТЬ А КАК Х ГДЕ А = 1 = 2",                 // predicate chain
        "ВЫБРАТЬ 1 КАК Х ПОМЕСТИТЬ ВТ ОБЪЕДИНИТЬ ВЫБРАТЬ 2 ПОМЕСТИТЬ ВТ2",
        "ВЫБРАТЬ 1 КАК Х\x01",                           // control byte
        "",
    };
    for (const std::string& q : hostile) {
        auto r = ParseBatchStrict(q);
        CHECK(!r.ok);
        CHECK(PrintCanonical(r.batch).empty());
    }
}

TEST(Canonical, PrintedTextHasNoPipeBraceOrBlockComment) {
    const std::vector<std::string> queries = {
        "ВЫБРАТЬ \"a|b\" КАК С, \"{x}\" КАК Д, \"/*\" КАК Е ИЗ Справочник.А КАК Т ГДЕ Т.А = \"//\"",
        "ВЫБРАТЬ 1 / 2 КАК Х, 3 * -4 КАК Y",
    };
    for (const std::string& q : queries) {
        auto a = ParseBatchStrict(q);
        CHECK(a.ok);
        const std::string text = PrintCanonical(a.batch);
        CHECK(!text.empty());
        // Outside string literals nothing but the canonical layout is present.
        std::string outside;
        bool in_string = false;
        for (const char c : text) {
            if (c == '"') in_string = !in_string;
            else if (!in_string) outside += c;
        }
        CHECK(outside.find('|') == std::string::npos);
        CHECK(outside.find('{') == std::string::npos);
        CHECK(outside.find('}') == std::string::npos);
        CHECK(outside.find("/*") == std::string::npos);
        CHECK(outside.find("//") == std::string::npos);
    }
}

TEST(Canonical, GeneratedExpressionsRoundTrip) {
    // Deterministic pseudo-random expression trees over every operator, printed and re-parsed.
    unsigned long long seed = 0x9E3779B97F4A7C15ULL;
    auto next = [&seed]() {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned>(seed >> 33);
    };
    const char* const leaves[] = {"А", "Т.Б", "1", "2.5", "\"s\"\"t\"", "&П", "NULL", "ИСТИНА", "Т.Ссылка"};
    const char* const bins[] = {"+", "-", "*", "/", "=", "<>", "<", ">", "<=", ">=", "И", "ИЛИ"};
    int printed = 0;
    for (int round = 0; round < 300; ++round) {
        // Build in source form with explicit parentheses everywhere: the tree shape is then fully determined.
        std::vector<std::string> pool;
        for (const char* l : leaves) pool.push_back(l);
        for (int step = 0; step < 6; ++step) {
            const std::string a = pool[next() % pool.size()];
            const std::string b = pool[next() % pool.size()];
            switch (next() % 9) {
                case 0: pool.push_back("(" + a + ") " + bins[next() % 12] + " (" + b + ")"); break;
                case 1: pool.push_back("НЕ (" + a + ")"); break;
                case 2: pool.push_back("-(" + a + ")"); break;
                case 3: pool.push_back("(" + a + ") В (" + b + ", 3)"); break;
                case 4: pool.push_back("(" + a + ") НЕ МЕЖДУ (" + b + ") И (" + a + ")"); break;
                case 5: pool.push_back("(" + a + ") ЕСТЬ НЕ NULL"); break;
                case 6: pool.push_back("ВЫБОР КОГДА " + a + " ТОГДА " + b + " ИНАЧЕ 0 КОНЕЦ"); break;
                case 7: pool.push_back("СУММА(" + a + ", " + b + ")"); break;
                default: pool.push_back("(" + a + ") ПОДОБНО (" + b + ")"); break;
            }
        }
        const std::string q = "ВЫБРАТЬ " + pool.back() + " КАК Х ИЗ Справочник.А КАК Т";
        auto a = ParseBatchStrict(q);
        if (!a.ok) continue;   // generator may produce chains the grammar rejects (a = b = c): skipped, never printed
        const PrintResult p = PrintCanonicalChecked(a.batch);
        CHECK(p.ok);
        auto b = ParseBatchStrict(p.text);
        CHECK(b.ok);
        CHECK_EQ(DumpBatch(b.batch), DumpBatch(a.batch, true));
        CHECK_EQ(PrintCanonical(b.batch), p.text);
        ++printed;
    }
    CHECK(printed >= 100);   // the generator must not degenerate into skipped rounds
}
