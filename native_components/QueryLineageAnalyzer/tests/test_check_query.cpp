#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "policy_fixture.h"
#include "strict/check_query.h"
#include "test_framework.h"

using lineage::strict::CheckQueryImpl;
using lineage::strict::CheckQueryWithPolicy;
using lineage::strict::kPolicyAB;
using lineage::strict::kPolicyABJson;
using lineage::strict::kPolicyVTJson;

namespace {

// A test policy (kPolicyAB by default) with a "tokens" table ([str-00001] protected at index 0, [str-00002] unknown
// at index 1) and the given "params" object. The host lists МСПТокен<index+1> in "params" too; the token table
// alone must be enough.
std::string TokenPolicy(const std::string& params_json, const std::string& base = kPolicyABJson) {
    nlohmann::json p = nlohmann::json::parse(base);
    p["params"] = nlohmann::json::parse(params_json);
    p["tokens"] = nlohmann::json::parse(
        R"({"[str-00001]": {"index": 0, "origin": "protected"}, "[str-00002]": {"index": 1, "origin": "unknown"}})");
    return p.dump();
}

nlohmann::json Check(const std::string& query, const std::string& policy) {
    return nlohmann::json::parse(CheckQueryImpl(query, policy));
}

std::string ReplaceAll(std::string text, const std::string& what, const std::string& with) {
    for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + with.size())) {
        text.replace(p, what.size(), with);
    }
    return text;
}

std::vector<std::string> Codes(const nlohmann::json& j) {
    std::vector<std::string> codes;
    for (const auto& r : j["reasons"]) codes.push_back(r["code"].get<std::string>());
    std::sort(codes.begin(), codes.end());
    return codes;
}

bool HasCode(const nlohmann::json& j, const std::string& code) {
    const std::vector<std::string> codes = Codes(j);
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

size_t Count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size())) ++n;
    return n;
}

const char* const kSelectA = "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ ";

}  // namespace

TEST(CheckQuery, JsonContract) {
    auto j = nlohmann::json::parse(CheckQueryImpl("ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А", kPolicyABJson));
    CHECK_EQ(j["api"].get<int>(), 1);
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(j["columns"][0]["name"].get<std::string>(), std::string("Имя"));
    CHECK_EQ(j["columns"][0]["origin"].get<std::string>(), std::string("protected"));
    CHECK(j["columns"][0]["direct"].get<bool>());
    CHECK(j["canonical"].get<std::string>().rfind("ВЫБРАТЬ", 0) == 0);
}

TEST(CheckQuery, IncompleteListsTables) {
    auto j = nlohmann::json::parse(CheckQueryImpl("ВЫБРАТЬ Н.Х ИЗ Справочник.Нет КАК Н", kPolicyABJson));
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("incomplete"));
    CHECK_EQ(j["missing_tables"][0].get<std::string>(), std::string("Справочник.Нет"));
}

TEST(CheckQuery, NeverThrowsAndFailsClosed) {
    auto j = nlohmann::json::parse(CheckQueryImpl("\xFF", "{"));
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
}

TEST(CheckQuery, MalformedPolicyIsConfirm) {
    auto j = nlohmann::json::parse(CheckQueryImpl("ВЫБРАТЬ 1", "{"));
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
    CHECK_EQ(j["reasons"][0]["code"].get<std::string>(), std::string("Q_POLICY_INVALID"));
    CHECK(j["canonical"].get<std::string>().empty());
}

TEST(CheckQuery, ParseFailureCarriesCodeAndPosition) {
    auto j = nlohmann::json::parse(CheckQueryImpl("ВЫБРАТЬ ИЗ ИЗ", kPolicyABJson));
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(j["reasons"][0]["code"].get<std::string>().rfind("Q_PARSE_", 0) == 0);
    CHECK(j["reasons"][0].contains("line"));
    CHECK(j["canonical"].get<std::string>().empty());
}

TEST(CheckQuery, ReasonsDoNotEchoClientText) {
    auto j = nlohmann::json::parse(CheckQueryImpl(
        "ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А ГДЕ А.Имя = \"СекретЗначение\"", kPolicyABJson));
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(j["reasons"].size() > 0);
    CHECK(j["reasons"].dump().find("СекретЗначение") == std::string::npos);
}

TEST(CheckQuery, UnlabelledBatchResultIsConfirm) {
    auto a = nlohmann::json::parse(CheckQueryImpl(
        "ВЫБРАТЬ А.Имя ИЗ Справочник.А КАК А;"
        "ВЫБРАТЬ Б.Сумма ИЗ Справочник.Б КАК Б", kPolicyABJson));
    CHECK_EQ(a["verdict"].get<std::string>(), std::string("confirm"));
    CHECK_EQ(a["reasons"][0]["code"].get<std::string>(), std::string("Q_BATCH_RESULT"));
    auto b = nlohmann::json::parse(CheckQueryImpl(
        "ВЫБРАТЬ Б.Сумма КАК Сумма ПОМЕСТИТЬ Т ИЗ Справочник.Б КАК Б;"
        "УНИЧТОЖИТЬ Т", kPolicyABJson));
    CHECK_EQ(b["verdict"].get<std::string>(), std::string("confirm"));
    CHECK_EQ(b["reasons"][0]["code"].get<std::string>(), std::string("Q_BATCH_RESULT"));
}

TEST(CheckQuery, TokenLiteralBecomesParameterOfItsOrigin) {
    const std::string policy = TokenPolicy(R"({"П": "client"})");
    auto j = Check(std::string(kSelectA) + "А.Имя = \"[str-00001]\"", policy);
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
    const std::string canonical = j["canonical"].get<std::string>();
    CHECK_EQ(Count(canonical, "&МСПТокен1"), size_t(1));
    CHECK(canonical.find("00001") == std::string::npos);

    // Tokens match case-insensitively in their Latin letters, as the host compares them.
    auto upper = Check(std::string(kSelectA) + "А.Имя = \"[STR-00001]\"", policy);
    CHECK_EQ(upper["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(Count(upper["canonical"].get<std::string>(), "&МСПТокен1"), size_t(1));

    // index 1 is МСПТокен2 and keeps its unknown origin.
    auto unknown = Check(std::string(kSelectA) + "А.Имя = \"[str-00002]\"", policy);
    CHECK_EQ(unknown["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(HasCode(unknown, "Q_EQ_UNKNOWN_TOKEN"));
    CHECK_EQ(Count(unknown["canonical"].get<std::string>(), "&МСПТокен2"), size_t(1));
}

TEST(CheckQuery, SameTokenLiteralTwiceIsOneParameter) {
    auto j = Check(std::string(kSelectA) + "А.Имя = \"[str-00001]\" И А.Код = \"[str-00001]\"", TokenPolicy("{}"));
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(Count(j["canonical"].get<std::string>(), "&МСПТокен1"), size_t(2));
}

TEST(CheckQuery, TokenInsideLongerOrUnissuedLiteralStaysLiteral) {
    const std::string policy = TokenPolicy("{}");
    for (const char* literal : {"\"x[str-00001]\"", "\"[str-00001] \"", "\"[str-09999]\""}) {
        auto j = Check(std::string(kSelectA) + "А.Имя = " + literal, policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
        CHECK(HasCode(j, "Q_EQ_LITERAL"));
        CHECK(j["canonical"].get<std::string>().find("&МСПТокен") == std::string::npos);
    }
}

// The rewritten literal is checked exactly as the parameter written by hand, in every place of the query that holds
// an expression ($ stands for the token).
void CheckLiteralAsParameter(const std::string& policy, const std::string& shape) {
    const std::pair<const char*, const char*> tokens[] = {{"\"[str-00001]\"", "&МСПТокен1"},
                                                          {"\"[str-00002]\"", "&МСПТокен2"}};
    for (const auto& token : tokens) {
        auto a = Check(ReplaceAll(shape, "$", token.first), policy);
        auto b = Check(ReplaceAll(shape, "$", token.second), policy);
        CHECK_EQ(a["verdict"].get<std::string>(), b["verdict"].get<std::string>());
        CHECK(Codes(a) == Codes(b));
        CHECK_EQ(a["canonical"].get<std::string>(), b["canonical"].get<std::string>());
        CHECK(a["canonical"].get<std::string>().find("&МСПТокен") != std::string::npos);
        CHECK_EQ(a["columns"].dump(), b["columns"].dump());
    }
}

TEST(CheckQuery, TokenLiteralChecksAsTheParameter) {
    const std::string params = R"({"МСПТокен1": "token_protected", "МСПТокен2": "token_unknown"})";
    const std::string policy = TokenPolicy(params);
    const char* const shapes[] = {
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = $",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ $ = А.Имя",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В ($)",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя <> $",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя ПОДОБНО $",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = ПОДСТРОКА($, 1, 3)",
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код = $",
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.СсылкаНаА.Имя = $",
        "ВЫБРАТЬ $ КАК Поле",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Ссылка В "
        "(ВЫБРАТЬ А2.Ссылка ИЗ Справочник.А КАК А2 ГДЕ А2.Имя = $)",
        "ВЫБРАТЬ Б.Сумма КАК С ИЗ Справочник.Б КАК Б "
        "ВНУТРЕННЕЕ СОЕДИНЕНИЕ Справочник.А КАК А ПО Б.СсылкаНаА = А.Ссылка И А.Имя = $",
        "ВЫБРАТЬ Х.С КАК С ИЗ (ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = $) КАК Х",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = $ "
        "ОБЪЕДИНИТЬ ВСЕ ВЫБРАТЬ А.Ссылка ИЗ Справочник.А КАК А ГДЕ А.Код = $",
        "ВЫБРАТЬ А.Имя КАК Имя, КОЛИЧЕСТВО(*) КАК Н ИЗ Справочник.А КАК А СГРУППИРОВАТЬ ПО А.Имя ИМЕЮЩИЕ А.Имя = $",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А УПОРЯДОЧИТЬ ПО ВЫБОР КОГДА А.Имя = $ ТОГДА 0 ИНАЧЕ 1 КОНЕЦ",
    };
    for (const char* shape : shapes) CheckLiteralAsParameter(policy, shape);
    CheckLiteralAsParameter(TokenPolicy(params, kPolicyVTJson),
                            "ВЫБРАТЬ О.СуммаОстаток КАК С "
                            "ИЗ РегистрНакопления.Взаиморасчеты.Остатки(, Контрагент.Имя = $) КАК О");
}

void CheckPolicyInvalid(const nlohmann::json& j) {
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(Codes(j) == std::vector<std::string>{"Q_POLICY_INVALID"});
    CHECK(j["canonical"].get<std::string>().empty());
    CHECK(j["columns"].empty());
}

// The host binds the token's value to МСПТокен<index+1>: a "params" label of another origin would relabel it (a
// client label returns a protected value unmasked, and an open field compared with it becomes an oracle). Any
// difference, in either direction, makes the policy invalid, whatever the query, written by hand or bound.
TEST(CheckQuery, TokenParamOfAnotherOriginIsPolicyInvalid) {
    const char* const shapes[] = {
        "ВЫБРАТЬ $ КАК Х",
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код В ($)",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = $",
    };
    const char* const params[] = {
        R"({"МСПТокен1": "client"})",
        R"({"мсптокен1": "client"})",
        R"({"МСПТокен1": "token_unknown"})",
        R"({"МСПТокен2": "client"})",
        R"({"МСПТокен2": "token_protected"})",
        // colliding spellings of different origins make the policy invalid by themselves
        R"({"МСПТокен1": "token_protected", "мсптокен1": "client"})",
    };
    for (const char* param : params) {
        const std::string policy = TokenPolicy(param);
        CheckPolicyInvalid(Check("ВЫБРАТЬ 1 КАК Х", policy));
        for (const char* shape : shapes) {
            for (const char* operand : {"&МСПТокен1", "\"[str-00001]\"", "&МСПТокен2", "\"[str-00002]\""}) {
                CheckPolicyInvalid(Check(ReplaceAll(shape, "$", operand), policy));
            }
        }
    }
}

// A "params" entry with the token's origin agrees; without an entry the token parameter, bound or written by hand,
// takes the token's origin.
TEST(CheckQuery, TokenParamOfTheTokenOriginAgrees) {
    for (const char* param : {R"({"МСПТокен1": "token_protected", "мсптокен2": "token_unknown"})", "{}"}) {
        const std::string policy = TokenPolicy(param);
        for (const char* operand : {"&МСПТокен1", "\"[str-00001]\""}) {
            auto selected = Check(ReplaceAll("ВЫБРАТЬ $ КАК Х", "$", operand), policy);
            CHECK_EQ(selected["verdict"].get<std::string>(), std::string("safe"));
            CHECK_EQ(selected["columns"][0]["origin"].get<std::string>(), std::string("protected"));
            CHECK(selected["columns"][0]["direct"].get<bool>());
            auto j = Check(std::string(kSelectA) + "А.Имя = " + operand, policy);
            CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
        }
        for (const char* operand : {"&МСПТокен2", "\"[str-00002]\""}) {
            auto j = Check(std::string(kSelectA) + "А.Имя = " + operand, policy);
            CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
            CHECK(HasCode(j, "Q_EQ_UNKNOWN_TOKEN"));
        }
    }
}

// A token parameter whose index the table does not list is an ordinary parameter: its "params" origin holds.
TEST(CheckQuery, TokenParamWithoutTableEntryKeepsParamsOrigin) {
    auto client = Check(std::string(kSelectA) + "А.Имя = &МСПТокен3", TokenPolicy(R"({"МСПТокен3": "client"})"));
    CHECK_EQ(client["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(HasCode(client, "Q_EQ_CLIENT_PARAM"));

    nlohmann::json p = nlohmann::json::parse(kPolicyABJson);
    p["params"] = nlohmann::json::parse(R"({"МСПТокен1": "client"})");
    auto no_table = Check(std::string(kSelectA) + "А.Имя = &МСПТокен1", p.dump());
    CHECK_EQ(no_table["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(HasCode(no_table, "Q_EQ_CLIENT_PARAM"));

    auto missing = Check(std::string(kSelectA) + "А.Имя = &МСПТокен3", TokenPolicy("{}"));
    CHECK_EQ(missing["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(HasCode(missing, "Q_RESOLVE_UNKNOWN_PARAM"));
    CHECK(!missing["canonical"].get<std::string>().empty());
}

// The table names one token once: two keys equal up to case are malformed (bsl::ParseTokenTable).
TEST(CheckQuery, CaseVariantTokenKeysArePolicyInvalid) {
    nlohmann::json p = nlohmann::json::parse(kPolicyABJson);
    p["tokens"] = nlohmann::json::parse(
        R"({"[str-00001]": {"index": 0, "origin": "protected"}, "[STR-00001]": {"index": 1, "origin": "protected"}})");
    CheckPolicyInvalid(Check(std::string(kSelectA) + "А.Имя = \"[str-00001]\"", p.dump()));
}

TEST(CheckQuery, MalformedTokenTableIsPolicyInvalid) {
    const char* const tables[] = {
        R"({"[str-00001]": {"index": -1, "origin": "protected"}})",
        R"({"[str-00001]": {"index": 0, "origin": "open"}})",
        R"({"not-a-token": {"index": 0, "origin": "protected"}})",
        R"([])",
    };
    for (const char* table : tables) {
        nlohmann::json p = nlohmann::json::parse(kPolicyABJson);
        p["tokens"] = nlohmann::json::parse(table);
        CheckPolicyInvalid(Check(std::string(kSelectA) + "А.Имя = \"[str-00001]\"", p.dump()));
        CheckPolicyInvalid(Check("ВЫБРАТЬ 1 КАК Х", p.dump()));
    }
}

// Query texts in client code are checked without a token table: a token literal there stays a literal (the code
// checker refuses it as C_TOKEN_IN_QUERY).
TEST(CheckQuery, PolicyOnlyCheckBindsNoTokens) {
    const auto r = CheckQueryWithPolicy(std::string(kSelectA) + "А.Имя = \"[str-00001]\"", kPolicyAB);
    CHECK_EQ(r.verdict, std::string("confirm"));
    CHECK(r.canonical.find("&МСПТокен") == std::string::npos);
}

// Parameter names are one name to the host up to case, and it binds whichever spelling it meets last: two "params"
// spellings of different origins cannot be told apart, so the policy is invalid, in either JSON order and with or
// without a token table (a client label over the bound protected value would return it unmasked).
TEST(CheckQuery, CaseCollidingParamsOfDifferentOriginsArePolicyInvalid) {
    const char* const params[] = {
        R"({"пар": "client", "Пар": "token_protected"})",
        R"({"Пар": "token_protected", "пар": "client"})",
        R"({"ПАР": "client", "пар": "token_unknown"})",
        R"({"Пар": "token_protected", "ПАР": "token_unknown"})",
        R"({"Par": "client", "PAR": "token_protected"})",
        R"({"Тёк": "client", "ТЁК": "token_protected"})",
        R"({"Пар": "client", "пар": "client", "пАр": "token_protected"})",
    };
    const char* const queries[] = {
        "ВЫБРАТЬ 1 КАК Х",
        "ВЫБРАТЬ &Пар КАК Х",
        "ВЫБРАТЬ &пар КАК Х",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &Пар",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (&Пар)",
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код В (&пар, &ПАР)",
    };
    for (const char* param : params) {
        nlohmann::json no_tokens = nlohmann::json::parse(kPolicyABJson);
        no_tokens["params"] = nlohmann::json::parse(param);
        for (const std::string& policy : {TokenPolicy(param), no_tokens.dump()}) {
            for (const char* query : queries) CheckPolicyInvalid(Check(query, policy));
        }
    }
}

// Spellings of one origin are one parameter: the policy checks as with a single entry.
TEST(CheckQuery, CaseCollidingParamsOfOneOriginMerge) {
    const std::pair<const char*, const char*> cases[] = {
        {R"({"пар": "client", "Пар": "client", "ПАР": "client"})", R"({"Пар": "client"})"},
        {R"({"пар": "token_protected", "Пар": "token_protected"})", R"({"Пар": "token_protected"})"},
        {R"({"пар": "token_unknown", "ПАР": "token_unknown"})", R"({"Пар": "token_unknown"})"},
        {R"({"МСПТокен1": "token_protected", "мсптокен1": "token_protected"})", R"({"МСПТокен1": "token_protected"})"},
    };
    const char* const queries[] = {
        "ВЫБРАТЬ &Пар КАК Х",
        "ВЫБРАТЬ &пар КАК Х",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &Пар",
        "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя В (&Пар)",
        "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код В (&пар, &ПАР)",
        "ВЫБРАТЬ &МСПТокен1 КАК Х",
    };
    for (const auto& c : cases) {
        for (const char* query : queries) {
            auto merged = Check(query, TokenPolicy(c.first));
            auto single = Check(query, TokenPolicy(c.second));
            CHECK(!HasCode(merged, "Q_POLICY_INVALID"));
            CHECK_EQ(merged.dump(), single.dump());
        }
    }
    auto selected = Check("ВЫБРАТЬ &пар КАК Х", TokenPolicy(cases[0].first));
    CHECK_EQ(selected["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(selected["columns"][0]["origin"].get<std::string>(), std::string("client"));
}

// Names that only look like a token parameter are other parameters (or no parameter at all): a "params" entry for
// them does not collide with the token parameter, and the token parameter keeps the token's origin.
TEST(CheckQuery, LookAlikeTokenParamNamesAreOtherNames) {
    // A "params" spelling that differs from the token parameter only in ie / yo must carry the token's origin
    // (YoInParamNames): it is checked at the end on its own.
    const std::string policy = TokenPolicy(R"({"МСПТокен01": "client", "MСПТокен1": "client"})");
    for (const char* name : {"&МСПТокен01"}) {
        auto j = Check(std::string("ВЫБРАТЬ ") + name + " КАК Х", policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
        CHECK_EQ(j["columns"][0]["origin"].get<std::string>(), std::string("client"));
        CHECK(!j["columns"][0]["direct"].get<bool>());
    }
    auto token = Check("ВЫБРАТЬ &МСПТокен1 КАК Х", policy);
    CHECK_EQ(token["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(token["columns"][0]["origin"].get<std::string>(), std::string("protected"));
    CHECK(token["columns"][0]["direct"].get<bool>());

    // A Latin M among Cyrillic letters is not an identifier the parser accepts.
    auto latin = Check("ВЫБРАТЬ &MСПТокен1 КАК Х", policy);
    CHECK_EQ(latin["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(HasCode(latin, "Q_PARSE_MIXED_SCRIPT"));
    CHECK(latin["columns"].empty());

    // '&' must touch the name: a space or a line break between them is a parse failure, never a parameter.
    for (const char* query : {"ВЫБРАТЬ & МСПТокен1 КАК Х", "ВЫБРАТЬ &\nМСПТокен1 КАК Х", "ВЫБРАТЬ & МСПТокён1 КАК Х"}) {
        auto j = Check(query, policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
        CHECK(HasCode(j, "Q_PARSE_TOKEN"));
        CHECK(j["columns"].empty());
    }

    // The yo spelling of a token parameter: another origin than the token's is a malformed policy; with the token's
    // origin both spellings are the token's.
    CheckPolicyInvalid(Check("ВЫБРАТЬ &МСПТокен1 КАК Х", TokenPolicy(R"({"МСПТокён1": "client"})")));
    const std::string same = TokenPolicy(R"({"МСПТокён1": "token_protected"})");
    for (const char* query : {"ВЫБРАТЬ &МСПТокён1 КАК Х", "ВЫБРАТЬ &МСПТокен1 КАК Х"}) {
        auto j = Check(query, same);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
        CHECK_EQ(j["columns"][0]["origin"].get<std::string>(), std::string("protected"));
    }
}

// A token literal as the СПЕЦСИМВОЛ operand is the token parameter there too, and ПОДОБНО over a protected value is
// refused whichever field it is matched against.
TEST(CheckQuery, TokenLiteralAsEscapeCharacterIsConfirm) {
    const std::string policy = TokenPolicy("{}");
    for (const char* query : {
             "ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"[str-00001]\"",
             "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"[str-00001]\"",
             "ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код ПОДОБНО \"а%\" СПЕЦСИМВОЛ &МСПТокен1"}) {
        auto j = Check(query, policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("confirm"));
        CHECK(HasCode(j, "Q_LIKE_PROTECTED"));
        const std::string canonical = j["canonical"].get<std::string>();
        CHECK(canonical.find("СПЕЦСИМВОЛ &МСПТокен") != std::string::npos);
        CHECK(canonical.find("[str-") == std::string::npos);
    }
    // An unknown token there is the unknown token parameter: never safe.
    auto unknown = Check("ВЫБРАТЬ Б.Ссылка КАК С ИЗ Справочник.Б КАК Б ГДЕ Б.Код ПОДОБНО \"а%\" СПЕЦСИМВОЛ \"[str-00002]\"",
                         policy);
    CHECK_EQ(unknown["verdict"].get<std::string>(), std::string("confirm"));
    CHECK(HasCode(unknown, "Q_UNKNOWN_EXPR"));
    CHECK(unknown["canonical"].get<std::string>().find("СПЕЦСИМВОЛ &МСПТокен2") != std::string::npos);
}

// One index names one token parameter: two tokens with the same index are a malformed table.
TEST(CheckQuery, RepeatedTokenIndexIsPolicyInvalid) {
    nlohmann::json p = nlohmann::json::parse(kPolicyABJson);
    p["tokens"] = nlohmann::json::parse(
        R"({"[str-00001]": {"index": 0, "origin": "protected"}, "[str-00003]": {"index": 0, "origin": "unknown"}})");
    CheckPolicyInvalid(Check(std::string(kSelectA) + "А.Имя = \"[str-00001]\"", p.dump()));
    CheckPolicyInvalid(Check("ВЫБРАТЬ &МСПТокен1 КАК Х", p.dump()));
    CheckPolicyInvalid(Check("ВЫБРАТЬ 1 КАК Х", p.dump()));
}

// A "params" spelling no query text can name (a space, an invisible or look-alike character, empty, a digit first)
// next to a clean one: the host may trim or fold it onto the clean name and bind the protected value there, so the
// policy is invalid whatever the query, never a client label for &<clean name>.
TEST(CheckQuery, ParamNameOutsideTheParameterAlphabetIsPolicyInvalid) {
    const std::pair<const char*, const char*> cases[] = {
        {R"({"Пар": "client", "Пар ": "token_protected"})", "ВЫБРАТЬ &Пар КАК Х"},
        {R"({"Пар": "client", " Пар": "token_protected"})", "ВЫБРАТЬ &Пар КАК Х"},
        {"{\"Пар\": \"client\", \"Пар\xC2\xA0\": \"token_protected\"}", "ВЫБРАТЬ &Пар КАК Х"},
        {"{\"Пар\": \"client\", \"Па\xE2\x80\x8Bр\": \"token_protected\"}", "ВЫБРАТЬ &Пар КАК Х"},
        {"{\"ПаВ\": \"client\", \"Па\xE1\xB2\x80\": \"token_protected\"}", "ВЫБРАТЬ &ПаВ КАК Х"},
        {"{\"Sum\": \"client\", \"\xC5\xBFum\": \"token_protected\"}", "ВЫБРАТЬ &Sum КАК Х"},
        {"{\"Id\": \"client\", \"\xC4\xB1" "d\": \"token_protected\"}", "ВЫБРАТЬ &ID КАК Х"},
        {"{\"k\": \"client\", \"\xE2\x84\xAA\": \"token_protected\"}", "ВЫБРАТЬ &k КАК Х"},
        {R"({"": "token_protected", "Пар": "client"})", "ВЫБРАТЬ &Пар КАК Х"},
        {R"({"1Пар": "token_protected", "Пар": "client"})", "ВЫБРАТЬ &Пар КАК Х"},
    };
    for (const auto& c : cases) {
        nlohmann::json no_tokens = nlohmann::json::parse(kPolicyABJson);
        no_tokens["params"] = nlohmann::json::parse(c.first);
        for (const std::string& policy : {TokenPolicy(c.first), no_tokens.dump()}) {
            CheckPolicyInvalid(Check(c.second, policy));
            CheckPolicyInvalid(Check("ВЫБРАТЬ 1 КАК Х", policy));
        }
    }
}

// The letter yo (U+0401 / U+0451) is one letter up to case: two spellings of different origins collide. Spellings
// that differ in ie / yo collide as well, whether or not the platform tells them apart (otherwise &Еж could keep the
// client label while the host binds the protected value of the yo spelling to it); with one origin each spelling
// still resolves.
TEST(CheckQuery, YoInParamNames) {
    CheckPolicyInvalid(Check("ВЫБРАТЬ &ёж КАК Х", TokenPolicy(R"({"Ёж": "client", "ёж": "token_protected"})")));
    for (const char* params : {R"({"Еж": "client", "ёж": "token_protected"})",
                               R"({"Ёж": "client", "Еж": "token_protected"})",
                               R"({"ёж": "client", "ЕЖ": "token_unknown"})"}) {
        nlohmann::json no_tokens = nlohmann::json::parse(kPolicyABJson);
        no_tokens["params"] = nlohmann::json::parse(params);
        for (const std::string& policy : {TokenPolicy(params), no_tokens.dump()}) {
            CheckPolicyInvalid(Check("ВЫБРАТЬ &Еж КАК Х", policy));
            CheckPolicyInvalid(Check("ВЫБРАТЬ &Ёж КАК Х", policy));
        }
    }
    const std::string policy = TokenPolicy(R"({"Еж": "token_protected", "ёж": "token_protected"})");
    for (const char* query : {"ВЫБРАТЬ &Еж КАК Х", "ВЫБРАТЬ &ЁЖ КАК Х"}) {
        auto j = Check(query, policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
        CHECK_EQ(j["columns"][0]["origin"].get<std::string>(), std::string("protected"));
    }
}

// Mixed scripts inside the parameter alphabet are legitimate "params" names. A query text naming one is refused by
// the parser's own mixed-script rule, not by the policy.
TEST(CheckQuery, MixedScriptParamNameIsAccepted) {
    const std::string policy = TokenPolicy(R"({"ДатаНачалаUTC": "client", "Пар": "client"})");
    auto j = Check("ВЫБРАТЬ &Пар КАК Х", policy);
    CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(j["columns"][0]["origin"].get<std::string>(), std::string("client"));
    auto named = Check("ВЫБРАТЬ &ДатаНачалаUTC КАК Х", policy);
    CHECK(Codes(named) == std::vector<std::string>{"Q_PARSE_MIXED_SCRIPT"});
}


namespace {

// kPolicyAB plus the constants table (an open number, a protected string, a reference) and one constant object.
std::string ConstantsPolicy() {
    nlohmann::json p = nlohmann::json::parse(kPolicyABJson);
    p["tables"]["Константы"] = nlohmann::json::parse(R"({"presentation": "protected", "fields": {
        "Порог": {"class": "open"}, "ИмяОрганизации": {"class": "protected"},
        "Основной": {"class": "open", "refs": ["Справочник.А"]}}})");
    p["tables"]["Константа.Порог"] = nlohmann::json::parse(R"({"fields": {"Значение": {"class": "open"}}})");
    return p.dump();
}

std::string FirstOrigin(const nlohmann::json& j) {
    return j["columns"].empty() ? std::string("no columns") : j["columns"][0]["origin"].get<std::string>();
}

}  // namespace

// ИЗ Константы is the constants table, not an undefined temp table: its columns are the policy fields of the
// "Константы" table, under any alias and in either spelling (Constants maps to the same key).
TEST(CheckQuery, ConstantsTableResolvesThroughThePolicy) {
    const std::string policy = ConstantsPolicy();
    for (const char* query : {
             "ВЫБРАТЬ Константы.Порог КАК П ИЗ Константы КАК Константы",
             "ВЫБРАТЬ К.Порог КАК П ИЗ Константы КАК К",
             "ВЫБРАТЬ Константы.Порог КАК П ИЗ Константы",
             "ВЫБРАТЬ Порог КАК П ИЗ Константы",
             "выбрать константы.Порог как П из константы как константы",
             "ВЫБРАТЬ К.Порог КАК П ИЗ Constants КАК К",
             "ВЫБРАТЬ Constants.Порог КАК П ИЗ Constants",
             "SELECT Constants.Порог AS П FROM Constants AS Constants",
         }) {
        auto j = Check(query, policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
        CHECK_EQ(FirstOrigin(j), std::string("open"));
        CHECK(j["missing_tables"].empty());
    }
    // a protected constant keeps its class, also behind a reference
    CHECK_EQ(FirstOrigin(Check("ВЫБРАТЬ Константы.ИмяОрганизации КАК Нм ИЗ Константы КАК Константы", policy)),
             std::string("protected"));
    CHECK_EQ(FirstOrigin(Check("ВЫБРАТЬ Константы.Основной.Имя КАК Нм ИЗ Константы КАК Константы", policy)),
             std::string("protected"));
    auto like = Check("ВЫБРАТЬ Константы.Порог КАК П ИЗ Константы КАК Константы "
                      "ГДЕ Константы.ИмяОрганизации ПОДОБНО \"А%\"", policy);
    CHECK(like["verdict"].get<std::string>() != "safe");
    CHECK(HasCode(like, "Q_LIKE_PROTECTED"));
    // a constant the policy does not list stays fail-closed
    auto unknown = Check("ВЫБРАТЬ Константы.Нет КАК Н ИЗ Константы КАК Константы", policy);
    CHECK(unknown["verdict"].get<std::string>() != "safe");
    CHECK(HasCode(unknown, "Q_UNKNOWN_COLUMN"));
    // a constant object: its Значение is an ordinary field of a dotted table
    auto value = Check("ВЫБРАТЬ К.Значение КАК З ИЗ Константа.Порог КАК К", policy);
    CHECK_EQ(value["verdict"].get<std::string>(), std::string("safe"));
    CHECK_EQ(FirstOrigin(value), std::string("open"));
}

// Without a "Константы" table the policy is incomplete: the host is asked for it under its Russian key, whatever the
// spelling in the text.
TEST(CheckQuery, ConstantsTableMissingFromThePolicy) {
    for (const char* query : {"ВЫБРАТЬ Константы.Порог КАК П ИЗ Константы КАК Константы",
                              "ВЫБРАТЬ К.Порог КАК П ИЗ Constants КАК К",
                              "ВЫБРАТЬ Порог КАК П ИЗ константы"}) {
        auto j = Check(query, kPolicyABJson);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("incomplete"));
        CHECK_EQ(j["missing_tables"].size(), 1u);
        CHECK_EQ(j["missing_tables"][0].get<std::string>(), std::string("Константы"));
    }
    auto value = Check("ВЫБРАТЬ К.Значение КАК З ИЗ Константа.Порог КАК К", kPolicyABJson);
    CHECK_EQ(value["verdict"].get<std::string>(), std::string("incomplete"));
    CHECK_EQ(value["missing_tables"][0].get<std::string>(), std::string("Константа.Порог"));
}

// A temporary table created earlier in the batch under the name Константы shadows the constants table: its column
// (protected here) wins over the open policy field of the same name.
TEST(CheckQuery, TempTableNamedConstantsShadowsTheConstantsTable) {
    const char* query = "ВЫБРАТЬ А.Имя КАК Порог ПОМЕСТИТЬ Константы ИЗ Справочник.А КАК А; "
                        "ВЫБРАТЬ Константы.Порог КАК П ИЗ Константы КАК Константы";
    for (const std::string& policy : {ConstantsPolicy(), kPolicyABJson}) {
        auto j = Check(query, policy);
        CHECK_EQ(j["verdict"].get<std::string>(), std::string("safe"));
        CHECK_EQ(FirstOrigin(j), std::string("protected"));
        CHECK(j["missing_tables"].empty());
    }
}
