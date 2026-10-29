#include "test_framework.h"
#include "strict/field_policy.h"

#include <iterator>
#include <string>
#include <vector>

using namespace lineage::strict;

TEST(Policy, ParseAndLookup) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Справочник.А":{"presentation":"protected",
        "fields":{"Имя":{"class":"protected"},"Ссылка":{"class":"open","refs":["Справочник.А"]}}}},"params":{"П":"client"}})");
    CHECK(p.ok);
    CHECK(p.policy.ClassOf("справочник.а", "имя") == FieldClass::Protected);   // case-insensitive
    CHECK(p.policy.ClassOf("Справочник.А", "Нет") == FieldClass::Unknown);
    CHECK(!p.policy.HasTable("Справочник.Б"));
    CHECK(p.policy.HasTable("СПРАВОЧНИК.а"));
    CHECK(p.policy.OriginOfParam("П") == ParamOrigin::Client);
    CHECK(p.policy.OriginOfParam("Нет") == ParamOrigin::Missing);
}

TEST(Policy, MalformedIsError) {
    CHECK(!FieldPolicy::FromJson("{").ok);
    CHECK(!FieldPolicy::FromJson("").ok);
    CHECK(!FieldPolicy::FromJson("[]").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":2})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"tables":{}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":"1"})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"А":{"fields":{"Х":{"class":"weird"}}}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"А":{"fields":{"Х":{}}}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"А":{"fields":{"Х":"open"}}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"А":{"presentation":"weird","fields":{}}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"А":{"fields":{"Х":{"class":"open","refs":"Б"}}}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"А":{"fields":{"Х":{"class":"open","refs":[1]}}}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":[]})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"params":{"П":"open"}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"params":{"П":5}})").ok);
}

TEST(Policy, PresentationRules) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Справочник.А":{"presentation":"protected","fields":{"Представление":{"class":"open"}}},
        "Перечисление.Б":{"presentation":"open","fields":{}},
        "Справочник.В":{"fields":{}}}})");
    CHECK(p.ok);
    CHECK(p.policy.PresentationClass("Справочник.А") == FieldClass::Protected);
    CHECK(p.policy.ClassOf("Справочник.А", "Представление") == FieldClass::Protected);   // never loosened by fields
    CHECK(p.policy.PresentationClass("Перечисление.Б") == FieldClass::Open);
    CHECK(p.policy.ClassOf("Перечисление.Б", "представление") == FieldClass::Open);
    CHECK(p.policy.PresentationClass("Справочник.В") == FieldClass::Protected);          // absent -> protected
    CHECK(p.policy.PresentationClass("Справочник.Нет") == FieldClass::Unknown);
    CHECK(p.policy.ClassOf("Справочник.Нет", "Представление") == FieldClass::Unknown);
}

TEST(Policy, RefsAndFieldInfo) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Справочник.А":{"presentation":"protected",
        "fields":{"Ссылка":{"class":"open","refs":["Справочник.А","Справочник.Б"]},"Код":{"class":"open"}}}}})");
    CHECK(p.ok);
    const FieldInfo* f = p.policy.Field("справочник.а", "ссылка");
    CHECK(f != nullptr);
    CHECK(f->cls == FieldClass::Open);
    CHECK_EQ(f->refs.size(), size_t(2));
    CHECK(p.policy.Field("Справочник.А", "Нет") == nullptr);
    CHECK(p.policy.Field("Справочник.Нет", "Код") == nullptr);
}

TEST(Policy, TabularSections) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Документ.Д":{"presentation":"protected",
        "fields":{"Номер":{"class":"open"}},
        "tabular_sections":{"Товары":{"fields":{"Комментарий":{"class":"protected"},"Сумма":{"class":"open"}}}}}}})");
    CHECK(p.ok);
    CHECK(p.policy.HasTable("Документ.Д.Товары"));
    CHECK(p.policy.ClassOf("документ.д.товары", "комментарий") == FieldClass::Protected);
    CHECK(p.policy.ClassOf("Документ.Д.Товары", "Сумма") == FieldClass::Open);
    CHECK(p.policy.PresentationClass("Документ.Д.Товары") == FieldClass::Protected);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"tables":{"Д":{"tabular_sections":{"Т":[]}}}})").ok);
}

TEST(Policy, DuplicateAfterNormalizationOnlyTightens) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"Справочник.А":{"presentation":"open",
        "fields":{"Имя":{"class":"open"},"ИМЯ":{"class":"protected"},"Код":{"class":"protected"},"код":{"class":"open"},
                  "Х":{"class":"open"},"х":{"class":"unknown"}}}}})");
    CHECK(p.ok);
    CHECK(p.policy.ClassOf("Справочник.А", "Имя") == FieldClass::Protected);
    CHECK(p.policy.ClassOf("Справочник.А", "Код") == FieldClass::Protected);
    CHECK(p.policy.ClassOf("Справочник.А", "Х") == FieldClass::Unknown);
}

TEST(Policy, VirtualTables) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"tables":{"РегистрНакопления.Р":{"presentation":"protected",
        "fields":{"Контрагент":{"class":"open","refs":["Справочник.К"]},"Комментарий":{"class":"protected"}}}},
        "virtual_tables":{"РегистрНакопления.Р.Остатки":{"base":"РегистрНакопления.Р",
        "extra_fields":{"СуммаОстаток":{"class":"open"}}}}})");
    CHECK(p.ok);
    CHECK_EQ(p.policy.ResolveVirtualBase("регистрнакопления.р.остатки"), std::string("РегистрНакопления.Р"));
    CHECK_EQ(p.policy.ResolveVirtualBase("РегистрНакопления.Р"), std::string());
    CHECK_EQ(p.policy.ResolveVirtualBase("Нет"), std::string());
    CHECK(p.policy.ClassOf("РегистрНакопления.Р.Остатки", "СуммаОстаток") == FieldClass::Open);
    CHECK(p.policy.ClassOf("РегистрНакопления.Р.Остатки", "Комментарий") == FieldClass::Protected);   // from base
    CHECK(p.policy.ClassOf("РегистрНакопления.Р.Остатки", "Нет") == FieldClass::Unknown);
    CHECK(p.policy.PresentationClass("РегистрНакопления.Р.Остатки") == FieldClass::Protected);
    CHECK(!p.policy.HasTable("РегистрНакопления.Р.Остатки"));
    // the base must be a declared table
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"virtual_tables":{"В":{"base":"Нет"}}})").ok);
    CHECK(!FieldPolicy::FromJson(R"({"version":1,"virtual_tables":{"В":{}}})").ok);
}

TEST(Policy, ParamOrigins) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"params":{"Контр":"token_protected","Дата":"client",
        "МСПТокен1":"token_protected","Х":"token_unknown"}})");
    CHECK(p.ok);
    CHECK(p.policy.OriginOfParam("контр") == ParamOrigin::TokenProtected);
    CHECK(p.policy.OriginOfParam("Дата") == ParamOrigin::Client);
    CHECK(p.policy.OriginOfParam("х") == ParamOrigin::TokenUnknown);
    CHECK(p.policy.OriginOfParam("Другой") == ParamOrigin::Missing);
}

// Parameter names colliding up to case are one parameter to the host: the same origin merges, another one is a
// malformed policy (no merged origin describes the value the host binds).
TEST(Policy, CollidingParamNames) {
    for (const char* json : {R"({"version":1,"params":{"пар":"client","Пар":"token_protected"}})",
                             R"({"version":1,"params":{"Пар":"token_unknown","ПАР":"client"}})",
                             R"({"version":1,"params":{"пар":"token_protected","ПАР":"token_unknown"}})"}) {
        auto p = FieldPolicy::FromJson(json);
        CHECK(!p.ok);
        CHECK_EQ(p.error, std::string("Q_POLICY_SHAPE"));
    }
    auto same = FieldPolicy::FromJson(R"({"version":1,"params":{"пар":"token_protected","Пар":"token_protected"}})");
    CHECK(same.ok);
    CHECK(same.policy.OriginOfParam("ПАР") == ParamOrigin::TokenProtected);
}

TEST(Policy, SetParamsRefusesCollidingOrigins) {
    auto p = FieldPolicy::FromJson(R"({"version":1,"params":{"Старый":"client"}})");
    CHECK(p.ok);
    CHECK(!p.policy.SetParams({{"пар", ParamOrigin::Client}, {"Пар", ParamOrigin::TokenProtected}}));
    CHECK(p.policy.OriginOfParam("Старый") == ParamOrigin::Client);   // nothing changed
    CHECK(p.policy.OriginOfParam("пар") == ParamOrigin::Missing);
    CHECK(p.policy.SetParams({{"пар", ParamOrigin::TokenUnknown}, {"Пар", ParamOrigin::TokenUnknown}}));
    CHECK(p.policy.OriginOfParam("ПАР") == ParamOrigin::TokenUnknown);
    CHECK(p.policy.OriginOfParam("Старый") == ParamOrigin::Missing);   // replaced
}

// A parameter name outside the alphabet of a parameter after '&' cannot be named by a query text, and a host that
// trims or folds it binds its value to another parameter (a client label over a protected value): the policy is
// malformed. Mixed scripts inside the alphabet stay legitimate names.
TEST(Policy, ParamNamesOutsideTheParameterAlphabet) {
    const char* const bad[] = {
        "Пар ",              // trailing space
        " Пар",              // leading space
        "Пар\xC2\xA0",       // NBSP
        "Па\xE2\x80\x8Bр",   // zero-width space
        "Па\xE1\xB2\x80",    // U+1C80, a look-alike of the Cyrillic ve
        "\xC5\xBFum",        // long s
        "\xC4\xB1" "d",      // dotless i
        "\xE2\x84\xAA",      // Kelvin sign
        "",                  // empty
        "1Пар",              // digit first
        "Пар-1",
        "Пар.Х",
    };
    for (const char* name : bad) {
        auto p = FieldPolicy::FromJson(std::string(R"({"version":1,"params":{"Пар":"client",")") + name +
                                       R"(":"token_protected"}})");
        CHECK(!p.ok);
        CHECK_EQ(p.error, std::string("Q_POLICY_SHAPE"));
    }
    // Invalid UTF-8 and raw control characters never reach ReadParams (the JSON parser refuses them), but SetParams
    // takes raw names: a byte that normalization would drop must not make the name collide with a clean one.
    std::vector<std::string> raw(std::begin(bad), std::end(bad));
    raw.push_back("Пар\xFF");
    raw.push_back("Пар\t");
    for (const std::string& name : raw) {
        auto good = FieldPolicy::FromJson(R"({"version":1,"params":{"Пар":"client"}})");
        CHECK(good.ok);
        CHECK(!good.policy.SetParams({{"Пар", ParamOrigin::Client}, {name, ParamOrigin::TokenProtected}}));
        CHECK(good.policy.OriginOfParam("Пар") == ParamOrigin::Client);   // nothing changed
        CHECK(!good.policy.AgreeParam(name, ParamOrigin::TokenProtected));
        CHECK(!good.policy.AgreeParam(name, ParamOrigin::Client));
    }
    auto p = FieldPolicy::FromJson(
        R"({"version":1,"params":{"ДатаНачалаUTC":"client","_x1":"client","Ёж":"token_protected","Пар2":"token_unknown"}})");
    CHECK(p.ok);
    CHECK(p.policy.OriginOfParam("датаначалаutc") == ParamOrigin::Client);
    CHECK(p.policy.OriginOfParam("_X1") == ParamOrigin::Client);
    CHECK(p.policy.OriginOfParam("ёж") == ParamOrigin::TokenProtected);
    CHECK(p.policy.OriginOfParam("Пар2") == ParamOrigin::TokenUnknown);
    CHECK(p.policy.AgreeParam("МСПТокен1", ParamOrigin::TokenProtected));
}

// Parameter names that differ only in yo / ie collide for the origin rule (whether or not the platform tells them
// apart): different origins are a malformed policy in both directions and in any case; the same origin is fine and
// each spelling still resolves.
TEST(Policy, YoCollidingParamNames) {
    for (const char* json : {R"({"version":1,"params":{"Еж":"client","Ёж":"token_protected"}})",
                             R"({"version":1,"params":{"Ёж":"client","Еж":"token_protected"}})",
                             R"({"version":1,"params":{"ёж":"token_unknown","ЕЖ":"client"}})",
                             R"({"version":1,"params":{"еЖ":"token_protected","ЁЖ":"token_unknown"}})"}) {
        auto p = FieldPolicy::FromJson(json);
        CHECK(!p.ok);
        CHECK_EQ(p.error, std::string("Q_POLICY_SHAPE"));
    }
    auto same = FieldPolicy::FromJson(R"({"version":1,"params":{"Еж":"token_protected","ёж":"token_protected"}})");
    CHECK(same.ok);
    CHECK(same.policy.OriginOfParam("еж") == ParamOrigin::TokenProtected);
    CHECK(same.policy.OriginOfParam("ЁЖ") == ParamOrigin::TokenProtected);

    auto p = FieldPolicy::FromJson(R"({"version":1,"params":{"Старый":"client"}})");
    CHECK(p.ok);
    CHECK(!p.policy.SetParams({{"Еж", ParamOrigin::Client}, {"Ёж", ParamOrigin::TokenProtected}}));
    CHECK(p.policy.OriginOfParam("Старый") == ParamOrigin::Client);   // nothing changed
    CHECK(p.policy.SetParams({{"Еж", ParamOrigin::Client}, {"Ёж", ParamOrigin::Client}}));
    CHECK(p.policy.OriginOfParam("Ёж") == ParamOrigin::Client);
    CHECK(!p.policy.AgreeParam("ёж", ParamOrigin::TokenProtected));
    CHECK(p.policy.AgreeParam("ёж", ParamOrigin::Client));
    // The lookup itself does not fold: a spelling the policy does not name stays missing.
    auto only = FieldPolicy::FromJson(R"({"version":1,"params":{"Еж":"client"}})");
    CHECK(only.ok);
    CHECK(only.policy.OriginOfParam("Ёж") == ParamOrigin::Missing);
}
