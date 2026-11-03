#include "test_framework.h"
#include "analysis_helpers.h"
#include "policy_fixture.h"

#include <string>

using namespace lineage::strict;

namespace {

Origin FirstOrigin(const AnalysisResult& r) {
    if (r.columns.empty()) {
        CHECK_EQ(r.reasons.empty() ? std::string("no columns") : r.reasons[0].code, std::string("columns"));
    }
    return r.columns.at(0).origin;
}

}  // namespace

TEST(Origin, ResolvesAliasImplicitAliasAndTempTable) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Имя ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ ВТ.Имя ИЗ ВТ КАК ВТ");
    CHECK_EQ(r.columns.at(0).origin, Origin::ProtectedDirect);
}

TEST(Origin, AmbiguousUnqualifiedFieldIsUnknown) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Код КАК К ИЗ Справочник.А КАК А, Справочник.Б КАК Б");   // both have Код (bare Ссылка is a keyword)
    CHECK_EQ(r.columns.at(0).origin, Origin::Unknown);
    CHECK(HasIssue(r, kResolveAmbiguousName));
}

TEST(Origin, DereferenceFollowsRefs) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.СсылкаНаА.Имя КАК Н ИЗ Справочник.Б КАК Б");
    CHECK_EQ(r.columns.at(0).origin, Origin::ProtectedDirect);
}

TEST(Origin, MissingTableReported) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Вх.Х ИЗ Справочник.Нет КАК Вх");
    CHECK(r.missing_tables.count("Справочник.Нет") == 1);
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
}

TEST(Origin, ImplicitAliasOnlyWhenUnique) {
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А")), Origin::ProtectedDirect);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Справочник.А.Имя КАК Н ИЗ Справочник.А")),
             Origin::ProtectedDirect);
    // the same last segment from two sources: the implicit alias is not unique
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А, Документ.А");
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
    CHECK(HasIssue(r, kResolveAmbiguousName));
    // an explicit alias hides the table name
    auto hidden = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А КАК Х");
    CHECK_EQ(FirstOrigin(hidden), Origin::Unknown);
    CHECK(HasIssue(hidden, kResolveUnknownName));
}

TEST(Origin, UnqualifiedFieldInSingleSourceAndUnknownNames) {
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Имя КАК Н ИЗ Справочник.А КАК А")), Origin::ProtectedDirect);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Сумма КАК С ИЗ Справочник.А КАК А, Справочник.Б КАК Б")),
             Origin::Open);
    auto unknown_alias = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Ц.Имя КАК Н ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(unknown_alias), Origin::Unknown);
    CHECK(HasIssue(unknown_alias, kResolveUnknownName));
    // a field the policy does not list is Unknown, never open
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Нет КАК Н ИЗ Справочник.А КАК А")), Origin::Unknown);
}

TEST(Origin, DuplicateAliasIsAmbiguous) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Х.Имя КАК Н ИЗ Справочник.А КАК Х, Справочник.Б КАК Х");
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
    CHECK(HasIssue(r, kResolveAmbiguousName));
}

TEST(Origin, TempTableLifecycle) {
    // used before it is created
    auto early = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ВТ.Имя ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(early), Origin::Unknown);
    CHECK(HasIssue(early, kResolveUnknownTemp));
    // destroyed, then used
    auto dropped = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Имя ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; УНИЧТОЖИТЬ ВТ; ВЫБРАТЬ ВТ.Имя ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(dropped), Origin::Unknown);
    CHECK(HasIssue(dropped, kResolveUnknownTemp));
    // destroying a table that does not exist
    CHECK(HasIssue(AnalyzeForTest(kPolicyAB, "УНИЧТОЖИТЬ ВТ; ВЫБРАТЬ 1 КАК Х"), kResolveUnknownTemp));
    // re-creation replaces the columns; the old protected column is gone
    auto replaced = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; УНИЧТОЖИТЬ ВТ; "
        "ВЫБРАТЬ Б.Сумма КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; ВЫБРАТЬ ВТ.Х ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(replaced), Origin::Open);
    // the select that creates a table reads the old one of the same name
    auto self = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ ВТ.Х КАК Х ПОМЕСТИТЬ ВТ ИЗ ВТ КАК ВТ; "
        "ВЫБРАТЬ ВТ.Х ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(self), Origin::ProtectedDirect);
    // lookup of the temp-table name is case-insensitive
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Имя ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ вт.имя ИЗ вт КАК вт")), Origin::ProtectedDirect);
}

TEST(Origin, TempTableKeepsRefsForDereference) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК К ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; ВЫБРАТЬ ВТ.К.Имя КАК Н ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(r), Origin::ProtectedDirect);
}

TEST(Origin, TempTableWithWildcardIsOpaque) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ * ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ ВТ.Имя ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
}

TEST(Origin, SubqueryAsSourceAndCorrelatedSubquery) {
    auto derived = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Т.Х КАК Х ИЗ (ВЫБРАТЬ А.Имя КАК Х ИЗ Справочник.А КАК А) КАК Т");
    CHECK_EQ(FirstOrigin(derived), Origin::ProtectedDirect);
    // a derived table does not see the outer scope
    auto isolated = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Т.Х КАК Х ИЗ Справочник.А КАК А, (ВЫБРАТЬ А.Имя КАК Х ИЗ Справочник.Б КАК Б) КАК Т");
    CHECK(HasIssue(isolated, kResolveUnknownName));
    // a subquery in a condition sees the outer alias
    auto corr = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А КАК А ГДЕ А.Ссылка В (ВЫБРАТЬ Б.СсылкаНаА ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма > А.Код)");
    // ... but only when no inner policy table could own the name (the policy might not list every field)
    CHECK(HasIssue(corr, kResolveAmbiguousName));
    // an alias visible at both levels is ambiguous (fail closed)
    auto shadow = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А КАК Т ГДЕ Т.Ссылка В (ВЫБРАТЬ Т.СсылкаНаА ИЗ Справочник.Б КАК Т)");
    CHECK(HasIssue(shadow, kResolveAmbiguousName));
    // missing table inside a nested query is still reported
    auto nested = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А КАК А ГДЕ А.Ссылка В (ВЫБРАТЬ Вх.Х ИЗ Справочник.Нет КАК Вх)");
    CHECK(nested.missing_tables.count("Справочник.Нет") == 1);
}

TEST(Origin, UnionJoinsColumnsPositionally) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(r), Origin::Mixed);
    auto mismatch = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВЫБРАТЬ А.Имя, А.Код ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(mismatch), Origin::Unknown);
    CHECK(HasIssue(mismatch, kResolveUnionArity));
}

TEST(Origin, CompositeRefsJoinAllTargets) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Владелец":{"class":"open","refs":["Справочник.А","Справочник.Б"]}}},
        "Справочник.А":{"fields":{"Имя":{"class":"protected"}}},
        "Справочник.Б":{"fields":{"Имя":{"class":"open"}}},
        "Справочник.Ц":{"fields":{"Имя":{"class":"open"}}}}})");
    CHECK(p.ok);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ Д.Владелец.Имя КАК Н ИЗ Документ.Д КАК Д")), Origin::Mixed);
    // a target without the field makes the result Unknown
    auto q = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Владелец":{"class":"open","refs":["Справочник.А","Справочник.Ц"]}}},
        "Справочник.А":{"fields":{"Имя":{"class":"protected"}}},
        "Справочник.Ц":{"fields":{"Другое":{"class":"open"}}}}})");
    CHECK(q.ok);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(q.policy, "ВЫБРАТЬ Д.Владелец.Имя КАК Н ИЗ Документ.Д КАК Д")), Origin::Unknown);
    // a missing target table is reported
    auto m = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Владелец":{"class":"open","refs":["Справочник.Нет"]}}}}})");
    CHECK(m.ok);
    auto r = AnalyzeForTest(m.policy, "ВЫБРАТЬ Д.Владелец.Имя КАК Н ИЗ Документ.Д КАК Д");
    CHECK(r.missing_tables.count("Справочник.Нет") == 1);
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
}

TEST(Origin, PresentationSegmentUsesTablePresentationClass) {
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.СсылкаНаА.Представление КАК П ИЗ Справочник.Б КАК Б")),
             Origin::ProtectedDirect);
}

TEST(Origin, YoAndYeNeverMergeSilently) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Справочник.Е":{"fields":{
        "Ёж":{"class":"protected"},"Ель":{"class":"open"}}}}})");
    CHECK(p.ok);
    // exact spelling resolves
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ Е.Ёж КАК Х ИЗ Справочник.Е КАК Е")), Origin::ProtectedDirect);
    // the other spelling is not the policy's field: Unknown, never open
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ Е.Еж КАК Х ИЗ Справочник.Е КАК Е")), Origin::Unknown);
    // result names that differ only by ё/е collide: the column list is not trusted
    auto t = AnalyzeForTest(p.policy,
        "ВЫБРАТЬ Е.Ёж КАК Ёж, Е.Ель КАК Еж ПОМЕСТИТЬ ВТ ИЗ Справочник.Е КАК Е; ВЫБРАТЬ ВТ.Еж ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(t), Origin::Unknown);
    CHECK(HasIssue(t, kResolveColumnMismatch));
    // a temp-table name spelled with the other letter is a different name: unknown, never a silent match
    auto d = AnalyzeForTest(p.policy,
        "ВЫБРАТЬ Е.Ёж КАК Х ПОМЕСТИТЬ Ёлка ИЗ Справочник.Е КАК Е; ВЫБРАТЬ Елка.Х ИЗ Елка КАК Елка");
    CHECK_EQ(FirstOrigin(d), Origin::Unknown);
    CHECK(HasIssue(d, kResolveUnknownTemp));
    // two temp tables that differ only by ё/е make either spelling unresolvable
    auto c = AnalyzeForTest(p.policy,
        "ВЫБРАТЬ Е.Ёж КАК Х ПОМЕСТИТЬ Ёлка ИЗ Справочник.Е КАК Е; ВЫБРАТЬ Е.Ель КАК Х ПОМЕСТИТЬ Елка ИЗ Справочник.Е КАК Е; "
        "ВЫБРАТЬ Елка.Х ИЗ Елка КАК Елка");
    CHECK_EQ(FirstOrigin(c), Origin::Unknown);
}

TEST(Origin, OrderByMayUseResultAlias) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО Н");
    CHECK(r.resolution_issues.empty());
    auto bad = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Имя КАК Н ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО Нет");
    CHECK(HasIssue(bad, kResolveUnknownName));
}

TEST(Origin, ParseErrorBecomesReason) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ИЗ");
    CHECK(!r.reasons.empty());
    CHECK(r.columns.empty());
}

TEST(Origin, ProtectedReferenceTaintsDereference) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Секрет":{"class":"protected","refs":["Справочник.А"]},
                                "Темный":{"class":"unknown","refs":["Справочник.А"]},
                                "Откр":{"class":"open","refs":["Справочник.А"]}}},
        "Справочник.А":{"fields":{"Код":{"class":"open"}}}}})");
    CHECK(p.ok);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ Д.Секрет.Код КАК К ИЗ Документ.Д КАК Д")),
             Origin::ProtectedDerived);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ Д.Темный.Код КАК К ИЗ Документ.Д КАК Д")), Origin::Unknown);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ Д.Откр.Код КАК К ИЗ Документ.Д КАК Д")), Origin::Open);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy,
        "ВЫБРАТЬ Д.Секрет КАК С ПОМЕСТИТЬ ВТ ИЗ Документ.Д КАК Д; ВЫБРАТЬ ВТ.С.Код КАК К ИЗ ВТ КАК ВТ")),
        Origin::ProtectedDerived);
}

TEST(Origin, UnionKeepsRefsOnlyWhenBothSidesHaveThem) {
    // the other branch is not a reference: a dereference of the merged column is a guess
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК К ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ Б.Сумма ИЗ Справочник.Б КАК Б; "
        "ВЫБРАТЬ ВТ.К.Код КАК Х ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
    auto c = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК К ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ "
        "ВЫБРАТЬ ВЫБОР КОГДА А.Имя = \"x\" ТОГДА А.Ссылка КОНЕЦ ИЗ Справочник.А КАК А; "
        "ВЫБРАТЬ ВТ.К.Код КАК Х ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(c), Origin::Unknown);
}

TEST(Origin, UnknownResolutionAlwaysLeavesAnIssue) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Справочник.Е":{"fields":{
        "Ёж":{"class":"protected"}}}}})");
    CHECK(p.ok);
    auto t = AnalyzeForTest(p.policy,
        "ВЫБРАТЬ Е.Ёж КАК Ёж ПОМЕСТИТЬ ВТ ИЗ Справочник.Е КАК Е; ВЫБРАТЬ ВТ.Еж КАК Х ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(t), Origin::Unknown);
    CHECK(!t.resolution_issues.empty());
    auto m = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Имя ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ ВТ.Нет КАК Х ИЗ ВТ КАК ВТ");
    CHECK_EQ(FirstOrigin(m), Origin::Unknown);
    CHECK(!m.resolution_issues.empty());
    auto unlisted = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.СсылкаНаА.ПредставлениеСсылки КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(unlisted), Origin::Unknown);
    CHECK(HasIssue(unlisted, kResolveUnknownField));
    auto value = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.СсылкаНаА.Значение КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(value), Origin::Unknown);
    CHECK(!value.resolution_issues.empty());
}

TEST(Origin, LatticeJoin) {
    CHECK(JoinOrigin(Origin::Open, Origin::Open) == Origin::Open);
    CHECK(JoinOrigin(Origin::ProtectedDirect, Origin::ProtectedDirect) == Origin::ProtectedDirect);
    CHECK(JoinOrigin(Origin::ProtectedDirect, Origin::Open) == Origin::Mixed);
    CHECK(JoinOrigin(Origin::ProtectedDirect, Origin::Client) == Origin::Mixed);
    CHECK(JoinOrigin(Origin::Client, Origin::Open) == Origin::Client);    // both public to the client
    CHECK(JoinOrigin(Origin::Unknown, Origin::Open) == Origin::Unknown);
    CHECK(JoinOrigin(Origin::Mixed, Origin::Unknown) == Origin::Unknown);
    CHECK(Transform(Origin::ProtectedDirect) == Origin::ProtectedDerived);
    CHECK(Transform(Origin::Open) == Origin::Open);
}

TEST(Origin, LiteralParamAndAggregateLabels) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ \"x\" КАК Л, &П КАК Пр, КОЛИЧЕСТВО(А.Имя) КАК К, А.Ссылка КАК С ИЗ Справочник.А КАК А");
    CHECK_EQ(r.columns.size(), static_cast<size_t>(4));
    CHECK_EQ(r.columns.at(0).origin, Origin::Client);
    CHECK_EQ(r.columns.at(1).origin, Origin::Client);     // &П is client in kPolicyAB
    CHECK_EQ(r.columns.at(2).origin, Origin::Open);       // count of protected = open number
    CHECK_EQ(r.columns.at(3).origin, Origin::Open);       // GUID ref
}

TEST(Origin, ParametersByPolicyOrigin) {
    auto token = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ &МСПТокен1 КАК Т ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(token), Origin::ProtectedDirect);
    auto missing = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ &Нет КАК Т ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(missing), Origin::Unknown);
    CHECK(HasIssue(missing, kResolveUnknownParam));
}

TEST(Origin, OtherLiteralKindsAndTypeFunctions) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ 1 КАК Ч, ИСТИНА КАК Б, NULL КАК Н, НЕОПРЕДЕЛЕНО КАК Н2, ЗНАЧЕНИЕ(Справочник.А.Пустая) КАК З, "
        "ТИПЗНАЧЕНИЯ(А.Ссылка) КАК Т, ТИП(Справочник.А) КАК Т2 ИЗ Справочник.А КАК А");
    CHECK_EQ(r.columns.size(), static_cast<size_t>(7));
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(r.columns.at(i).origin, Origin::Client);
    for (size_t i = 4; i < 7; ++i) CHECK_EQ(r.columns.at(i).origin, Origin::Open);
}

TEST(Origin, SumAndMinMaxAreDerivedFromProtectedInput) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ СУММА(А.Код) КАК С, МАКСИМУМ(А.Имя) КАК М, МИНИМУМ(Б.Сумма) КАК М2, СУММА(Б.Сумма) КАК С2, "
        "КОЛИЧЕСТВО(*) КАК К, КОЛИЧЕСТВО(РАЗЛИЧНЫЕ А.Имя) КАК К2 ИЗ Справочник.А КАК А, Справочник.Б КАК Б");
    CHECK_EQ(r.columns.size(), static_cast<size_t>(6));
    CHECK_EQ(r.columns.at(0).origin, Origin::ProtectedDerived);
    CHECK_EQ(r.columns.at(1).origin, Origin::ProtectedDerived);
    CHECK_EQ(r.columns.at(2).origin, Origin::Open);
    CHECK_EQ(r.columns.at(3).origin, Origin::Open);
    CHECK_EQ(r.columns.at(4).origin, Origin::Open);
    CHECK_EQ(r.columns.at(5).origin, Origin::Open);
}

TEST(Origin, FunctionsCombineOperandsAndUnknownFunctionsFailClosed) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПОДСТРОКА(А.Имя, 1, 2) КАК П, ЕСТЬNULL(Б.Сумма, 0) КАК Е, ГОД(&П) КАК Г, "
        "ВЫРАЗИТЬ(А.Имя КАК СТРОКА(10)) КАК Вр, -Б.Сумма КАК Н, А.Имя + \"x\" КАК Ф, СтранныйВызов(Б.Сумма) КАК Х "
        "ИЗ Справочник.А КАК А, Справочник.Б КАК Б");
    CHECK_EQ(r.columns.size(), static_cast<size_t>(7));
    CHECK_EQ(r.columns.at(0).origin, Origin::Mixed);       // protected + literal operands
    CHECK_EQ(r.columns.at(1).origin, Origin::Client);      // open + literal
    CHECK_EQ(r.columns.at(2).origin, Origin::Client);
    CHECK_EQ(r.columns.at(3).origin, Origin::ProtectedDerived);
    CHECK_EQ(r.columns.at(4).origin, Origin::Open);
    CHECK_EQ(r.columns.at(5).origin, Origin::Mixed);
    CHECK_EQ(r.columns.at(6).origin, Origin::Unknown);     // function outside the allowlist
}

TEST(Origin, PredicatesDeriveFromTheirOperands) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя = \"x\" КАК Р, А.Имя В (\"a\", \"b\") КАК В1, А.Имя ПОДОБНО \"a%\" КАК П, "
        "А.Код МЕЖДУ 1 И 2 КАК М, А.Имя ЕСТЬ NULL КАК Н, Б.Сумма > 1 КАК С "
        "ИЗ Справочник.А КАК А, Справочник.Б КАК Б");
    CHECK_EQ(r.columns.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < 4; ++i) CHECK_EQ(r.columns.at(i).origin, Origin::Mixed);
    CHECK_EQ(r.columns.at(4).origin, Origin::ProtectedDerived);
    CHECK_EQ(r.columns.at(5).origin, Origin::Client);
}

TEST(Origin, InSubqueryJoinsSubqueryColumns) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма В (ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(r), Origin::Mixed);
    auto open = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма В (ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(open), Origin::Open);
    auto wild = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма В (ВЫБРАТЬ * ИЗ Справочник.Б КАК Б2) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(wild), Origin::Unknown);
}

TEST(Origin, CaseCombinesBranchesAndConditions) {
    // implicit flow: constant branches, but the condition reads a protected value
    auto flow = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР КОГДА А.Имя = \"x\" ТОГДА 1 ИНАЧЕ 0 КОНЕЦ КАК Х ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(flow), Origin::Mixed);
    auto simple = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР А.Имя КОГДА \"x\" ТОГДА 1 ИНАЧЕ 0 КОНЕЦ КАК Х ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(simple), Origin::Mixed);
    auto branch = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР КОГДА Б.Сумма > 1 ТОГДА А.Имя ИНАЧЕ А.Имя КОНЕЦ КАК Х ИЗ Справочник.А КАК А, Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(branch), Origin::Mixed);   // client-visible condition joined with a protected branch
    auto only = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР КОГДА А.Имя ЕСТЬ NULL ТОГДА А.Код ИНАЧЕ А.Имя КОНЕЦ КАК Х ИЗ Справочник.А КАК А");
    CHECK_EQ(FirstOrigin(only), Origin::ProtectedDerived);
    auto plain = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР КОГДА Б.Сумма > 1 ТОГДА 1 ИНАЧЕ 0 КОНЕЦ КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(plain), Origin::Client);
    auto unknown = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР КОГДА Б.Нет > 1 ТОГДА 1 КОНЕЦ КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(unknown), Origin::Unknown);
}

TEST(Origin, ReferencePresentationBelongsToTheMainSpace) {
    auto fn = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(Б.СсылкаНаА) КАК П, ПРЕДСТАВЛЕНИЕССЫЛКИ(Б.СсылкаНаА) КАК П2 ИЗ Справочник.Б КАК Б");
    CHECK_EQ(fn.columns.at(0).origin, Origin::ProtectedDirect);
    CHECK_EQ(fn.columns.at(1).origin, Origin::ProtectedDirect);
    // presentation of an open table
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Вид":{"class":"open","refs":["Справочник.О"]}}},
        "Справочник.О":{"presentation":"open","fields":{"Код":{"class":"open"}}}}})");
    CHECK(p.ok);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(Д.Вид) КАК П ИЗ Документ.Д КАК Д")),
             Origin::Open);
    // a protected scalar operand: a plain function over protected data
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(А.Имя) КАК П ИЗ Справочник.А КАК А")),
             Origin::ProtectedDerived);
    // an operand whose type cannot be proven (a parameter may hold any reference): fail closed
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(&П) КАК П ИЗ Справочник.А КАК А")),
             Origin::Unknown);
    // a target that the policy does not list
    auto q = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Вид":{"class":"open","refs":["Справочник.Нет"]}}}}})");
    CHECK(q.ok);
    auto miss = AnalyzeForTest(q.policy, "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(Д.Вид) КАК П ИЗ Документ.Д КАК Д");
    CHECK_EQ(FirstOrigin(miss), Origin::Unknown);
    CHECK(miss.missing_tables.count("Справочник.Нет") == 1);
}

TEST(Origin, CallClassification) {
    CHECK(ClassifyCall("КОЛИЧЕСТВО") == CallClass::Count);
    CHECK(ClassifyCall("count") == CallClass::Count);
    CHECK(ClassifyCall("СУММА") == CallClass::Sum);
    CHECK(ClassifyCall("МАКСИМУМ") == CallClass::MinMax);
    CHECK(ClassifyCall("ПРЕДСТАВЛЕНИЕ") == CallClass::Presentation);
    CHECK(ClassifyCall("ТИПЗНАЧЕНИЯ") == CallClass::TypeOf);
    CHECK(ClassifyCall("ПОДСТРОКА") == CallClass::Plain);
    CHECK(ClassifyCall("Подстрока") == CallClass::Plain);
    CHECK(ClassifyCall("СтранныйВызов") == CallClass::Unrecognized);
}

TEST(Origin, CastOfReferenceToNonReferenceTypeFailsClosed) {
    auto str = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ВЫРАЗИТЬ(Б.СсылкаНаА КАК СТРОКА(50)) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(str), Origin::Unknown);
    auto param = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ВЫРАЗИТЬ(&П КАК СТРОКА(50)) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(param), Origin::Unknown);
    auto to_ref = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ВЫРАЗИТЬ(Б.СсылкаНаА КАК Справочник.А) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(to_ref), Origin::Open);
    auto scalar = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ВЫРАЗИТЬ(Б.Сумма КАК ЧИСЛО(15, 2)) КАК Х ИЗ Справочник.Б КАК Б");
    CHECK_EQ(FirstOrigin(scalar), Origin::Open);
}

TEST(Origin, TypeOfDependsOnOperandOrigin) {
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ТИПЗНАЧЕНИЯ(А.Имя) КАК Х ИЗ Справочник.А КАК А")),
             Origin::ProtectedDerived);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ТИПЗНАЧЕНИЯ(Б.Сумма) КАК Х ИЗ Справочник.Б КАК Б")),
             Origin::Open);
}

TEST(Origin, PeriodArgumentIsAConstant) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Документ.Д":{"fields":{"Дата":{"class":"open"}}}}})");
    CHECK(p.ok);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ НАЧАЛОПЕРИОДА(Д.Дата, ДЕНЬ) КАК Х ИЗ Документ.Д КАК Д")),
             Origin::Open);
    CHECK_EQ(FirstOrigin(AnalyzeForTest(p.policy, "ВЫБРАТЬ РАЗНОСТЬДАТ(Д.Дата, Д.Дата, МЕСЯЦ) КАК Х ИЗ Документ.Д КАК Д")),
             Origin::Open);
    // any other identifier there is a field lookup
    auto r = AnalyzeForTest(p.policy, "ВЫБРАТЬ НАЧАЛОПЕРИОДА(Д.Дата, Нечто) КАК Х ИЗ Документ.Д КАК Д");
    CHECK_EQ(FirstOrigin(r), Origin::Unknown);
    CHECK(!r.resolution_issues.empty());
}

TEST(Origin, PresentationOverNonFieldOperandFailsClosed) {
    CHECK_EQ(FirstOrigin(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(ЕСТЬNULL(Б.СсылкаНаА, НЕОПРЕДЕЛЕНО)) КАК Х ИЗ Справочник.Б КАК Б")), Origin::Unknown);
}

TEST(Origin, FunctionsOverProtectedNeedConfirmation) {
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ПОДСТРОКА(А.Имя, 1, 1) КАК Х ИЗ Справочник.А КАК А");
    CHECK(HasReason(r, "Q_FUNC_OVER_PROTECTED"));
    CHECK(r.verdict == Verdict::Confirm);
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Имя + \"x\" КАК Х ИЗ Справочник.А КАК А"), "Q_FUNC_OVER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫБОР КОГДА А.Имя = &МСПТокен1 ТОГДА 1 ИНАЧЕ 0 КОНЕЦ КАК Х ИЗ Справочник.А КАК А"), "Q_FUNC_OVER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ВЫРАЗИТЬ(А.Имя КАК СТРОКА(10)) КАК Х ИЗ Справочник.А КАК А"), "Q_FUNC_OVER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ЕСТЬNULL(А.Имя, \"\") КАК Х ИЗ Справочник.А КАК А"), "Q_FUNC_OVER_PROTECTED"));
    // an equality in the select list is a computed bit
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя = &МСПТокен1 КАК Х ИЗ Справочник.А КАК А"), "Q_FUNC_OVER_PROTECTED"));
}

TEST(Origin, EqualityRules) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = \"x\""), "Q_EQ_LITERAL"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &П"), "Q_EQ_CLIENT_PARAM"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А, Справочник.Б КАК Б ГДЕ А.Имя = Б.Код"), "Q_EQ_CLIENT_PARAM"));
    auto ok = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1");
    CHECK(ok.verdict == Verdict::Safe);
    CHECK(ok.reasons.empty());
    // reversed operands and IN
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ &МСПТокен1 = А.Имя").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (&МСПТокен1)").verdict == Verdict::Safe);
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (\"a\", &МСПТокен1)"), "Q_EQ_LITERAL"));
    // protected = protected field, and IN (subquery of a protected column)
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А, Справочник.А КАК А2 ГДЕ А.Имя = А2.Имя").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (ВЫБРАТЬ А2.Имя ИЗ Справочник.А КАК А2)").verdict ==
        Verdict::Safe);
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (ВЫБРАТЬ Б.Код ИЗ Справочник.Б КАК Б)"),
        "Q_EQ_CLIENT_PARAM"));
    // a token parameter the host marked unknown
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Справочник.А":{"fields":{
        "Имя":{"class":"protected"},"Ссылка":{"class":"open"}}}},"params":{"Т":"token_unknown"}})");
    CHECK(p.ok);
    CHECK(HasReason(AnalyzeForTest(p.policy,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &Т"), "Q_EQ_UNKNOWN_TOKEN"));
}

TEST(Origin, CompareLikeBetweenOnProtected) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя > \"a\""), "Q_COMPARE_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя <> &МСПТокен1"), "Q_COMPARE_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя МЕЖДУ \"a\" И \"b\""), "Q_COMPARE_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя ПОДОБНО \"a%\""), "Q_LIKE_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя ЕСТЬ NULL"), "Q_FUNC_OVER_PROTECTED"));
    // open values are free
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма > 1 И Б.Код ПОДОБНО \"a%\"").verdict == Verdict::Safe);
}

TEST(Origin, OrderingTopMinMaxTotals) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО Нм"), "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО А.Имя"), "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм ИЗ Справочник.А КАК А АВТОУПОРЯДОЧИВАНИЕ"), "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК К ИЗ Справочник.Б КАК Б АВТОУПОРЯДОЧИВАНИЕ"), "Q_ORDER_PROTECTED"));
    auto top = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ПЕРВЫЕ 5 А.Имя КАК Нм ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО Нм");
    CHECK(HasReason(top, "Q_TOP_WITH_ORDER"));
    // ordering by an open column
    auto open = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ПЕРВЫЕ 5 Б.Сумма КАК С ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО С");
    CHECK(open.verdict == Verdict::Safe);
    // a union column that is protected in the other part
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО Х"),
        "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ МАКСИМУМ(А.Имя) КАК М ИЗ Справочник.А КАК А"), "Q_MINMAX_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ИТОГИ СУММА(С) ПО ОБЩИЕ"), "Q_TOTALS"));
}

TEST(Origin, PresentationPlacement) {
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(Б.СсылкаНаА) КАК П ИЗ Справочник.Б КАК Б").verdict == Verdict::Safe);
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ ПРЕДСТАВЛЕНИЕ(Б.СсылкаНаА) = \"x\""), "Q_PRESENTATION"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б СГРУППИРОВАТЬ ПО Б.Сумма, ПРЕДСТАВЛЕНИЕ(Б.СсылкаНаА)"),
        "Q_PRESENTATION"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПРЕДСТАВЛЕНИЕ(А.Имя) КАК П ИЗ Справочник.А КАК А"), "Q_FUNC_OVER_PROTECTED"));
}

TEST(Origin, ColumnAndStructureReasons) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А"),
        "Q_MIXED_COLUMN"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.Нет КАК Х ИЗ Справочник.Б КАК Б"), "Q_UNKNOWN_COLUMN"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ * ИЗ Справочник.Б КАК Б"), "Q_WILDCARD"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.* ИЗ Справочник.Б КАК Б"), "Q_WILDCARD"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ СтранныйВызов(Б.Сумма) КАК Х ИЗ Справочник.Б КАК Б"), "Q_UNKNOWN_FUNCTION"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ДЛЯ ИЗМЕНЕНИЯ"), "Q_FOR_UPDATE"));
}

TEST(Origin, UnknownAnywhereAndResolutionIssuesConfirm) {
    // Unknown in a condition only: no output column is Unknown
    auto cond = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Нет > 1");
    CHECK(cond.verdict == Verdict::Confirm);
    CHECK(HasReason(cond, "Q_UNKNOWN_EXPR"));
    CHECK(HasReason(cond, "Q_RESOLVE_UNKNOWN_FIELD"));
    // Unknown inside a subquery that is never output
    auto sub = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Ссылка В (ВЫБРАТЬ Б2.Нет ИЗ Справочник.Б КАК Б2)");
    CHECK(sub.verdict == Verdict::Confirm);
    // an unlisted parameter, a missing table
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма = &Нет").verdict == Verdict::Confirm);
    auto miss = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Вх.Х КАК Х ИЗ Справочник.Нет КАК Вх");
    CHECK(HasReason(miss, "Q_INCOMPLETE"));
    CHECK(miss.verdict == Verdict::Confirm);
    // a parse failure never yields Safe
    CHECK(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ИЗ").verdict == Verdict::Confirm);
}

TEST(Origin, LegitimateShapesAreSafe) {
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК К, СУММА(Б.Сумма) КАК С ИЗ Справочник.Б КАК Б СГРУППИРОВАТЬ ПО Б.СсылкаНаА").verdict ==
        Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм, КОЛИЧЕСТВО(*) КАК Н ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Б.Код) КАК Н ИЗ Справочник.Б КАК Б").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм, Б.Сумма КАК С ИЗ Справочник.А КАК А ВНУТРЕННЕЕ СОЕДИНЕНИЕ Справочник.Б КАК Б "
        "ПО Б.СсылкаНаА = А.Ссылка").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА ЕСТЬ NULL").verdict == Verdict::Safe);
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Документ.Д":{"fields":{"Дата":{"class":"open"}}}}})");
    CHECK(p.ok);
    CHECK(AnalyzeForTest(p.policy,
        "ВЫБРАТЬ НАЧАЛОПЕРИОДА(Д.Дата, МЕСЯЦ) КАК М ИЗ Документ.Д КАК Д").verdict == Verdict::Safe);
}

TEST(Origin, KeywordAliasColumnsResolveThroughTempTables) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК Ссылка, Б.Сумма КАК Значение ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б;"
        "ВЫБРАТЬ ВТ.Ссылка КАК Ссылка, ВТ.Значение КАК Сумма, ВТ.Ссылка.Имя КАК Имя ИЗ ВТ КАК ВТ");
    CHECK(r.verdict == Verdict::Safe);
    CHECK_EQ(r.columns.size(), 3u);
    CHECK_EQ(r.columns[0].name, std::string("Ссылка"));
    CHECK_EQ(r.columns[0].origin, Origin::Open);
    CHECK_EQ(r.columns[0].refs.size(), 1u);
    CHECK_EQ(r.columns[1].origin, Origin::Open);
    CHECK_EQ(r.columns[2].origin, Origin::ProtectedDirect);   // the dereference still reads the policy
}

TEST(Origin, OrderByOrdinalResolvesToTheSelectedColumn) {
    const char* open_cols = "ВЫБРАТЬ Б.Сумма КАК С, А.Имя КАК Нм ИЗ Справочник.Б КАК Б, Справочник.А КАК А ";
    CHECK(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО 1").verdict ==
          Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB, std::string(open_cols) + "УПОРЯДОЧИТЬ ПО 1 УБЫВ").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПЕРВЫЕ 3 Б.Сумма КАК С, Б.Код КАК Кд ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО 2, 1").verdict ==
        Verdict::Safe);
    // The ordinal of a protected column is ordering by it.
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, std::string(open_cols) + "УПОРЯДОЧИТЬ ПО 2"), "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПЕРВЫЕ 1 А.Имя КАК Нм ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО 1"), "Q_TOP_WITH_ORDER"));
    // A union column carries the join of every part.
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО 1"),
        "Q_ORDER_PROTECTED"));
    // Not a plain in-range ordinal: out of range, zero, leading zero, fraction, other literal, a wildcard before it.
    for (const char* tail : {"УПОРЯДОЧИТЬ ПО 3", "УПОРЯДОЧИТЬ ПО 0", "УПОРЯДОЧИТЬ ПО 01", "УПОРЯДОЧИТЬ ПО 1.0",
                             "УПОРЯДОЧИТЬ ПО \"1\"", "УПОРЯДОЧИТЬ ПО ИСТИНА", "УПОРЯДОЧИТЬ ПО NULL",
                             "УПОРЯДОЧИТЬ ПО 99999999999999999999"}) {
        CHECK(HasReason(AnalyzeForTest(kPolicyAB, std::string(open_cols) + tail), "Q_ORDER_PROTECTED"));
    }
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.*, Б.Сумма КАК С ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО 1"),
                    "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.Нет КАК С ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО 1"),
                    "Q_ORDER_PROTECTED"));
    // Grouping and totals by an ordinal stay refused.
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б СГРУППИРОВАТЬ ПО 1"), "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ИТОГИ ПО 1"), "Q_ORDER_PROTECTED"));
}

// The documentation does not say that a plain ordering by a reference avoids its presentation (only that
// АВТОУПОРЯДОЧИВАНИЕ substitutes the default sort fields), so every ordering by a reference needs every target's
// presentation to be open. Справочник.А and Справочник.Б have protected presentations under kPolicyAB.
TEST(Origin, OrderingByReferenceNeedsOpenPresentation) {
    const char* q = "ВЫБРАТЬ Б.Сумма КАК С, Б.СсылкаНаА КАК К ИЗ Справочник.Б КАК Б ";
    for (const char* tail : {"УПОРЯДОЧИТЬ ПО Б.СсылкаНаА", "УПОРЯДОЧИТЬ ПО К", "УПОРЯДОЧИТЬ ПО 2", "УПОРЯДОЧИТЬ ПО С, К УБЫВ"}) {
        CHECK(HasReason(AnalyzeForTest(kPolicyAB, std::string(q) + tail), "Q_ORDER_PROTECTED"));
    }
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, std::string(q) + "ИТОГИ ПО К"), "Q_TOTALS"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ПЕРВЫЕ 1 Б.Ссылка КАК К ИЗ Справочник.Б КАК Б УПОРЯДОЧИТЬ ПО К"),
                    "Q_TOP_WITH_ORDER"));
    CHECK(AnalyzeForTest(kPolicyAB, std::string(q) + "УПОРЯДОЧИТЬ ПО С").verdict == Verdict::Safe);
    // A target with an open presentation may be ordered by.
    auto open = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Справочник.О": {"presentation": "open", "fields": {"Ссылка": {"class": "open", "refs": ["Справочник.О"]}}}}})");
    CHECK(open.ok);
    for (const char* tail : {"УПОРЯДОЧИТЬ ПО О.Ссылка", "УПОРЯДОЧИТЬ ПО К", "УПОРЯДОЧИТЬ ПО 1", "ИТОГИ ПО К"}) {
        CHECK(AnalyzeForTest(open.policy, std::string("ВЫБРАТЬ О.Ссылка КАК К ИЗ Справочник.О КАК О ") + tail).verdict ==
              Verdict::Safe);
    }
}

// An expression column may hold a reference whose targets were lost: it is not "provably not a reference".
TEST(Origin, LostReferenceTargetsAreNotOrderable) {
    auto open = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Справочник.О": {"presentation": "open", "fields": {"Ссылка": {"class": "open", "refs": ["Справочник.О"]},
                                                          "Ч": {"class": "open"}}}}})");
    CHECK(open.ok);
    auto known = [&](const std::string& item) {
        const AnalysisResult r = AnalyzeForTest(open.policy, "ВЫБРАТЬ " + item + " КАК К ИЗ Справочник.О КАК О");
        CHECK_EQ(r.columns.size(), 1u);
        return r.columns[0].refs_known;
    };
    for (const char* item : {"О.Ч", "О.Ссылка", "1", "СУММА(О.Ч)", "КОЛИЧЕСТВО(*)", "ЕСТЬNULL(О.Ч, 0)", "МАКСИМУМ(О.Ч)",
                             "ВЫБОР КОГДА О.Ч > 1 ТОГДА О.Ч ИНАЧЕ 0 КОНЕЦ", "ПОДСТРОКА(\"а\", 1, 1)", "О.Ч = 1",
                             "ВЫРАЗИТЬ(О.Ч КАК Число(10))", "ТИП(Справочник.О)"}) {
        if (!known(item)) CHECK_EQ(std::string(item), std::string("refs known"));
    }
    for (const char* item : {"&П", "ЗНАЧЕНИЕ(Справочник.О.Пустая)", "ВЫРАЗИТЬ(О.Ч КАК Справочник.О)",
                             "ЕСТЬNULL(О.Ссылка, 0)", "МАКСИМУМ(О.Ссылка)", "ВЫБОР КОГДА О.Ч > 1 ТОГДА О.Ссылка КОНЕЦ"}) {
        if (known(item)) CHECK_EQ(std::string(item), std::string("refs unknown"));
    }
    // A cast of a possible reference to a string would read its presentation: Unknown.
    CHECK(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ ВЫРАЗИТЬ(ЕСТЬNULL(Б.СсылкаНаА, 0) КАК Строка(10)) КАК К ИЗ Справочник.Б КАК Б")
              .columns.at(0).origin == Origin::Unknown);
    // A union of a reference and a number has lost its targets; one of two references keeps them.
    CHECK(!AnalyzeForTest(open.policy, "ВЫБРАТЬ О.Ссылка КАК К ИЗ Справочник.О КАК О ОБЪЕДИНИТЬ ВЫБРАТЬ 1")
               .columns.at(0).refs_known);
    CHECK(AnalyzeForTest(open.policy, "ВЫБРАТЬ О.Ссылка КАК К ИЗ Справочник.О КАК О ОБЪЕДИНИТЬ ВЫБРАТЬ О2.Ссылка "
                                      "ИЗ Справочник.О КАК О2 УПОРЯДОЧИТЬ ПО К").verdict == Verdict::Safe);
    CHECK(HasReason(AnalyzeForTest(open.policy, "ВЫБРАТЬ ЗНАЧЕНИЕ(Справочник.О.Пустая) КАК К УПОРЯДОЧИТЬ ПО 1"),
                    "Q_ORDER_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(open.policy, "ВЫБРАТЬ О.Ссылка КАК К ИЗ Справочник.О КАК О "
                                                "ОБЪЕДИНИТЬ ВЫБРАТЬ 1 АВТОУПОРЯДОЧИВАНИЕ"), "Q_ORDER_PROTECTED"));
}

TEST(Origin, TotalsOverOpenColumnsByReturnedColumns) {
    const std::string q = "ВЫБРАТЬ Б.СсылкаНаА КАК К, Б.Сумма КАК С, Б.Код КАК Кд ИЗ Справочник.Б КАК Б ";
    // (К is a reference to Справочник.А, whose presentation is protected: grouping totals by it is refused below.)
    for (const char* tail : {"ИТОГИ ПО Кд", "УПОРЯДОЧИТЬ ПО Кд ИТОГИ СУММА(С) ПО Кд", "ИТОГИ КОЛИЧЕСТВО(С), МАКСИМУМ(С) ПО Кд, С",
                             "ИТОГИ КОЛИЧЕСТВО(*), КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Кд) ПО кд КАК Группа"}) {
        auto r = AnalyzeForTest(kPolicyAB, q + tail);
        if (r.verdict != Verdict::Safe) CHECK_EQ(std::string(tail) + ": " + r.reasons.at(0).code, std::string("safe"));
    }
    // Refused: ОБЩИЕ, a hierarchy, periods, a literal or an expression that is not a returned column, an aggregate
    // that is not СУММА / СРЕДНЕЕ / КОЛИЧЕСТВО / МИНИМУМ / МАКСИМУМ over open non-reference data, ПЕРВЫЕ.
    for (const char* tail : {"ИТОГИ СУММА(С) ПО ОБЩИЕ", "ИТОГИ ПО К ИЕРАРХИЯ", "ИТОГИ ПО К ТОЛЬКО ИЕРАРХИЯ",
                             "ИТОГИ ПО Кд ПЕРИОДАМИ(ДЕНЬ, &П, &П)", "ИТОГИ ПО 1", "ИТОГИ ПО Б.СсылкаНаА",
                             "ИТОГИ ПО К.Ссылка", "ИТОГИ ПО Сумма", "ИТОГИ ПОДСТРОКА(Кд, 1, 1) ПО К",
                             "ИТОГИ МАКСИМУМ(К) ПО Кд", "ИТОГИ С ПО К", "ИТОГИ СУММА(С) + 1 ПО К"}) {
        CHECK(HasReason(AnalyzeForTest(kPolicyAB, q + tail), "Q_TOTALS"));
    }
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ПЕРВЫЕ 2 Б.СсылкаНаА КАК К ИЗ Справочник.Б КАК Б ИТОГИ ПО К"), "Q_TOTALS"));
    // Protected anywhere: a returned column, a grouping column, the row set.
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С, А.Имя КАК Нм ИЗ Справочник.А КАК А ИТОГИ ПО С"), "Q_TOTALS"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С, А.Имя КАК Нм ИЗ Справочник.А КАК А ИТОГИ ПО Нм"), "Q_TOTALS"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, q + "ГДЕ Б.СсылкаНаА.Имя = &П ИТОГИ ПО К"), "Q_TOTALS"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, q + "ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ А.Ссылка, 1, А.Имя ИЗ Справочник.А КАК А "
                                              "ИТОГИ ПО К"), "Q_TOTALS"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.Нет КАК Н, Б.Сумма КАК С ИЗ Справочник.Б КАК Б ИТОГИ ПО С"),
                    "Q_TOTALS"));
}

TEST(Origin, OrdinalOrderingAndNonLastResultSets) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б СГРУППИРОВАТЬ ПО 1"), "Q_ORDER_PROTECTED"));
    // a non-last statement returning rows is checked like the last one
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ИЗ Справочник.А КАК А ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ \"q\" ; ВЫБРАТЬ 1 КАК Y");
    CHECK(r.verdict == Verdict::Confirm);
    CHECK(HasReason(r, "Q_MIXED_COLUMN"));
    // ... but a ПОМЕСТИТЬ statement is not a result set
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; ВЫБРАТЬ ВТ.Х КАК Y ИЗ ВТ КАК ВТ").verdict == Verdict::Safe);
}

// ---- clause-specific rules and implicit-condition channels ----

TEST(Origin, ConditionChannels) {
    // join ON comparing a protected field with a literal
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ВНУТРЕННЕЕ СОЕДИНЕНИЕ Справочник.А КАК А "
        "ПО Б.СсылкаНаА = А.Ссылка И А.Имя = \"x\""), "Q_EQ_LITERAL"));
    // HAVING with a function over a protected value
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя "
        "ИМЕЮЩИЕ МАКСИМУМ(ПОДСТРОКА(А.Имя, 1, 1)) = \"x\""), "Q_FUNC_OVER_PROTECTED"));
    // a virtual table condition over a dereferenced protected field
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ РегистрНакопления.Взаиморасчеты.Остатки(, Контрагент.Имя ПОДОБНО \"А%\") КАК О"),
        "Q_LIKE_PROTECTED"));
    // ВЫБОР in a condition produces predicate bits
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А "
        "ГДЕ ВЫБОР КОГДА А.Имя = &МСПТокен1 ТОГДА 1 ИНАЧЕ 0 КОНЕЦ = 1"), "Q_FUNC_OVER_PROTECTED"));
    // IN list of literals
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (\"a\", \"b\")"), "Q_EQ_LITERAL"));
}

TEST(Origin, SubqueryRowSetTaintsItsValue) {
    // an open column of a subquery whose rows were chosen by a protected condition is not open
    auto lit = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В "
        "(ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 ГДЕ Б2.СсылкаНаА.Имя = \"x\")");
    CHECK(HasReason(lit, "Q_EQ_LITERAL"));
    auto tok = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В "
        "(ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 ГДЕ Б2.СсылкаНаА.Имя = &МСПТокен1)");
    CHECK(tok.verdict == Verdict::Confirm);
    CHECK(HasReason(tok, "Q_COMPARE_PROTECTED"));
    // join and grouping conditions count too
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В (ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 "
        "ВНУТРЕННЕЕ СОЕДИНЕНИЕ Справочник.А КАК А ПО Б2.СсылкаНаА = А.Ссылка И А.Имя = &МСПТокен1)").verdict ==
        Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В "
        "(ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 СГРУППИРОВАТЬ ПО Б2.Сумма, Б2.СсылкаНаА.Имя)").verdict ==
        Verdict::Confirm);
    // the same subquery with an open condition stays safe
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В "
        "(ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 ГДЕ Б2.Сумма > 1)").verdict == Verdict::Safe);
    // a value subquery inside a select list
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма В (ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 ГДЕ Б2.СсылкаНаА.Имя = &МСПТокен1) КАК Х "
        "ИЗ Справочник.Б КАК Б").verdict == Verdict::Confirm);
}

TEST(Origin, AggregatesOverProtectedRowSetsNeedConfirmation) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК Н ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1"), "Q_AGGREGATE_ROWSET"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ СУММА(Б.Сумма) КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя = &МСПТокен1"), "Q_AGGREGATE_ROWSET"));
    // a protected join condition
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ СУММА(Б.Сумма) КАК С ИЗ Справочник.Б КАК Б ВНУТРЕННЕЕ СОЕДИНЕНИЕ Справочник.А КАК А "
        "ПО Б.СсылкаНаА = А.Ссылка И А.Имя = &МСПТокен1"), "Q_AGGREGATE_ROWSET"));
    // a protected grouping that is not returned as is
    auto grp = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК Н ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя");
    CHECK(HasReason(grp, "Q_GROUP_PROTECTED"));
    CHECK(HasReason(grp, "Q_AGGREGATE_ROWSET"));
    // a HAVING aggregate of such a row set
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК К ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя = &МСПТокен1 "
        "СГРУППИРОВАТЬ ПО Б.СсылкаНаА ИМЕЮЩИЕ КОЛИЧЕСТВО(*) > 1"), "Q_AGGREGATE_ROWSET"));
    // a protected condition in an inner query reaches the outer aggregate through a derived table / temp table
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК Н ИЗ (ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б "
        "ГДЕ Б.СсылкаНаА.Имя = &МСПТокен1) КАК Х"), "Q_AGGREGATE_ROWSET"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя = &МСПТокен1; "
        "ВЫБРАТЬ СУММА(ВТ.С) КАК С ИЗ ВТ КАК ВТ"), "Q_AGGREGATE_ROWSET"));
    // an aggregate inside a subquery with a protected condition
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В "
        "(ВЫБРАТЬ СУММА(Б2.Сумма) ИЗ Справочник.Б КАК Б2 ГДЕ Б2.СсылкаНаА.Имя = &МСПТокен1)"), "Q_AGGREGATE_ROWSET"));
    // a protected condition in one part of a union
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя = &МСПТокен1 "
        "ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ СУММА(Б3.Сумма) ИЗ Справочник.Б КАК Б3 ГДЕ Б3.СсылкаНаА.Имя = &МСПТокен1"),
        "Q_AGGREGATE_ROWSET"));
    // controls: aggregates over open row sets, or grouped by the returned protected key, stay safe
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК Н, СУММА(Б.Сумма) КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма > 1").verdict ==
        Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Нм, КОЛИЧЕСТВО(*) КАК Н ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя").verdict ==
        Verdict::Safe);
    // a protected condition without an aggregate is the documented token equality
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя = &МСПТокен1").verdict == Verdict::Safe);
}

TEST(Origin, VirtualTableParametersAreLabelledAgainstTheBaseTable) {
    const std::string vt = "РегистрНакопления.Взаиморасчеты.Остатки";
    // client period, open condition, periodicity word
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(&П, Контрагент = &П) КАК О").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(НАЧАЛОПЕРИОДА(&П, МЕСЯЦ), ) КАК О").verdict == Verdict::Safe);
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, КВАРТАЛ, Контрагент = &П) КАК О").verdict == Verdict::Safe);
    // the token equality on a dereferenced protected field is allowed; the row set is then protected
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Контрагент.Имя = &МСПТокен1) КАК О").verdict == Verdict::Safe);
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ СУММА(О.СуммаОстаток) КАК С ИЗ " + vt + "(, Контрагент.Имя = &МСПТокен1) КАК О"),
        "Q_AGGREGATE_ROWSET"));
    // literal / client-parameter equality, ordering, function over protected
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Контрагент.Имя = \"x\") КАК О"), "Q_EQ_LITERAL"));
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Комментарий = &П) КАК О"), "Q_EQ_CLIENT_PARAM"));
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Контрагент.Имя > \"a\") КАК О"), "Q_COMPARE_PROTECTED"));
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, ПОДСТРОКА(Комментарий, 1, 1) = &П) КАК О"),
        "Q_FUNC_OVER_PROTECTED"));
    // a subquery inside a parameter is walked with its own rules
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Контрагент В (ВЫБРАТЬ А.Ссылка ИЗ Справочник.А КАК А "
        "ГДЕ А.Имя = \"x\")) КАК О"), "Q_EQ_LITERAL"));
    // a protected value that is not a condition
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(&МСПТокен1) КАК О"), "Q_VT_PARAM"));
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Комментарий) КАК О"), "Q_VT_PARAM"));
    // fail closed: unknown field or parameter, a field of another source, an unknown virtual table
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Нет = &П) КАК О").verdict == Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Контрагент = &Нет) КАК О").verdict == Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ Справочник.Б КАК Б ВНУТРЕННЕЕ СОЕДИНЕНИЕ " + vt +
        "(, Контрагент = Б.СсылкаНаА) КАК О ПО ИСТИНА").verdict == Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.Х КАК С ИЗ РегистрНакопления.Нет.Остатки(, Х = &П) КАК О").verdict == Verdict::Confirm);
}

TEST(Origin, UnlistedFieldsFailClosed) {
    // the policy must list every field of every table: a name it lacks never resolves to anything open
    auto r = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Скрытое = &П");
    CHECK(r.verdict == Verdict::Confirm);
    CHECK(HasIssue(r, kResolveUnknownField));
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В (ВЫБРАТЬ А.Скрытое ИЗ Справочник.А КАК А)").verdict ==
        Verdict::Confirm);
}

TEST(Origin, GroupingAndDistinctHandMultiplicitiesToConsumers) {
    const char* kGrouped = "ВЫБРАТЬ А.Имя КАК Н, КОЛИЧЕСТВО(*) КАК К ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя";
    // the top-level result keeps the exemption
    CHECK(AnalyzeForTest(kPolicyAB, kGrouped).verdict == Verdict::Safe);
    // consumed through a derived table, a temp table, a union part, a subquery
    CHECK(AnalyzeForTest(kPolicyAB, std::string("ВЫБРАТЬ Х.К КАК К ИЗ (") + kGrouped + ") КАК Х").verdict ==
          Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyAB, std::string(kGrouped).insert(
          std::string(kGrouped).find(" ИЗ "), " ПОМЕСТИТЬ ВТ") + "; ВЫБРАТЬ ВТ.К КАК К ИЗ ВТ КАК ВТ").verdict ==
          Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Х.Н КАК Н ИЗ (" + std::string(kGrouped) + ") КАК Х СГРУППИРОВАТЬ ПО Х.Н ИМЕЮЩИЕ Х.К > 1")
        .verdict == Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК К ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ КОЛИЧЕСТВО(*) ИЗ Справочник.А КАК А "
        "СГРУППИРОВАТЬ ПО А.Имя").verdict == Verdict::Confirm);
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В (ВЫБРАТЬ КОЛИЧЕСТВО(*) ИЗ Справочник.А КАК А "
        "СГРУППИРОВАТЬ ПО А.Имя)").verdict == Verdict::Confirm);
    // РАЗЛИЧНЫЕ over a protected column
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ РАЗЛИЧНЫЕ А.Имя КАК Н ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК К ИЗ ВТ КАК ВТ"),
        "Q_AGGREGATE_ROWSET"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(РАЗЛИЧНЫЕ А.Имя) КАК К ИЗ Справочник.А КАК А"), "Q_AGGREGATE_ROWSET"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ КОЛИЧЕСТВО(А.Имя) КАК К ИЗ Справочник.А КАК А"), "Q_AGGREGATE_ROWSET"));
    // top-level РАЗЛИЧНЫЕ over an open column is untouched
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ РАЗЛИЧНЫЕ Б.Сумма КАК С ИЗ Справочник.Б КАК Б").verdict == Verdict::Safe);
}

TEST(Origin, VirtualTableEarlierSlotsMayNotBeProtected) {
    const std::string vt = "РегистрНакопления.Взаиморасчеты.Остатки";
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(Контрагент.Имя = &МСПТокен1, Контрагент = &П) КАК О"),
        "Q_VT_PARAM"));
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(, Контрагент.Имя = &МСПТокен1, ) КАК О"), "Q_VT_PARAM"));
}

// ---- propagation through temp tables, UNION and subqueries ----

TEST(Origin, UnionOfProtectedAndClientIsMixed) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ИЗ Справочник.А КАК А ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ \"догадка\"");
    CHECK_EQ(r.columns.at(0).origin, Origin::Mixed);
    CHECK(HasReason(r, "Q_MIXED_COLUMN"));
}

TEST(Origin, UnionArityMismatchIsRejected) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ Б2.Сумма, Б2.Код ИЗ Справочник.Б КАК Б2");
    CHECK(HasReason(r, "Q_PARSE_UNION_ARITY"));
    CHECK(r.verdict == Verdict::Confirm);
}

TEST(Origin, TempTableLaunderingKeepsTaint) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ГОД(А.Имя) КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ ВТ.Х КАК Х ИЗ ВТ КАК ВТ");
    CHECK(HasReason(r, "Q_FUNC_OVER_PROTECTED"));               // flagged where it happens
    CHECK_EQ(r.columns.at(0).origin, Origin::ProtectedDerived); // and stays derived
}

TEST(Origin, ClientColumnThroughTempTableIsStillClient) {
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ \"x\" КАК Г ПОМЕСТИТЬ ВТ; ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А, ВТ КАК ВТ ГДЕ А.Имя = ВТ.Г");
    CHECK(HasReason(r, "Q_EQ_CLIENT_PARAM"));
}

TEST(Origin, InSubqueryOperandOrigin) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (ВЫБРАТЬ \"x\")"), "Q_EQ_CLIENT_PARAM"));
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя В (ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А)")
        .verdict == Verdict::Safe);
}

TEST(Origin, UnionOrderTotalsChecked) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ИЗ Справочник.А КАК А ОБЪЕДИНИТЬ ВЫБРАТЬ А2.Имя ИЗ Справочник.А КАК А2 УПОРЯДОЧИТЬ ПО Х"),
        "Q_ORDER_PROTECTED"));
}

TEST(Origin, TempTableChainsKeepTaintAndRows) {
    // three links: the derived value stays derived
    auto r = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ ГОД(А.Имя) КАК Х ПОМЕСТИТЬ ВТ1 ИЗ Справочник.А КАК А; "
        "ВЫБРАТЬ ВТ1.Х КАК Х ПОМЕСТИТЬ ВТ2 ИЗ ВТ1 КАК ВТ1; "
        "ВЫБРАТЬ ВТ2.Х КАК Х ПОМЕСТИТЬ ВТ3 ИЗ ВТ2 КАК ВТ2; "
        "ВЫБРАТЬ ВТ3.Х КАК Х ИЗ ВТ3 КАК ВТ3");
    CHECK_EQ(r.columns.at(0).origin, Origin::ProtectedDerived);
    CHECK(HasReason(r, "Q_FUNC_OVER_PROTECTED"));
    // protected rows (open column, protected filter) stay protected through three links
    auto c = AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ1 ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1; "
        "ВЫБРАТЬ ВТ1.Р КАК Р ПОМЕСТИТЬ ВТ2 ИЗ ВТ1 КАК ВТ1; "
        "ВЫБРАТЬ ВТ2.Р КАК Р ПОМЕСТИТЬ ВТ3 ИЗ ВТ2 КАК ВТ2; "
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК К ИЗ ВТ3 КАК ВТ3");
    CHECK(HasReason(c, "Q_AGGREGATE_ROWSET"));
}

TEST(Origin, DropAndRecreateUsesTheNewTable) {
    // protected first, clean after re-creation: the old taint must not stick
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1; "
        "УНИЧТОЖИТЬ ВТ; "
        "ВЫБРАТЬ Б.Сумма КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; "
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК К ИЗ ВТ КАК ВТ").verdict == Verdict::Safe);
    // clean first, protected after re-creation without a drop: the new taint must be seen
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; "
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1; "
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК К ИЗ ВТ КАК ВТ"), "Q_AGGREGATE_ROWSET"));
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; УНИЧТОЖИТЬ ВТ; "
        "ВЫБРАТЬ ПОДСТРОКА(А.Имя,1,1) КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; ВЫБРАТЬ ВТ.Х КАК Х ИЗ ВТ КАК ВТ"),
        "Q_FUNC_OVER_PROTECTED"));
    // a dropped table is gone
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; УНИЧТОЖИТЬ ВТ; ВЫБРАТЬ ВТ.Х КАК Х ИЗ ВТ КАК ВТ"),
        "Q_RESOLVE_UNKNOWN_TEMP_TABLE"));
}

TEST(Origin, TempTableReadInLaterSubquery) {
    // protected column of a temp table is the operand of IN against an open column
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; "
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В (ВЫБРАТЬ ВТ.Х ИЗ ВТ КАК ВТ)"),
        "Q_EQ_CLIENT_PARAM"));
    // a temp table whose rows were chosen by a protected filter taints the subquery result
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1; "
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА В (ВЫБРАТЬ ВТ.Р ИЗ ВТ КАК ВТ)"),
        "Q_COMPARE_PROTECTED"));
    // an aggregate over such a temp table inside a subquery
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1; "
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Сумма В (ВЫБРАТЬ КОЛИЧЕСТВО(*) ИЗ ВТ КАК ВТ)"),
        "Q_AGGREGATE_ROWSET"));
}

TEST(Origin, TempTableReadInVirtualTableParameter) {
    const std::string vt = "РегистрНакопления.Взаиморасчеты.Остатки";
    // a protected-row temp table inside the condition slot taints the virtual table's row set
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А ГДЕ А.Имя = &МСПТокен1; "
        "ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК К ИЗ " + vt + "(, Контрагент В (ВЫБРАТЬ ВТ.Р ИЗ ВТ КАК ВТ)) КАК О"),
        "Q_AGGREGATE_ROWSET"));
    // a protected column of a temp table in an EARLIER slot
    CHECK(HasReason(AnalyzeForTest(kPolicyVT,
        "ВЫБРАТЬ А.Имя КАК Х ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А; "
        "ВЫБРАТЬ О.СуммаОстаток КАК С ИЗ " + vt + "(Контрагент В (ВЫБРАТЬ ВТ.Х ИЗ ВТ КАК ВТ), ) КАК О"), "Q_VT_PARAM"));
}

TEST(Origin, UnionOfTempTables) {
    CHECK_EQ(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ПОМЕСТИТЬ ВТ1 ИЗ Справочник.А КАК А; ВЫБРАТЬ \"q\" КАК Х ПОМЕСТИТЬ ВТ2; "
        "ВЫБРАТЬ ВТ1.Х КАК Х ИЗ ВТ1 КАК ВТ1 ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ ВТ2.Х ИЗ ВТ2 КАК ВТ2").columns.at(0).origin,
        Origin::Mixed);
    // two protected parts stay protected and safe
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Имя КАК Х ПОМЕСТИТЬ ВТ1 ИЗ Справочник.А КАК А; ВЫБРАТЬ А.Код КАК Х ПОМЕСТИТЬ ВТ2 ИЗ Справочник.А КАК А; "
        "ВЫБРАТЬ ВТ1.Х КАК Х ИЗ ВТ1 КАК ВТ1 ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ ВТ2.Х ИЗ ВТ2 КАК ВТ2").verdict == Verdict::Safe);
    // a union part with a protected filter taints the whole result for a later count
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.Сумма КАК К ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ "
        "ВЫБРАТЬ Б2.Сумма ИЗ Справочник.Б КАК Б2 ВНУТРЕННЕЕ СОЕДИНЕНИЕ Справочник.А КАК А ПО Б2.СсылкаНаА = А.Ссылка "
        "ГДЕ А.Имя = &МСПТокен1; ВЫБРАТЬ КОЛИЧЕСТВО(*) КАК К ИЗ ВТ КАК ВТ"), "Q_AGGREGATE_ROWSET"));
}

TEST(Origin, TempTableReferencesSurviveOnlyWhenSafe) {
    // references carried through a temp table: the protected target field is a direct read
    CHECK(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; ВЫБРАТЬ ВТ.Р.Имя КАК Н ИЗ ВТ КАК ВТ")
        .verdict == Verdict::Safe);
    // a function over it is still caught
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б; "
        "ВЫБРАТЬ ПОДСТРОКА(ВТ.Р.Имя,1,1) КАК Н ИЗ ВТ КАК ВТ"), "Q_FUNC_OVER_PROTECTED"));
    // a union of a reference and a client parameter loses the targets: the dereference does not resolve
    CHECK(!AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Б.СсылкаНаА КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.Б КАК Б ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ &П; "
        "ВЫБРАТЬ ВТ.Р.Имя КАК Н ИЗ ВТ КАК ВТ").reasons.empty());
    // a grouped temp table hands its multiplicities on and drops the references
    CHECK(!AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ А.Ссылка КАК Р ПОМЕСТИТЬ ВТ ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя, А.Ссылка; "
        "ВЫБРАТЬ ВТ.Р.Имя КАК Н ИЗ ВТ КАК ВТ").reasons.empty());
}

TEST(Origin, DerivedTableInsideTempChain) {
    CHECK(HasReason(AnalyzeForTest(kPolicyAB,
        "ВЫБРАТЬ Х.К КАК К ПОМЕСТИТЬ ВТ ИЗ (ВЫБРАТЬ ПОДСТРОКА(А.Имя,1,1) КАК К ИЗ Справочник.А КАК А) КАК Х; "
        "ВЫБРАТЬ ВТ.К КАК К ИЗ ВТ КАК ВТ"), "Q_FUNC_OVER_PROTECTED"));
}

TEST(Origin, ConstantsTableIsNotATempTable) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Константы":{"fields":{"Порог":{"class":"open"}}}}})");
    CHECK(p.ok);
    auto known = AnalyzeForTest(p.policy, "ВЫБРАТЬ Константы.Порог КАК П ИЗ Константы КАК Константы");
    CHECK_EQ(FirstOrigin(known), Origin::Open);
    CHECK(!HasIssue(known, kResolveUnknownTemp));
    auto missing = AnalyzeForTest(kPolicyAB, "ВЫБРАТЬ К.Порог КАК П ИЗ Constants КАК К");
    CHECK_EQ(FirstOrigin(missing), Origin::Unknown);
    CHECK(!HasIssue(missing, kResolveUnknownTemp));
    CHECK(HasIssue(missing, kResolveUnknownSource));
    CHECK(missing.missing_tables.size() == 1 && missing.missing_tables.count("Константы") == 1);
    // any other name without a dot is still a temporary table
    CHECK(HasIssue(AnalyzeForTest(p.policy, "ВЫБРАТЬ К.Порог КАК П ИЗ Константа КАК К"), kResolveUnknownTemp));
}
