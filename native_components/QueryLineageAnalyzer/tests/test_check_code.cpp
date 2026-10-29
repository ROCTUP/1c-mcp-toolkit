#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "bsl/bsl_types.h"
#include "bsl/check_code.h"
#include "policy_fixture.h"
#include "test_framework.h"

using lineage::bsl::CheckCodeImpl;
using lineage::strict::kPolicyABJson;

namespace {

using Json = nlohmann::json;

Json Check(const std::string& code, const std::string& policy = kPolicyABJson) {
    Json j = Json::parse(CheckCodeImpl(code, policy));
    if (std::getenv("QLA_DEBUG")) std::cout << j.dump() << std::endl;
    return j;
}

std::string Verdict(const Json& j) { return j["verdict"].get<std::string>(); }

bool HasReason(const Json& j, const char* code) {
    for (const Json& r : j["reasons"]) {
        if (r["code"].get<std::string>() == code) return true;
    }
    return false;
}

bool HasRisk(const Json& j, const char* code) {
    for (const Json& r : j["risks"]) {
        if (r["code"].get<std::string>() == code) return true;
    }
    return false;
}

std::string Label(const Json& j, const char* path) {
    const auto it = j["result_labels"].find(path);
    return it == j["result_labels"].end() ? std::string("<none>") : it->get<std::string>();
}

// The policy of kPolicyABJson with two issued tokens.
std::string PolicyWithTokens() {
    Json p = Json::parse(kPolicyABJson);
    p["tokens"] = Json::parse(R"({"[str-00001]": {"index": 0, "origin": "protected"}})");
    return p.dump();
}

const char* kQuery =
    "З = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С, А.Имя КАК Имя ИЗ Справочник.А КАК А\");\n";

}  // namespace

TEST(CheckCode, ResultLabels) {
    // The plan expects "open" for the reference column; the checker writes "ref" so that the masker applies the
    // reference rule to the object description of the reference (see check_code.h). The column is called Имя here:
    // "И" is the AND keyword of the query language, so the plan's alias does not parse.
    const Json j = Check(std::string(kQuery) + "Результат = З.Выполнить().Выгрузить();");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("container"));
    CHECK_EQ(Label(j, "$[*]"), std::string("container"));
    CHECK_EQ(Label(j, "$[*].Имя"), std::string("protected"));
    CHECK_EQ(Label(j, "$[*].С"), std::string("ref"));
    CHECK(!j["canonical"].get<std::string>().empty());
}

TEST(CheckCode, ClientLiteralInResultIsClient) {
    const Json j = Check("Результат = Новый Структура(\"К\", \"x\");");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("container"));
    CHECK_EQ(Label(j, "$.К"), std::string("client"));
}

TEST(CheckCode, MixedContainerNeedsConfirmation) {
    const Json j = Check(
        "З = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А\"); Т = З.Выполнить().Выгрузить();\n"
        "М = Новый Массив; М.Добавить(Т[0].Имя); М.Добавить(\"догадка\"); Результат = М;");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK_EQ(Label(j, "$[*]"), std::string("mixed"));
    CHECK(HasReason(j, "C_MIXED_RESULT"));
}

TEST(CheckCode, NotLoadedPolicyFailsClosed) {
    const Json j = Check("Результат = 1;", "{");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasReason(j, "C_POLICY_INVALID"));
    CHECK(j["canonical"].get<std::string>().empty());
    CHECK(HasRisk(j, "R_UNKNOWN_EFFECT"));
}

// The code checker reads the same policy: "params" spellings colliding up to case with different origins make it
// malformed there too, though the code sets its own parameters.
TEST(CheckCode, CollidingParamOriginsArePolicyInvalid) {
    Json policy = Json::parse(kPolicyABJson);
    policy["params"] = Json::parse(R"({"пар": "client", "Пар": "token_protected"})");
    const Json j = Check("Результат = 1;", policy.dump());
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasReason(j, "C_POLICY_INVALID"));
    CHECK(HasRisk(j, "R_UNKNOWN_EFFECT"));
}

TEST(CheckCode, EnvelopeShape) {
    const Json j = Check("Результат = 1;");
    CHECK_EQ(j["api"].get<int>(), 1);
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK(j["reasons"].is_array() && j["reasons"].empty());
    CHECK(j["risks"].is_array() && j["risks"].empty());
    CHECK(j["missing_tables"].is_array());
    CHECK(j["result_labels"].is_object());
    CHECK_EQ(Label(j, "$"), std::string("client"));
    CHECK(j["canonical"].get<std::string>().find("Результат") != std::string::npos);
}

TEST(CheckCode, UnassignedResultIsOpen) {
    const Json j = Check("Х = 1;");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("open"));
}

TEST(CheckCode, ProtectedScalarIsLabelledProtected) {
    const Json j = Check(std::string(kQuery) + "Т = З.Выполнить().Выгрузить(); Результат = Т[0].Имя;");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("protected"));
}

TEST(CheckCode, DerivedLabelNeedsConfirmation) {
    const Json j = Check(std::string(kQuery) + "Т = З.Выполнить().Выгрузить(); Результат = НЕ Т[0].Имя;");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK_EQ(Label(j, "$"), std::string("protected_derived"));
    CHECK(HasReason(j, "C_DERIVED_RESULT"));
}

TEST(CheckCode, UnknownLabelNeedsConfirmation) {
    const Json j = Check("Результат = Новый ТаблицаЗначений;");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK_EQ(Label(j, "$"), std::string("unknown"));
    CHECK(HasReason(j, "C_UNLABELED_RESULT"));
}

TEST(CheckCode, StructureKeysKeepSeparateLabels) {
    const Json j = Check(std::string(kQuery) + "Т = З.Выполнить().Выгрузить();\n"
                         "Результат = Новый Структура(\"С, Имя\", Т[0].С, Т[0].Имя); Результат.Вставить(\"К\", 5);");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$.С"), std::string("ref"));
    CHECK_EQ(Label(j, "$.Имя"), std::string("protected"));
    CHECK_EQ(Label(j, "$.К"), std::string("client"));
}

TEST(CheckCode, ArrayOfStructuresIsLabelledPerKey) {
    const Json j = Check(std::string(kQuery) + "Т = З.Выполнить().Выгрузить(); М = Новый Массив;\n"
                         "Для Каждого Стр Из Т Цикл М.Добавить(Новый Структура(\"С, Имя\", Стр.С, Стр.Имя)); КонецЦикла;\n"
                         "Результат = М;");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("container"));
    CHECK_EQ(Label(j, "$[*]"), std::string("container"));
    CHECK_EQ(Label(j, "$[*].Имя"), std::string("protected"));
    CHECK_EQ(Label(j, "$[*].С"), std::string("ref"));
}

TEST(CheckCode, UnloadedColumnIsAnArrayOfThatColumn) {
    const Json j = Check(std::string(kQuery) + "Результат = З.Выполнить().Выгрузить().ВыгрузитьКолонку(\"Имя\");");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$[*]"), std::string("protected"));
}

// A cell of an open column without reference targets, moved as is, is "open" wherever it lands (as a table cell is):
// the masker applies its reference rule to the serialized value by shape, whatever the label.
TEST(CheckCode, OpenColumnCellWithoutTargetsIsOpen) {
    const std::string q =
        "З = Новый Запрос(\"ВЫБРАТЬ Б.Сумма КАК Сумма, Б.Ссылка КАК С ИЗ Справочник.Б КАК Б\");\n"
        "В = З.Выполнить().Выбрать(); В.Следующий(); Т = З.Выполнить().Выгрузить();\n";
    const Json s = Check(q + "Х = В.Сумма; Результат = Новый Структура(\"Сумма, С, Т\", Х, В.С, Т[0].Сумма);");
    CHECK_EQ(Verdict(s), std::string("safe"));
    CHECK_EQ(Label(s, "$.Сумма"), std::string("open"));
    CHECK_EQ(Label(s, "$.С"), std::string("ref"));
    CHECK_EQ(Label(s, "$.Т"), std::string("open"));
    const Json root = Check(q + "Результат = В.Сумма;");
    CHECK_EQ(Verdict(root), std::string("safe"));
    CHECK_EQ(Label(root, "$"), std::string("open"));
    const Json col = Check(q + "Результат = Т.ВыгрузитьКолонку(\"Сумма\");");
    CHECK_EQ(Verdict(col), std::string("safe"));
    CHECK_EQ(Label(col, "$[*]"), std::string("open"));
    const Json map = Check(q + "Результат = Новый Соответствие; Результат.Вставить(\"к\", В.Сумма);");
    CHECK_EQ(Verdict(map), std::string("safe"));
    CHECK_EQ(Label(map, "$[*].value"), std::string("open"));
    // Joined with another cell on two paths: still a cell. Joined with a computed value: not.
    const Json joined = Check(q + "Если В.Сумма = 1 Тогда Х = В.Сумма; Иначе Х = Т[0].Сумма; КонецЕсли; Результат = Х;");
    CHECK_EQ(Label(joined, "$"), std::string("open"));
    const Json mixed = Check(q + "Если В.Сумма = 1 Тогда Х = В.Сумма; Иначе Х = В; КонецЕсли; Результат = Х;");
    CHECK_EQ(Verdict(mixed), std::string("confirm"));
    // Anything but a move keeps the old rule: the value may still be a reference whose presentation is read.
    for (const char* code : {"Результат = Строка(В.Сумма);", "Результат = \"x\" + В.Сумма;",
                             "М = Новый Массив; М.Добавить(В.Сумма); Результат = М[0];",
                             "Результат = Новый Структура(\"К\", Т.ВыгрузитьКолонку(\"Сумма\")[0]);"}) {
        CHECK_EQ(Verdict(Check(q + code)), std::string("confirm"));
    }
    // A protected column stays protected, a tainted holder taints the cell.
    const Json prot = Check(std::string(kQuery) + "В = З.Выполнить().Выбрать(); В.Следующий(); Результат = В.Имя;");
    CHECK_EQ(Label(prot, "$"), std::string("protected"));
}

TEST(CheckCode, MapIsAnArrayOfKeyValueEntries) {
    const Json j = Check(std::string(kQuery) + "Т = З.Выполнить().Выгрузить();\n"
                         "М = Новый Соответствие; М.Вставить(\"к\", Т[0].Имя); Результат = М;");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("container"));
    CHECK_EQ(Label(j, "$[*]"), std::string("container"));
    CHECK_EQ(Label(j, "$[*].key"), std::string("client"));
    CHECK_EQ(Label(j, "$[*].value"), std::string("protected"));
}

TEST(CheckCode, TaintedMapKeyNeedsConfirmation) {
    const Json j = Check(std::string(kQuery) + "Т = З.Выполнить().Выгрузить();\n"
                         "М = Новый Соответствие; М.Вставить(Т[0].Имя, 1); Результат = М;");
    CHECK_EQ(Verdict(j), std::string("confirm"));
}

TEST(CheckCode, DepthLimitOfTheSerializer) {
    // Six nested structures: the innermost one is at depth 5, its value at depth 6 is "<max_depth>" and is labelled unknown.
    const Json j = Check(
        "С1 = Новый Структура(\"а\", 1); С2 = Новый Структура(\"в\", С1); С3 = Новый Структура(\"в\", С2);\n"
        "С4 = Новый Структура(\"в\", С3); С5 = Новый Структура(\"в\", С4); Результат = Новый Структура(\"в\", С5);");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasReason(j, "C_UNLABELED_RESULT"));
    CHECK_EQ(Label(j, "$.в.в.в.в.в"), std::string("container"));
    CHECK_EQ(Label(j, "$.в.в.в.в.в.а"), std::string("unknown"));
}

TEST(CheckCode, StructureKeyThatIsNotAnIdentifierIsUnlabeled) {
    const Json j = Check("Результат = Новый Структура(\"А.Б\", 1);");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasReason(j, "C_UNLABELED_RESULT"));
}

TEST(CheckCode, ParseRefusalIsConfirmWithRefusedRisks) {
    const Json j = Check("Результат = Вычислить(\"1\");");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK_EQ(j["reasons"].size(), static_cast<size_t>(1));
    CHECK(!HasReason(j, "C_NOT_IMPLEMENTED"));
    CHECK(HasRisk(j, "R_UNKNOWN_EFFECT"));
    CHECK(j["canonical"].get<std::string>().empty());
    CHECK(j["risks"][0].contains("line") && j["risks"][0].contains("col"));
}

TEST(CheckCode, RiskForcesConfirmAndKeepsCanonical) {
    const Json j = Check("ЗаписьЖурналаРегистрации(\"x\"); Результат = 1;");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasRisk(j, "R_WRITE"));
    CHECK(!j["canonical"].get<std::string>().empty());
}

TEST(CheckCode, IncompleteListsMissingTables) {
    const Json j = Check("З = Новый Запрос(\"ВЫБРАТЬ Н.Х КАК Х ИЗ Справочник.Нет КАК Н\");\n"
                         "Результат = З.Выполнить().Выгрузить();");
    CHECK_EQ(Verdict(j), std::string("incomplete"));
    CHECK_EQ(j["missing_tables"][0].get<std::string>(), std::string("Справочник.Нет"));
    CHECK(j["canonical"].get<std::string>().empty());
}

TEST(CheckCode, BoundTokenIsPrintedAsHostValue) {
    const Json j = Check("Результат = \"[str-00001]\";", PolicyWithTokens());
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$"), std::string("protected"));
    const std::string canonical = j["canonical"].get<std::string>();
    CHECK(canonical.find("МСПЗначенияТокенов[0]") != std::string::npos);
    CHECK(canonical.find("str-00001") == std::string::npos);
}

TEST(CheckCode, QueryTextIsReplacedByItsCanonicalForm) {
    const Json j = Check("З = Новый Запрос(\"выбрать а.Имя как Имя из Справочник.А как а\");\n"
                         "Результат = З.Выполнить().Выгрузить();");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK(j["canonical"].get<std::string>().find("ВЫБРАТЬ") != std::string::npos);
    CHECK_EQ(Label(j, "$[*].Имя"), std::string("protected"));
}

TEST(CheckCode, ClientTextOnlyInCanonicalAndPaths) {
    // A string value the client wrote appears in the canonical text and nowhere else.
    const Json j = Check("Х = \"СекретноеЗначениеКлиента\"; Результат = Х;");
    CHECK_EQ(Verdict(j), std::string("safe"));
    Json rest = j;
    rest.erase("canonical");
    CHECK(rest.dump().find("СекретноеЗначениеКлиента") == std::string::npos);
    CHECK(j["canonical"].get<std::string>().find("СекретноеЗначениеКлиента") != std::string::npos);
}

TEST(CheckCode, NeverThrowsAndFailsClosed) {
    const Json j = Check("\xFF\xFE", "\xFF");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    const Json k = Check(std::string(3000000, 'x'));
    CHECK_EQ(Verdict(k), std::string("confirm"));
}

TEST(CheckCode, AnalysisFailureHasNoCanonical) {
    // An over-budget loop nest is C_LIMIT: the walk did not complete, nothing may be executed from this answer.
    std::string code = "Х = 0;\n";
    for (int i = 0; i < 40; ++i) code += "Пока Х < 5 Цикл\n";
    code += "Х = Х + 1;\n";
    for (int i = 0; i < 40; ++i) code += "КонецЦикла;\n";
    code += "Результат = Х;";
    const Json j = Check(code);
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(j["canonical"].get<std::string>().empty());
}

TEST(CheckCode, KeysDifferingOnlyInYoAreUnknown) {
    // ё and е fold into one field internally; the serialized spelling is not known, so the field is unknown.
    const Json j = Check("Результат = Новый Структура(\"Её\", 1); Результат.Вставить(\"Ее\", 2);");
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasReason(j, "C_UNLABELED_RESULT"));
    CHECK_EQ(Label(j, "$.Её"), std::string("unknown"));
    // A single spelling keeps its exact spelling in the path.
    const Json k = Check("Результат = Новый Структура(\"Её\", 1);");
    CHECK_EQ(Verdict(k), std::string("safe"));
    CHECK_EQ(Label(k, "$.Её"), std::string("client"));
}

TEST(CheckCode, InvalidBodyHasUnknownRoot) {
    lineage::bsl::Body body;
    body.stmts.push_back(std::make_shared<lineage::bsl::Stmt>());   // kind Unset: ValidateBody refuses it
    lineage::bsl::LabelState state;
    const auto r = lineage::bsl::AnalyzeCodeWalk(body, lineage::bsl::TypeCheckOptions(), state);
    CHECK_EQ(r.result_labels.size(), static_cast<size_t>(1));
    CHECK_EQ(r.result_labels[0].label, std::string("unknown"));
    CHECK(!r.result_reasons.empty());
}

// A parameter name literal no query text can spell (a space, an invisible or look-alike character, empty, a digit
// first) is refused: the host may trim or fold it onto the clean spelling and bind the protected value there. The
// column of the clean parameter never keeps the client label of its other binding.
TEST(CheckCode, ParamNameOutsideTheParameterAlphabetIsConfirm) {
    const std::pair<const char*, const char*> cases[] = {
        {"Пар", "Пар "},
        {"Пар", " Пар"},
        {"Пар", "Пар\xC2\xA0"},
        {"Пар", "Па\xE2\x80\x8Bр"},
        {"ПаВ", "Па\xE1\xB2\x80"},
        {"Sum", "\xC5\xBFum"},
        {"ID", "\xC4\xB1" "d"},
        {"k", "\xE2\x84\xAA"},
        {"Пар", ""},
        {"Пар", "1Пар"},
    };
    for (const auto& c : cases) {
        const std::string code = std::string(
            "З = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А\");\n"
            "В = З.Выполнить().Выбрать();\nВ.Следующий();\n"
            "З2 = Новый Запрос(\"ВЫБРАТЬ &") + c.first + " КАК Х\");\n"
            "З2.УстановитьПараметр(\"" + c.first + "\", 1);\n"
            "З2.УстановитьПараметр(\"" + c.second + "\", В.Имя);\n"
            "Результат = З2.Выполнить().Выгрузить();\n";
        const Json j = Check(code);
        CHECK_EQ(Verdict(j), std::string("confirm"));
        CHECK(HasReason(j, "C_BAD_PARAM_NAME"));
        CHECK(Label(j, "$[*].Х") != std::string("client"));
    }
}

// Mixed scripts inside the parameter alphabet are a legitimate parameter name (a query text naming it is refused by
// the query parser's own mixed-script rule, so the text below names another parameter).
TEST(CheckCode, MixedScriptParamNameIsSafe) {
    const Json j = Check(
        "З = Новый Запрос(\"ВЫБРАТЬ &Пар КАК Х\");\n"
        "З.УстановитьПараметр(\"Пар\", 1);\n"
        "З.УстановитьПараметр(\"ДатаНачалаUTC\", 1);\n"
        "Результат = З.Выполнить().Выгрузить();\n");
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK(!HasReason(j, "C_BAD_PARAM_NAME"));
    CHECK_EQ(Label(j, "$[*].Х"), std::string("client"));
}

namespace {

// A query reading the protected Имя, its first row in В, and a second query З2 (by default its only column is the
// parameter &Пар); `setters` follow, then З2's result is the Результат.
std::string LostQueryCode(const std::string& setters, const char* text = "\"ВЫБРАТЬ &Пар КАК Х\"") {
    return std::string(
               "З = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А\");\n"
               "В = З.Выполнить().Выбрать();\nВ.Следующий();\n"
               "З2 = Новый Запрос(") +
           text + ");\n" + setters + "Результат = З2.Выполнить().Выгрузить();\n";
}

}  // namespace

// The control of the tests below: the parameter bound once with a literal is a client column.
TEST(CheckCode, LiteralParameterIsClient) {
    const Json j = Check(LostQueryCode("З2.УстановитьПараметр(\"Пар\", 1);\n"));
    CHECK_EQ(Verdict(j), std::string("safe"));
    CHECK_EQ(Label(j, "$[*].Х"), std::string("client"));
}

// A parameter binding the walk cannot follow (a name that is not a literal, the Параметры collection, any member,
// method, index or iteration not modelled on Запрос, a wrong argument count, a reserved name) is a reason, and every
// column of that query's results is unknown: a confirmed run must not return the protected value under the client
// label of the literal binding.
TEST(CheckCode, UnfollowedParameterBindingMakesColumnsUnknown) {
    const std::pair<const char*, const char*> cases[] = {
        {"Н = \"Пар\";\nЗ2.УстановитьПараметр(Н, В.Имя);\n", "C_DYNAMIC_PARAM"},
        {"З2.УстановитьПараметр(\"Па\" + \"р\", В.Имя);\n", "C_DYNAMIC_PARAM"},
        {"З2.SetParameter(\"Па\" + \"р\", В.Имя);\n", "C_DYNAMIC_PARAM"},
        {"З2.Параметры[\"Пар\"] = В.Имя;\n", "C_UNKNOWN_MEMBER"},
        {"З2.Параметры.Пар = В.Имя;\n", "C_UNKNOWN_MEMBER"},
        {"З2.Параметры.Вставить(\"Пар\", В.Имя);\n", "C_UNKNOWN_MEMBER"},
        {"З2.Parameters.Insert(\"Пар\", В.Имя);\n", "C_UNKNOWN_MEMBER"},
        {"З2.УстановитьПараметр(\"Пар\", В.Имя, 1);\n", "C_BAD_ARGS"},
        {"З2.УстановитьПараметр(\"МСПТокен1\", В.Имя);\n", "C_RESERVED"},
        {"З2.НайтиПараметры();\n", "C_UNKNOWN_METHOD"},
        {"З2[\"Пар\"] = В.Имя;\n", "C_UNKNOWN_INDEX"},
        {"Т = З2[0];\n", "C_UNKNOWN_INDEX"},
        {"З2.Пар = В.Имя;\n", "C_UNKNOWN_MEMBER"},
        {"Т = З2.Текст;\n", "C_UNKNOWN_MEMBER"},
    };
    for (const auto& c : cases) {
        const Json j = Check(LostQueryCode(std::string("З2.УстановитьПараметр(\"Пар\", 1);\n") + c.first));
        CHECK_EQ(Verdict(j), std::string("confirm"));
        CHECK(HasReason(j, c.second));
        CHECK_EQ(Label(j, "$[*]"), std::string("container"));
        CHECK_EQ(Label(j, "$[*].Х"), std::string("unknown"));
    }
}

// The same for a query allocated without a text, a query executed before the binding the walk cannot follow (the
// fixpoint relabels the earlier result too), and a column read from a row of the result.
TEST(CheckCode, UnfollowedParameterBindingVariants) {
    const char* const head =
        "З = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А\");\n"
        "В = З.Выполнить().Выбрать();\nВ.Следующий();\n"
        "З2 = Новый Запрос(\"ВЫБРАТЬ &Пар КАК Х\");\nЗ2.УстановитьПараметр(\"Пар\", 1);\n";
    {
        const Json j = Check(LostQueryCode(
            "З2.Текст = \"ВЫБРАТЬ &Пар КАК Х\";\nЗ2.УстановитьПараметр(\"Пар\", 1);\nЗ2.Параметры.Пар = В.Имя;\n", ""));
        CHECK_EQ(Verdict(j), std::string("confirm"));
        CHECK_EQ(Label(j, "$[*].Х"), std::string("unknown"));
    }
    {
        const Json j = Check(std::string(head) +
                             "Р = З2.Выполнить().Выгрузить();\nЗ2.Параметры.Пар = В.Имя;\nРезультат = Р;\n");
        CHECK_EQ(Verdict(j), std::string("confirm"));
        CHECK_EQ(Label(j, "$[*].Х"), std::string("unknown"));
    }
    {
        const Json j = Check(std::string(head) +
                             "З2.Параметры.Пар = В.Имя;\nВ2 = З2.Выполнить().Выбрать();\nВ2.Следующий();\n"
                             "Результат = Новый Структура(\"К\", В2.Х);\n");
        CHECK_EQ(Verdict(j), std::string("confirm"));
        CHECK_EQ(Label(j, "$.К"), std::string("unknown"));
    }
}

// A query whose text or object the walk loses (a text that is not constant-folded, a copy into another variable, a
// collection or an argument) has no known columns: the whole result is unknown.
TEST(CheckCode, LostQueryTextOrObjectMakesResultUnknown) {
    const std::pair<const char*, const char*> cases[] = {
        {"Т = \"ВЫБРАТЬ А.Имя КАК Х ИЗ Справочник.А КАК А\";\nЗ2.Текст = Т;\n", "C_DYNAMIC_QUERY"},
        {"З3 = З2;\nЗ3.УстановитьПараметр(\"Пар\", В.Имя);\n", "C_QUERY_ESCAPES"},
        {"С = Новый Структура(\"З\", З2);\n", "C_QUERY_ESCAPES"},
        {"М = Новый Массив;\nМ.Добавить(З2);\n", "C_QUERY_ESCAPES"},
        {"МойМодуль.Заполнить(З2, В.Имя);\n", "C_QUERY_ESCAPES"},
    };
    for (const auto& c : cases) {
        const Json j = Check(LostQueryCode(std::string("З2.УстановитьПараметр(\"Пар\", 1);\n") + c.first));
        CHECK_EQ(Verdict(j), std::string("confirm"));
        CHECK(HasReason(j, c.second));
        CHECK_EQ(Label(j, "$"), std::string("unknown"));
        CHECK_EQ(Label(j, "$[*].Х"), std::string("<none>"));
    }
}

// Two bindings whose names differ only in yo / ie are one binding to the code checker, whichever the platform does:
// its label joins both values, so the column is not client.
TEST(CheckCode, YoVariantParameterBindingsJoin) {
    const Json j = Check(LostQueryCode("З2.УстановитьПараметр(\"Еж\", 1);\nЗ2.УстановитьПараметр(\"Ёж\", В.Имя);\n",
                                       "\"ВЫБРАТЬ &Еж КАК Х\""));
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK_EQ(Label(j, "$[*].Х"), std::string("unknown"));
}

// "params" spellings differing only in yo / ie with different origins make the policy malformed for the code checker
// too; the same origin is fine.
TEST(CheckCode, YoCollidingParamOriginsArePolicyInvalid) {
    Json policy = Json::parse(kPolicyABJson);
    policy["params"] = Json::parse(R"({"Еж": "client", "Ёж": "token_protected"})");
    const Json j = Check("Результат = 1;", policy.dump());
    CHECK_EQ(Verdict(j), std::string("confirm"));
    CHECK(HasReason(j, "C_POLICY_INVALID"));
    policy["params"] = Json::parse(R"({"Еж": "client", "ёж": "client"})");
    const Json same = Check("Результат = 1;", policy.dump());
    CHECK_EQ(Verdict(same), std::string("safe"));
}
