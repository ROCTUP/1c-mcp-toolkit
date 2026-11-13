#include "test_framework.h"
#include "schema_enricher.h"  // AnalyzeSourcesImpl

using lineage::AnalyzeSourcesImpl;

namespace {
std::string Schema(std::initializer_list<const char*> cols) {
    std::string s = "{\"columns\":[";
    bool first = true;
    for (auto c : cols) { s += std::string(first ? "" : ",") + "{\"name\":\"" + c + "\"}"; first = false; }
    return s + "]}";
}
struct Golden { const char* query; std::initializer_list<const char*> cols; const char* expected; };
const Golden kGolden[] = {
    {"ВЫБРАТЬ Контрагенты.Наименование ИЗ Справочник.Контрагенты КАК Контрагенты", {"Наименование"}, R"({"columns":[{"name":"Наименование","sources":["Справочник.Контрагенты.Наименование"]}]})"},
    {"ВЫБРАТЬ Т.Ссылка.Наименование КАК Имя ИЗ Документ.Реализация КАК Т", {"Имя"}, R"({"columns":[{"name":"Имя","sources":["Документ.Реализация.Ссылка.Наименование"]}]})"},
    {"ВЫБРАТЬ Поле ПОМЕСТИТЬ ВТ ИЗ Справочник.Номенклатура; ВЫБРАТЬ ВТ.Поле ИЗ ВТ КАК ВТ", {"Поле"}, R"({"columns":[{"name":"Поле","sources":["Справочник.Номенклатура.Поле"]}]})"},
    {"ВЫБРАТЬ А.Сумма ИЗ Документ.А КАК А ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ Б.Сумма ИЗ Документ.Б КАК Б", {"Сумма"}, R"({"columns":[{"name":"Сумма","sources":["Документ.А.Сумма","Документ.Б.Сумма"]}]})"},
    {"ВЫБРАТЬ Т.Номенклатура ИЗ Документ.Реализация.Товары КАК Т ГДЕ Т.Количество > 0", {"Номенклатура"}, R"({"columns":[{"name":"Номенклатура","sources":["Документ.Реализация.Товары.Номенклатура"]}]})"},
    {"ВЫБРАТЬ ЕСТЬNULL(Т.Сумма, 0) КАК С ИЗ (ВЫБРАТЬ Сумма ИЗ Документ.А) КАК Т", {"С"}, R"({"columns":[{"name":"С","sources":["Документ.А.Сумма"]}]})"},
    {"ВЫБРАТЬ ВЫБОР КОГДА Т.Флаг ТОГДА Т.Имя ИНАЧЕ \"\" КОНЕЦ КАК И ИЗ Справочник.Х КАК Т", {"И"}, R"({"columns":[{"name":"И","sources":["Справочник.Х.Имя","Справочник.Х.Флаг"]}]})"},
    {"ВЫБРАТЬ СчетДт.Представление ИЗ РегистрБухгалтерии.Хозрасчетный", {"СчетДтПредставление"}, R"({"columns":[{"name":"СчетДтПредставление"}]})"},
};
}  // namespace

TEST(Lineage, GoldenOutputUnchanged) {
    for (const auto& g : kGolden) CHECK_EQ(AnalyzeSourcesImpl(g.query, Schema(g.cols)), std::string(g.expected));
}

TEST(Lineage, EmptyInputsReturnSchema) {
    CHECK_EQ(AnalyzeSourcesImpl("", "{\"columns\":[]}"), std::string("{\"columns\":[]}"));
    CHECK_EQ(AnalyzeSourcesImpl("ВЫБРАТЬ 1", ""), std::string(""));
}
