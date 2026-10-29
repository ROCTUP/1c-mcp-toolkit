#include "strict/origin.h"

#include <set>

#include "utf_utils.h"

namespace lineage::strict {

const char* OriginName(Origin o) {
    switch (o) {
        case Origin::Open: return "open";
        case Origin::Client: return "client";
        case Origin::ProtectedDirect: return "protected";
        case Origin::ProtectedDerived: return "protected_derived";
        case Origin::Mixed: return "mixed";
        case Origin::Unknown: return "unknown";
    }
    return "unknown";
}

namespace {

bool Listed(const std::set<std::string>& names, const std::string& key) { return names.count(key) != 0; }

}  // namespace

CallClass ClassifyCall(const std::string& name) {
    static const std::set<std::string> kCount = {"КОЛИЧЕСТВО", "COUNT"};
    static const std::set<std::string> kSum = {"СУММА", "SUM", "СРЕДНЕЕ", "AVG"};
    static const std::set<std::string> kMinMax = {"МИНИМУМ", "MIN", "МАКСИМУМ", "MAX"};
    static const std::set<std::string> kPresentation = {"ПРЕДСТАВЛЕНИЕ", "PRESENTATION", "ПРЕДСТАВЛЕНИЕССЫЛКИ",
                                                        "REFPRESENTATION"};
    static const std::set<std::string> kTypeOf = {"ТИПЗНАЧЕНИЯ", "VALUETYPE"};
    static const std::set<std::string> kPlain = {
        "ЕСТЬNULL", "ISNULL", "ПОДСТРОКА", "SUBSTRING", "ГОД", "YEAR", "КВАРТАЛ", "QUARTER", "МЕСЯЦ", "MONTH",
        "ДЕНЬГОДА", "DAYOFYEAR", "ДЕНЬ", "DAY", "НЕДЕЛЯ", "WEEK", "ДЕНЬНЕДЕЛИ", "WEEKDAY", "ЧАС", "HOUR", "МИНУТА",
        "MINUTE", "СЕКУНДА", "SECOND", "НАЧАЛОПЕРИОДА", "BEGINOFPERIOD", "КОНЕЦПЕРИОДА", "ENDOFPERIOD",
        "ДОБАВИТЬКДАТЕ", "DATEADD", "РАЗНОСТЬДАТ", "DATEDIFF", "ДАТАВРЕМЯ", "DATETIME"};
    const std::string key = NormalizeIdentifierKey(name);
    if (Listed(kCount, key)) return CallClass::Count;
    if (Listed(kSum, key)) return CallClass::Sum;
    if (Listed(kMinMax, key)) return CallClass::MinMax;
    if (Listed(kPresentation, key)) return CallClass::Presentation;
    if (Listed(kTypeOf, key)) return CallClass::TypeOf;
    if (Listed(kPlain, key)) return CallClass::Plain;
    return CallClass::Unrecognized;
}

int PeriodArgIndex(const std::string& call_name) {
    const std::string key = NormalizeIdentifierKey(call_name);
    if (key == "НАЧАЛОПЕРИОДА" || key == "BEGINOFPERIOD" || key == "КОНЕЦПЕРИОДА" || key == "ENDOFPERIOD" ||
        key == "ДОБАВИТЬКДАТЕ" || key == "DATEADD") {
        return 1;
    }
    if (key == "РАЗНОСТЬДАТ" || key == "DATEDIFF") return 2;
    return -1;
}

bool IsPeriodName(const std::string& identifier) {
    static const std::set<std::string> kPeriods = {
        "ГОД", "YEAR", "ПОЛУГОДИЕ", "HALFYEAR", "КВАРТАЛ", "QUARTER", "МЕСЯЦ", "MONTH", "ДЕКАДА", "TENDAYS",
        "НЕДЕЛЯ", "WEEK", "ДЕНЬ", "DAY", "ЧАС", "HOUR", "МИНУТА", "MINUTE", "СЕКУНДА", "SECOND"};
    return kPeriods.count(NormalizeIdentifierKey(identifier)) != 0;
}

}  // namespace lineage::strict
