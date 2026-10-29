#include "test_framework.h"
#include "analysis_helpers.h"
#include "policy_fixture.h"
#include "bsl/bsl_canonical.h"

#include <memory>
#include <string>

using namespace lineage::bsl;
using namespace lineage::strict;

namespace {

void Dump(const std::string& code, const CodeAnalysis& r) {
    std::cout << "  refused: " << code << std::endl << "   ";
    for (const CodeReason& reason : r.reasons) {
        std::cout << " " << reason.code << "@" << reason.pos.line << ":" << reason.pos.col;
    }
    std::cout << std::endl;
}

bool Safe(const std::string& code, const TokenTable& tokens = TokenTable()) {
    const auto r = CheckCodeForTest(kPolicyAB, code, tokens);
    if (r.verdict != CodeVerdict::Safe) Dump(code, r);
    return r.verdict == CodeVerdict::Safe && r.reasons.empty();
}

bool Denied(const std::string& code, const char* reason, const TokenTable& tokens = TokenTable()) {
    const auto r = CheckCodeForTest(kPolicyAB, code, tokens);
    const bool ok = r.verdict == CodeVerdict::Confirm && HasCodeReason(r, reason);
    if (!ok) Dump(code, r);
    return ok;
}

// Columns С (an open reference to Справочник.А) and Имя (protected), as CheckQuery labels them under kPolicyAB.
const char* kQ = "З = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С, А.Имя КАК Имя ИЗ Справочник.А КАК А\");\n";

}  // namespace

TEST(BslTaint, UnknownIdentifiersAndReceivers) {
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Результат = ЭтотОбъект;"), "C_UNKNOWN_IDENT"));
    // The parser already refuses the reserved name, with its own code (the plan expected C_UNKNOWN_IDENT here).
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Результат = МСПЗначенияТокенов[0];"), "C_RESERVED"));
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Х = ПолучитьОбъект(); Результат = Х.Имя;"), "C_UNKNOWN_IDENT"));
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Результат = ОбщегоНазначения.Ч();"), "C_COMMON_MODULE"));
}

TEST(BslTaint, ContextObjectsAreNotModules) {
    CHECK(Denied("Результат = ЭтотОбъект.Метод();", "C_UNKNOWN_IDENT"));
    CHECK(!HasCodeReason(CheckCodeForTest(kPolicyAB, "Результат = ЭтаФорма.Метод();"), "C_COMMON_MODULE"));
    CHECK(Denied("ЭтотОбъект = 1; Результат = 1;", "C_UNKNOWN_IDENT"));
    CHECK(Denied("Результат = Код;", "C_UNKNOWN_IDENT"));
}

TEST(BslTaint, AcceptsLegitimateQueryFlow) {
    const std::string code = std::string(kQ) +
        "Т = З.Выполнить().Выгрузить();\n"
        "М = Новый Массив;\n"
        "Для Каждого Стр Из Т Цикл М.Добавить(Новый Структура(\"С, Имя\", Стр.С, Стр.Имя)); КонецЦикла;\n"
        "Результат = М;";
    CHECK(Safe(code));
    const std::string sel = std::string(kQ) +
        "В = З.Выполнить().Выбрать(); М = Новый Массив;\n"
        "Пока В.Следующий() Цикл М.Добавить(В.Имя); КонецЦикла; Результат = М;";
    CHECK(Safe(sel));
    CHECK(Safe("З = Новый Запрос; З.Текст = \"ВЫБРАТЬ 1\" + \" КАК А\"; З.УстановитьПараметр(\"П\", 1);\n"
               "Результат = З.Выполнить().Пустой();"));
}

TEST(BslTaint, ColumnsNeedKnownColumnSet) {
    // Without labels from the query check (here: the text does not parse) a column read is refused, not guessed.
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ\"); В = З.Выполнить().Выбрать(); Результат = В.Имя;", "C_UNKNOWN_MEMBER"));
    CHECK(Denied(std::string(kQ) + "В = З.Выполнить().Выбрать(); Результат = В.Нет;", "C_UNKNOWN_MEMBER"));
    CHECK(Denied(std::string(kQ) + "Т = З.Выполнить().Выгрузить(); Результат = Т[0].Нет;", "C_UNKNOWN_MEMBER"));
    CHECK(Safe(std::string(kQ) + "Т = З.Выполнить().Выгрузить(); Результат = Т[0].Имя;"));
    CHECK(Safe(std::string(kQ) + "В = З.Выполнить().Выбрать(); Результат = В.иМЯ;"));
}

TEST(BslTaint, ReferenceMethodsOnlyOnReferenceColumns) {
    const std::string pre = std::string(kQ) + "Т = З.Выполнить().Выгрузить(); Р = Т[0];\n";
    CHECK(Safe(pre + "Результат = Р.С.УникальныйИдентификатор();"));
    CHECK(Safe(pre + "Результат = Р.С.Метаданные().ПолноеИмя();"));
    CHECK(Safe(pre + "Результат = Р.С.Пустая();"));
    CHECK(Denied(pre + "Результат = Р.Имя.УникальныйИдентификатор();", "C_UNKNOWN_RECEIVER"));
    CHECK(Denied(pre + "Результат = Р.С.ПолучитьОбъект();", "C_UNKNOWN_METHOD"));
    CHECK(Denied(pre + "Результат = Р.С.Метаданные().Имя();", "C_UNKNOWN_METHOD"));
}

TEST(BslTaint, MethodsOnUnknownAreDenied) {
    CHECK(Denied("М = Новый Массив; Х = М.Получить(0); Результат = Х.Метод();", "C_UNKNOWN_RECEIVER"));
    CHECK(Denied("С = Новый Структура(\"К\", 1); Х = С[\"К\"]; Результат = Х.Выполнить();", "C_UNKNOWN_RECEIVER"));
    CHECK(Denied("Результат = Неопределено.Метод();", "C_UNKNOWN_METHOD"));
    CHECK(Denied("Х = 1; Результат = Х.Метод();", "C_UNKNOWN_METHOD"));
    CHECK(Denied("Х = ПолучитьОбъект(); Результат = Х[0];", "C_UNKNOWN_RECEIVER"));
}

TEST(BslTaint, EvalAndHandlersAsMembersAreDenied) {
    CHECK(Denied("М = Новый Массив; Результат = М.Вычислить(\"1\");", "C_EVAL"));
    CHECK(Denied("М = Новый Массив; Результат = М.Eval(\"1\");", "C_EVAL"));
    CHECK(Denied("М = Новый Массив; М.Выполнить(); Результат = 1;", "C_UNKNOWN_METHOD"));
    CHECK(Denied("З = Новый Запрос; Результат = З.Вычислить(\"1\");", "C_EVAL"));
    CHECK(Denied("Х = ПолучитьОбъект(); Результат = Х.Вычислить(\"1\");", "C_EVAL"));
}

TEST(BslTaint, StringNamedCallbacksAndGlobalsAreDenied) {
    CHECK(Denied("О = Новый ОписаниеОповещения(\"Имя\", ЭтотОбъект); Результат = 1;", "C_UNKNOWN_TYPE"));
    CHECK(Denied("ПодключитьОбработчикОжидания(\"Имя\", 1); Результат = 1;", "C_UNKNOWN_IDENT"));
    CHECK(Denied("Результат = Новый HTTPСоединение(\"x\");", "C_PARSE"));
    CHECK(Denied("Результат = Новый ЧтениеТекста;", "C_UNKNOWN_TYPE"));
    CHECK(Denied("Результат = Справочники.А.НайтиПоКоду(\"1\");", "C_UNKNOWN_IDENT"));
}

TEST(BslTaint, NewTypeAllowlist) {
    for (const char* ok : {"Результат = Новый Массив;", "Результат = Новый Структура;", "Результат = Новый Соответствие;",
                           "Результат = Новый ТаблицаЗначений;", "Результат = Новый СписокЗначений;",
                           "Результат = Новый УникальныйИдентификатор;", "Результат = Новый Структура(\"А\", 1);"})
        CHECK(Safe(ok));
    CHECK(Denied("Результат = Новый Запрос(\"а\", \"б\");", "C_BAD_ARGS"));
}

TEST(BslTaint, YoIsEquivalentToYe) {
    // ё and е are one letter in allowlist keys and in variable names.
    CHECK(Safe("Х = Новый Массив; Х.добавить(1); Результат = Х.Количество();"));
    CHECK(Safe("Еж = Новый Массив; Результат = еж.Количество();"));
    CHECK(Denied("Ёж = Новый Массив; Результат = Еж.Количество();", "C_AMBIGUOUS_IDENT"));
}

TEST(BslTaint, ReassignmentAlongBranchesLosesTheType) {
    CHECK(Denied("Если Истина Тогда Х = Новый Массив; Иначе Х = Новый Структура; КонецЕсли; Х.Добавить(1); Результат = 1;",
                 "C_UNKNOWN_RECEIVER"));
    CHECK(Safe("Если Истина Тогда Х = Новый Массив; Иначе Х = Новый Массив; КонецЕсли; Х.Добавить(1); Результат = 1;"));
    // Assigned on one path only: may be a module variable on the other.
    CHECK(Denied("Если Истина Тогда Х = Новый Массив; КонецЕсли; Результат = Х.Количество();", "C_UNKNOWN_IDENT"));
    CHECK(Denied("Попытка Х = Новый Массив; Исключение Х = 1; КонецПопытки; Х.Добавить(1); Результат = 1;",
                 "C_UNKNOWN_RECEIVER"));
    CHECK(Denied("Х = Новый Массив; Попытка Х = Новый Структура; Исключение Х.Добавить(1); КонецПопытки; Результат = 1;",
                 "C_UNKNOWN_RECEIVER"));
}

TEST(BslTaint, LoopCarriedTypesAreUnknown) {
    // The first iteration sees a Запрос, the second the Массив from the first.
    CHECK(Denied("Х = Новый Запрос; Для Н = 1 По 3 Цикл Х.Выполнить(); Х = Новый Массив; КонецЦикла; Результат = 1;",
                 "C_UNKNOWN_RECEIVER"));
    CHECK(Denied("Х = Новый Массив; Пока Ложь Цикл Х = 1; КонецЦикла; Х.Добавить(1); Результат = 1;", "C_UNKNOWN_RECEIVER"));
    // Reassigned before use in the same iteration: fine.
    CHECK(Safe("Для Н = 1 По 3 Цикл Х = Новый Массив; Х.Добавить(Н); КонецЦикла; Результат = 1;"));
    // The loop variable after the loop may be unassigned (zero iterations).
    CHECK(Denied("Для Н = 1 По 3 Цикл КонецЦикла; Результат = Н;", "C_UNKNOWN_IDENT"));
    CHECK(Safe("М = Новый Массив; Для Каждого Э Из М Цикл Результат = 1; КонецЦикла;"));
}

TEST(BslTaint, ShortCircuitOutParameter) {
    CHECK(Safe("С = Новый Структура(\"К\", 1); Если С.Свойство(\"К\", Х) Тогда Результат = Х; Иначе Результат = 0; КонецЕсли;"));
    // Х set only if the left side is true: unassigned on the other path.
    CHECK(Denied("С = Новый Структура; Л = Истина; Если Л И С.Свойство(\"К\", Х) Тогда Результат = 1; КонецЕсли; Р = Х;",
                 "C_UNKNOWN_IDENT"));
    CHECK(Denied("С = Новый Структура; Если С.Свойство(\"К\", 5) Тогда Результат = 1; КонецЕсли;", "C_BAD_ARGS"));
}

TEST(BslTaint, QueryTextRules) {
    CHECK(Safe("З = Новый Запрос; З.Текст = \"ВЫБРАТЬ 1\"; Результат = 1;"));
    CHECK(Denied("Т = \"ВЫБРАТЬ 1\"; З = Новый Запрос; З.Текст = Т + \"\"; Результат = 1;", "C_DYNAMIC_QUERY"));
    CHECK(Denied("Т = \"ВЫБРАТЬ 1\"; З = Новый Запрос(Т); Результат = 1;", "C_DYNAMIC_QUERY"));
    CHECK(Denied("З = Новый Запрос; З.Параметры = 1; Результат = 1;", "C_UNKNOWN_MEMBER"));
    CHECK(Denied("З = Новый Запрос; Н = \"П\"; З.УстановитьПараметр(Н, 1); Результат = 1;", "C_DYNAMIC_PARAM"));
    CHECK(Denied("З = Новый Запрос; Результат = З.Текст;", "C_UNKNOWN_MEMBER"));
    const auto r = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ 1\"); З.Текст = \"ВЫБРАТЬ 2\" + \" \" + \"КАК А\"; Результат = 1;");
    CHECK_EQ(r.query_texts.size(), static_cast<size_t>(2));
    CHECK_EQ(r.query_texts[0].text, std::string("ВЫБРАТЬ 1"));
    CHECK_EQ(r.query_texts[1].text, std::string("ВЫБРАТЬ 2 КАК А"));
    CHECK(r.query_texts[0].site == r.query_texts[1].site);
}

TEST(BslTaint, QueryObjectDoesNotEscape) {
    // A copy would let a text change reach the object the checker never saw.
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ 1\"); Д = З; Д.Текст = \"ВЫБРАТЬ 2\"; Результат = З.Выполнить();",
                 "C_QUERY_ESCAPES"));
    CHECK(Denied("З = Новый Запрос; М = Новый Массив; М.Добавить(З); Результат = 1;", "C_QUERY_ESCAPES"));
    CHECK(Denied("З = Новый Запрос; Результат = З;", "C_QUERY_ESCAPES"));
    CHECK(Denied("М = Новый Массив(Новый Запрос(\"ВЫБРАТЬ 1\")); Результат = 1;", "C_QUERY_ESCAPES"));
    CHECK(Denied("Результат = Строка(Новый Запрос(\"ВЫБРАТЬ 1\"));", "C_QUERY_ESCAPES"));
    CHECK(Safe("З = Новый Запрос(\"ВЫБРАТЬ 1\"); Результат = З.Выполнить().Пустой();"));
}

TEST(BslTaint, ContainerRules) {
    CHECK(Safe("С = Новый Структура; С.Вставить(\"К\", 1); Результат = С[\"К\"];"));
    CHECK(Safe("С = Новый Соответствие; С.Вставить(1, 2); Результат = С.Получить(1);"));
    CHECK(Denied("С = Новый Структура; К = \"К\"; Результат = С[К];", "C_DYNAMIC_KEY"));
    CHECK(Denied("С = Новый Структура; К = \"К\"; Результат = С.Свойство(К);", "C_DYNAMIC_KEY"));
    CHECK(Denied("С = Новый Структура(\"К\", 1); Результат = С.К;", "C_UNKNOWN_MEMBER"));
    CHECK(Safe("М = Новый Массив; М.Добавить(1); М[0] = 2; Результат = М[0];"));
    CHECK(Denied("М = Новый Массив; М.Очистить(); Результат = 1;", "C_UNKNOWN_METHOD"));
    CHECK(Denied("С = Новый Структура; Для Каждого Э Из С Цикл КонецЦикла; Результат = 1;", "C_UNKNOWN_ITER"));
    CHECK(Denied("Для Каждого Э Из Х Цикл КонецЦикла; Результат = 1;", "C_UNKNOWN_IDENT"));
}

TEST(BslTaint, GlobalFunctionAllowlist) {
    for (const char* ok : {"Результат = ЗначениеЗаполнено(1);", "Результат = ТипЗнч(1);", "Результат = Тип(\"Строка\");",
                           "Результат = Строка(1);", "Результат = Число(\"1\");", "Результат = Формат(1, \"ЧГ=0\");",
                           "Результат = Макс(1, 2);", "Результат = Окр(1.5, 0);", "Результат = Лев(\"abc\", 1);",
                           "Результат = СтрДлина(\"abc\") + 1;", "Результат = ValueIsFilled(1);"})
        CHECK(Safe(ok));
    for (const char* bad : {"Результат = Сообщить(\"x\");", "Результат = УстановитьПривилегированныйРежим(Истина);",
                            "Результат = СтрНайти(\"a\", \"b\");", "Результат = ОписаниеОшибки();"})
        CHECK(Denied(bad, "C_UNKNOWN_IDENT"));
    CHECK(Denied("Н = \"Строка\"; Результат = Тип(Н);", "C_DYNAMIC_KEY"));
}

TEST(BslTaint, ScalarArithmeticTypes) {
    CHECK(Safe("Х = 1 + 2; Результат = Х;"));
    CHECK(Denied("Х = 1 + 2; Результат = Х.Метод();", "C_UNKNOWN_METHOD"));
    CHECK(Safe("Результат = ?(Истина, 1, 2);"));
    CHECK(Denied("Результат = ?(Истина, Новый Массив, 2).Количество();", "C_UNKNOWN_RECEIVER"));
}

// ---- taint propagation, implicit flow, query integration, bound tokens ----

TEST(BslTaint, ImplicitFlowOnProtectedCondition) {
    auto r = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А\"); В = З.Выполнить().Выбрать(); Н = 0;\n"
        "Пока В.Следующий() Цикл Если В.Имя = \"x\" Тогда Н = Н + 1; КонецЕсли; КонецЦикла; Результат = Н;");
    CHECK(HasCodeReason(r, "C_IMPLICIT_FLOW"));
}

TEST(BslTaint, CharCodesOfProtectedNeedConfirmation) {
    auto r = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А\"); Т = З.Выполнить().Выгрузить();\n"
        "Результат = КодСимвола(Т[0].Имя, 1);");
    CHECK(HasCodeReason(r, "C_FUNC_OVER_PROTECTED"));
}

TEST(BslTaint, MovingProtectedIntoResultIsSafe) {
    auto r = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С, А.Имя КАК Имя ИЗ Справочник.А КАК А\"); Т = З.Выполнить().Выгрузить();\n"
        "М = Новый Массив; Для Каждого Стр Из Т Цикл М.Добавить(Новый Структура(\"С, Имя\", Стр.С, Стр.Имя)); КонецЦикла;\n"
        "Результат = М;");
    CHECK(r.verdict == CodeVerdict::Safe);
}

TEST(BslTaint, DynamicQueryTextNeedsConfirmation) {
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Т = \"ВЫБРАТЬ 1\"; З = Новый Запрос; З.Текст = Т + \"\"; Результат = 1;"),
                        "C_DYNAMIC_QUERY"));
}

namespace {

// kQ plus a value table Т of its rows.
std::string WithTable(const std::string& rest) {
    return std::string(kQ) + "Т = З.Выполнить().Выгрузить();\n" + rest;
}

TokenTable OneToken() {
    TokenTable t;
    TokenBinding b;
    b.index = 0;
    b.origin = Origin::ProtectedDirect;
    t["[STR-00001]"] = b;
    return t;
}

// Справочник.А with an open presentation: a reference may then be read as a string.
const FieldPolicy kPolicyOpenPresentation = ParsePolicyOrDie(R"({
  "version": 1,
  "tables": {
    "Справочник.А": {
      "presentation": "open",
      "fields": {"Имя": {"class": "protected"}, "Ссылка": {"class": "open", "refs": ["Справочник.А"]}}
    }
  }
})");

}  // namespace

TEST(BslTaint, OperatorsOverProtectedNeedConfirmation) {
    // A comparison is a dictionary oracle even without a branch.
    CHECK(Denied(WithTable("Результат = Т[0].Имя = \"Иванов\";"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("Результат = Т[0].Имя + \"x\";"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("Итог = 0; Итог = Итог + Т[0].Имя; Результат = Итог;"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("Результат = Не Т[0].Имя;"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("Результат = ЗначениеЗаполнено(Т[0].Имя);"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("Результат = ТипЗнч(Т[0].Имя);"), "C_FUNC_OVER_PROTECTED"));
    // Comparing open references is fine.
    CHECK(Safe(WithTable("Результат = Т[0].С = Т[0].С;")));
}

TEST(BslTaint, ReferencesReadAsStringsUsePresentation) {
    CHECK(Denied(WithTable("Результат = Строка(Т[0].С);"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("Результат = \"x\" + Т[0].С;"), "C_FUNC_OVER_PROTECTED"));
    const auto open = CheckCodeForTest(kPolicyOpenPresentation, WithTable("Результат = Строка(Т[0].С) + \"x\";"));
    CHECK(open.verdict == CodeVerdict::Safe);
    // An element of unknown type may be a reference whose targets are unknown.
    CHECK(Denied(WithTable("М = Т.ВыгрузитьКолонку(\"С\"); Х = М[0]; М2 = Новый Массив; М2.Добавить(Х);\n"
                           "Результат = Строка(М2[0]);"), "C_FUNC_OVER_PROTECTED"));
    CHECK(Safe("М = Новый Массив; М.Добавить(1); Результат = Строка(М[0]);"));
}

TEST(BslTaint, KeysAndIndexesOverProtected) {
    CHECK(Denied(WithTable("С = Новый Соответствие; С.Вставить(Т[0].Имя, 1); Результат = С.Получить(\"Иванов\");"),
                 "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("С = Новый Соответствие; С.Вставить(\"Иванов\", 1); Результат = С.Получить(Т[0].Имя);"),
                 "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("С = Новый Соответствие; С.Вставить(Т[0].Имя, 1); Результат = С.Количество();"),
                 "C_FUNC_OVER_PROTECTED"));
    CHECK(Denied(WithTable("С = Новый Структура; С.Вставить(Т[0].Имя, 1); Результат = 1;"), "C_TAINTED_KEY"));
    CHECK(Denied(WithTable("М = Новый Массив; Результат = М[Т[0].Имя];"), "C_FUNC_OVER_PROTECTED"));
    // Protected values stored under client keys and read back by those keys are only moved.
    CHECK(Safe(WithTable("С = Новый Соответствие; С.Вставить(\"К\", Т[0].Имя); С.Вставить(Т[0].С, Т[0].Имя);\n"
                         "Результат = С.Получить(\"К\");")));
}

TEST(BslTaint, ImplicitFlowChannels) {
    CHECK(Denied(WithTable("Н = 0; Если Ложь Тогда Н = 1; ИначеЕсли Т[0].Имя = \"x\" Тогда Н = 2; КонецЕсли; Результат = Н;"),
                 "C_IMPLICIT_FLOW"));
    CHECK(Denied(WithTable("Результат = ?(Т[0].Имя = \"x\", 1, 0);"), "C_IMPLICIT_FLOW"));
    CHECK(Denied(WithTable("С = Новый Структура; Л = Т[0].Имя = \"x\" И С.Свойство(\"К\", Х); Результат = 1;"),
                 "C_IMPLICIT_FLOW"));
    CHECK(Denied(WithTable("Н = 0; Пока Т[0].Имя = \"x\" Цикл Н = 1; Прервать; КонецЦикла; Результат = Н;"),
                 "C_IMPLICIT_FLOW"));
    CHECK(Denied(WithTable("М = Т.ВыгрузитьКолонку(\"Имя\"); Для Каждого Э Из М Цикл Если Э = \"x\" Тогда Н = 1; КонецЕсли;\n"
                           "КонецЦикла; Результат = 1;"), "C_IMPLICIT_FLOW"));
    // A Попытка body that operates on protected data: the Исключение block depends on it.
    CHECK(Denied(WithTable("Попытка Х = Число(Т[0].Имя); Исключение Х = 0; КонецПопытки; Результат = Х;"), "C_IMPLICIT_FLOW"));
    CHECK(Safe("Попытка Х = 1; Исключение Х = 0; КонецПопытки; Результат = Х;"));
}

TEST(BslTaint, PcTaintsAssignmentsAndMutations) {
    const auto r = CheckCodeForTest(kPolicyAB,
        WithTable("Н = 0; М = Новый Массив; Если Т[0].Имя = \"x\" Тогда Н = 1; М.Добавить(1); КонецЕсли;\n"
                  "Результат = Новый Структура(\"Н, М\", Н, М.Количество());"));
    CHECK(HasCodeReason(r, "C_IMPLICIT_FLOW"));
    CHECK(IsTainted(r.result_label));
}

TEST(BslTaint, AliasesShareCollectionContents) {
    // Д and М are one array: data added through Д is seen through М.
    CHECK(Denied(WithTable("М = Новый Массив; Д = М; Д.Добавить(Т[0].Имя);\n"
                           "Для Каждого Э Из М Цикл Если Э = \"x\" Тогда Н = 1; КонецЕсли; КонецЦикла; Результат = 1;"),
                 "C_IMPLICIT_FLOW"));
    // Результат taken before the mutation still carries it.
    const auto r = CheckCodeForTest(kPolicyAB, WithTable("М = Новый Массив; Результат = М; М.Добавить(Т[0].Имя);"));
    CHECK(r.verdict == CodeVerdict::Safe);
    CHECK_EQ(r.result_label, Origin::ProtectedDirect);
    // A collection nested in another one, mutated afterwards.
    const auto n = CheckCodeForTest(kPolicyAB,
        WithTable("В = Новый Массив; М = Новый Массив; М.Добавить(В); В.Добавить(Т[0].Имя); Результат = М;"));
    CHECK_EQ(n.result_label, Origin::ProtectedDirect);
    CHECK_EQ(CheckCodeForTest(kPolicyAB, "Результат = Новый Структура(\"К\", \"x\");").result_label, Origin::Client);
}

TEST(BslTaint, CountersAndOpenAccumulationStaySafe) {
    CHECK(Safe(WithTable("Н = 0; Для Каждого Стр Из Т Цикл Н = Н + 1; КонецЦикла;\n"
                         "Для Инд = 0 По Т.Количество() - 1 Цикл Н = Н + 1; КонецЦикла; Результат = Н;")));
    CHECK(Safe("З = Новый Запрос(\"ВЫБРАТЬ Б.Сумма КАК Сумма ИЗ Справочник.Б КАК Б\"); Т = З.Выполнить().Выгрузить();\n"
               "Итог = 0; Для Каждого Стр Из Т Цикл Итог = Итог + Стр.Сумма; КонецЦикла; Результат = Итог;"));
    CHECK(Safe(WithTable("В = З.Выполнить().Выбрать(); Н = 0; Пока В.Следующий() Цикл Если В.С = Т[0].С Тогда Н = Н + 1;\n"
                         "КонецЕсли; КонецЦикла; Результат = Н;")));
}

TEST(BslTaint, LoopHeadJoinsEveryAssignment) {
    // Продолжить skips the later assignment: the value from the first branch reaches the next iteration.
    CHECK(Denied("Х = Новый Массив; Для Н = 1 По 3 Цикл Х.Добавить(1); Если Н = 2 Тогда Х = Новый Структура; Продолжить;\n"
                 "КонецЕсли; Х = Новый Массив; КонецЦикла; Результат = 1;", "C_UNKNOWN_RECEIVER"));
    CHECK(Safe("Х = Новый Массив; Для Н = 1 По 3 Цикл Х.Добавить(Н); Х = Новый Массив; КонецЦикла; Результат = Х.Количество();"));
}

TEST(BslTaint, DereferenceUsesFieldPolicy) {
    CHECK(Safe(WithTable("Результат = Т[0].С.Имя;")));
    CHECK(Safe(WithTable("Результат = Т[0].С.Ссылка.УникальныйИдентификатор();")));
    CHECK(Denied(WithTable("Если Т[0].С.Код = \"1\" Тогда Н = 1; КонецЕсли; Результат = 1;"), "C_IMPLICIT_FLOW"));
    CHECK(Denied(WithTable("Результат = Т[0].С.Нет;"), "C_UNKNOWN_MEMBER"));
    const auto r = CheckCodeForTest(kPolicyAB, WithTable("Результат = Т[0].С.Имя;"));
    CHECK_EQ(r.result_label, Origin::ProtectedDirect);
}

TEST(BslTaint, QueryParametersFromCode) {
    const std::string q2 = "З2 = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &П\");\n";
    // A client value compared with a protected field: the dictionary oracle of the query path.
    CHECK(Denied(q2 + "З2.УстановитьПараметр(\"П\", \"Иванов\"); Результат = З2.Выполнить().Выгрузить();",
                 "Q_EQ_CLIENT_PARAM"));
    // A protected value from another query, or an issued token, is compared with protected data: fine.
    CHECK(Safe(WithTable(q2 + "З2.УстановитьПараметр(\"П\", Т[0].Имя); Результат = З2.Выполнить().Выгрузить();")));
    CHECK(Safe(q2 + "З2.УстановитьПараметр(\"П\", \"[STR-00001]\"); Результат = З2.Выполнить().Выгрузить();", OneToken()));
    // Bound on one path to a client value: the join is mixed.
    CHECK(!Safe(WithTable(q2 + "Если Истина Тогда З2.УстановитьПараметр(\"П\", Т[0].Имя); Иначе З2.УстановитьПараметр(\"П\", \"x\");\n"
                               "КонецЕсли; Результат = З2.Выполнить().Выгрузить();")));
    // Never set: the host's "params" do not apply to code.
    CHECK(!Safe(q2 + "Результат = З2.Выполнить().Выгрузить();"));
    // The token parameter namespace is reserved.
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ 1 КАК А\"); З.УстановитьПараметр(\"МСПТокен1\", 1); Результат = 1;", "C_RESERVED"));
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ &мспТокен1 КАК А\"); Результат = З.Выполнить().Выгрузить();", "C_RESERVED"));
}

TEST(BslTaint, SeveralTextsPerSiteTakeTheWorstColumn) {
    const std::string one = "З = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А\");\n";
    const std::string use = "Т = З.Выполнить().Выгрузить(); Если Т[0].С = Неопределено Тогда Н = 1; КонецЕсли; Результат = 1;";
    CHECK(Safe(one + use));
    CHECK(Denied(one + "Если Истина Тогда З.Текст = \"ВЫБРАТЬ А.Имя КАК С ИЗ Справочник.А КАК А\"; КонецЕсли;\n" + use,
                 "C_IMPLICIT_FLOW"));
    // Different column names: no column is known.
    CHECK(Denied(one + "З.Текст = \"ВЫБРАТЬ А.Ссылка КАК Д ИЗ Справочник.А КАК А\"; Т = З.Выполнить().Выгрузить();\n"
                       "Результат = Т[0].С;", "C_UNKNOWN_MEMBER"));
}

TEST(BslTaint, QueryReasonsCanonicalAndMissingTables) {
    const auto ok = CheckCodeForTest(kPolicyAB, WithTable("Результат = Т;"));
    CHECK(ok.verdict == CodeVerdict::Safe);
    CHECK_EQ(ok.query_canonical.size(), static_cast<size_t>(1));
    CHECK(ok.query_canonical.begin()->first == ok.query_texts[0].text_expr);
    CHECK(!ok.query_canonical.begin()->second.empty());
    const auto missing = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ Н.Ссылка КАК С ИЗ Справочник.Нет КАК Н\"); Результат = З.Выполнить().Выгрузить();");
    CHECK(missing.verdict == CodeVerdict::Confirm);
    CHECK_EQ(missing.missing_tables.count("Справочник.Нет"), static_cast<size_t>(1));
    // A query reason is reported at the text's position in the code.
    const auto bad = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = \"\"x\"\"\"); Результат = 1;");
    CHECK(HasCodeReason(bad, "Q_EQ_LITERAL"));
}

TEST(BslTaint, BoundTokens) {
    const auto r = CheckCodeForTest(kPolicyAB, "Результат = \"[STR-00001]\";", OneToken());
    CHECK(r.verdict == CodeVerdict::Safe);
    CHECK_EQ(r.bound_tokens.size(), static_cast<size_t>(1));
    CHECK_EQ(r.bound_tokens.begin()->second, 0);
    CHECK_EQ(r.result_label, Origin::ProtectedDirect);
    CHECK_EQ(CheckCodeForTest(kPolicyAB, "Результат = \"[str-00001]\";", OneToken()).bound_tokens.size(), static_cast<size_t>(1));
    CHECK(Denied("Результат = \"x[STR-00001]\";", "C_TOKEN_IN_STRING", OneToken()));
    CHECK(Denied("Результат = \"[STR-00002]\";", "C_TOKEN_UNKNOWN", OneToken()));
    CHECK(Denied("Результат = \"[STR-00001]\";", "C_TOKEN_UNKNOWN"));
    CHECK(Denied("С = Новый Структура(\"[STR-00001]\", 1); Результат = С;", "C_TOKEN_AS_NAME", OneToken()));
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ 1 КАК А\"); З.УстановитьПараметр(\"[STR-00001]\", 1); Результат = 1;",
                 "C_TOKEN_AS_NAME", OneToken()));
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ \"\"[STR-00001]\"\" КАК А\"); Результат = З.Выполнить().Выгрузить();",
                 "C_TOKEN_IN_QUERY", OneToken()));
    CHECK(Denied("Результат = \"[STR-00001]\" + \"x\";", "C_FUNC_OVER_PROTECTED", OneToken()));
    CHECK(Denied(WithTable("Если Т[0].Имя = \"[STR-00001]\" Тогда Н = 1; КонецЕсли; Результат = 1;"), "C_IMPLICIT_FLOW",
                 OneToken()));
    CHECK(Safe("Результат = \"[STR-0001]\" + \"[ИНН-00001]\" + \"[A00001]\";"));   // not the token format
}

TEST(BslTaint, TokenTableParsing) {
    TokenTable t;
    CHECK(ParseTokenTable(R"({"version": 1})", t) && t.empty());
    CHECK(ParseTokenTable(R"({"tokens": {"[str-00001]": {"index": 0, "origin": "protected"},
                                         "[ORG-00002]": {"index": 1, "origin": "unknown"}}})", t));
    CHECK_EQ(t.size(), static_cast<size_t>(2));
    CHECK(t.count("[STR-00001]") == 1 && t["[STR-00001]"].origin == Origin::ProtectedDirect);
    CHECK(t["[ORG-00002]"].origin == Origin::Unknown && t["[ORG-00002]"].index == 1);
    for (const char* bad : {
             R"({"tokens": []})", R"({"tokens": {"STR-00001": {"index": 0, "origin": "protected"}}})",
             R"({"tokens": {"[STR-00001]": {"index": -1, "origin": "protected"}}})",
             R"({"tokens": {"[STR-00001]": {"index": 0, "origin": "client"}}})",
             R"({"tokens": {"[STR-00001]": {"index": 0}}})",
             R"({"tokens": {"[STR-00001]": {"index": 0, "origin": "protected", "value": "x"}}})",
             R"({"tokens": {"[STR-00001]": {"index": 0, "origin": "protected"}, "[ORG-00001]": {"index": 0, "origin": "protected"}}})",
             R"({"tokens": {"[STR-00001]": {"index": 0, "origin": "protected"}, "[str-00001]": {"index": 1, "origin": "protected"}}})",
             "{"}) {
        CHECK(!ParseTokenTable(bad, t));
        CHECK(t.empty());
    }
    CHECK(IsTokenFormat("[INN-00001]") && IsTokenFormat("[a-123456]"));
    CHECK(!IsTokenFormat("[INN-0001]") && !IsTokenFormat("[ИНН-00001]") && !IsTokenFormat("[INN00001]"));
}

TEST(BslTaint, ColumnsFromBoundParametersAreNotOvertainted) {
    // The first pass checks the query before the code has bound П; that pass must not leave Unknown behind.
    const auto r = CheckCodeForTest(kPolicyAB,
        "З = Новый Запрос(\"ВЫБРАТЬ &П КАК Х\"); З.УстановитьПараметр(\"П\", 1);\n"
        "Т = З.Выполнить().Выгрузить(); М = Новый Массив; М.Добавить(Т[0].Х); Результат = М;");
    CHECK(r.verdict == CodeVerdict::Safe);
    CHECK_EQ(r.result_label, Origin::Client);
}

TEST(BslTaint, ReplacementsPrintThroughTheCanonicalPrinter) {
    // The body must outlive the analysis: its nodes key the replacement maps.
    const lineage::bsl::ParseResult parsed = lineage::bsl::ParseBsl(
        "З = Новый Запрос; З.Текст = \"ВЫБРАТЬ А.Ссылка КАК С ИЗ Справочник.А КАК А ГДЕ А.Имя = &П\";\n"
        "З.УстановитьПараметр(\"П\", \"[STR-00001]\"); Результат = З.Выполнить().Выгрузить();");
    CHECK(parsed.ok);
    const CodeAnalysis r = AnalyzeCode(parsed.body, kPolicyAB, OneToken());
    CHECK(r.verdict == CodeVerdict::Safe);
    lineage::bsl::PrintOptions options;
    options.query_replacements = r.query_canonical;
    options.bound_tokens = r.bound_tokens;
    const lineage::bsl::PrintResult printed = lineage::bsl::PrintBslCanonicalChecked(parsed.body, options);
    CHECK(printed.ok);
    CHECK(printed.text.find("МСПЗначенияТокенов[0]") != std::string::npos);
    CHECK(printed.text.find("[STR-00001]") == std::string::npos);
    CHECK(printed.text.find(r.query_canonical.begin()->second.substr(0, 14)) != std::string::npos);
}

TEST(BslTaint, DeepNestingStaysWithinBudget) {
    // 30 nested loops, each with a branch: the walk must end (C_LIMIT at worst), quickly.
    std::string code = "Н = 0;\n";
    for (int i = 0; i < 30; ++i) code += "Для И" + std::to_string(i) + " = 1 По 2 Цикл Если Н = 1 Тогда Н" + std::to_string(i) + " = Н; КонецЕсли;\n";
    code += "Н = Н + 1;\n";
    for (int i = 0; i < 30; ++i) code += "КонецЦикла;\n";
    code += "Результат = Н;";
    const lineage::bsl::ParseResult parsed = lineage::bsl::ParseBsl(code);
    CHECK(parsed.ok);
    const CodeAnalysis r = AnalyzeCode(parsed.body, kPolicyAB, TokenTable());
    CHECK(r.verdict == CodeVerdict::Safe || HasCodeReason(r, "C_LIMIT"));
}

namespace {

// Справочник.А with a protected reference Р to Справочник.Б (open amount).
const FieldPolicy kPolicyProtectedRef = ParsePolicyOrDie(R"({
  "version": 1,
  "tables": {
    "Справочник.А": {
      "presentation": "protected",
      "fields": {"Р": {"class": "protected", "refs": ["Справочник.Б"]}, "Ссылка": {"class": "open", "refs": ["Справочник.А"]}}
    },
    "Справочник.Б": {
      "presentation": "protected",
      "fields": {"Сумма": {"class": "open"}, "Ссылка": {"class": "open", "refs": ["Справочник.Б"]}}
    }
  }
})");

const char* kQR = "З = Новый Запрос(\"ВЫБРАТЬ А.Р КАК Р ИЗ Справочник.А КАК А\"); Т = З.Выполнить().Выгрузить();\n";

}  // namespace

TEST(BslTaint, DereferenceOfTaintedReferenceIsAnOperation) {
    // Moving the protected reference itself is fine; reading through it picks a row by a protected value.
    CHECK(CheckCodeForTest(kPolicyProtectedRef, std::string(kQR) + "Результат = Т[0].Р;").verdict == CodeVerdict::Safe);
    const auto r = CheckCodeForTest(kPolicyProtectedRef, std::string(kQR) + "Результат = Т[0].Р.Сумма;");
    CHECK(HasCodeReason(r, "C_FUNC_OVER_PROTECTED"));
    // The dereference may fail, so the Исключение block depends on it.
    const auto t = CheckCodeForTest(kPolicyProtectedRef,
        std::string(kQR) + "Н = 0; Попытка Х = Т[0].Р.Сумма; Н = 1; Исключение КонецПопытки; Результат = Н;");
    CHECK(HasCodeReason(t, "C_IMPLICIT_FLOW"));
    CHECK(IsTainted(t.result_label));
}

TEST(BslTaint, KeyInsertionNextToTaintedKeysIsAnOracle) {
    CHECK(Denied(WithTable("М = Новый Соответствие; М.Вставить(Т[0].Имя, 1); М.Вставить(\"Иванов\", 2); Результат = М;"),
                 "C_TAINTED_KEY"));
    CHECK(Denied("М = Новый Соответствие; М.Вставить(\"[STR-00001]\", 1); М.Вставить(\"Иванов\", 2); Результат = М;",
                 "C_TAINTED_KEY", OneToken()));
    CHECK(Denied("М = Новый Соответствие; М[\"[STR-00001]\"] = 1; Результат = М;", "C_TAINTED_KEY", OneToken()));
    CHECK(Denied(WithTable("М = Новый Соответствие; М.Вставить(\"Иванов\", 1); М.Вставить(Т[0].Имя, 2); Результат = М;"),
                 "C_TAINTED_KEY"));
    // Open references and client literals as keys are fine.
    CHECK(Safe(WithTable("М = Новый Соответствие; М.Вставить(Т[0].С, 1); М.Вставить(Т[1].С, 2); М[\"К\"] = 3; Результат = М;")));
}

TEST(BslTaint, UnloadingAnUnknownColumnIsRefused) {
    CHECK(Denied(WithTable("Результат = Т.ВыгрузитьКолонку(\"Нет\");"), "C_UNKNOWN_MEMBER"));
    CHECK(Safe(WithTable("Результат = Т.ВыгрузитьКолонку(\"С\");")));
}

TEST(BslTaint, ConditionalLoopExitTaintsWhatTheLoopLeaves) {
    const auto b = CheckCodeForTest(kPolicyAB,
        WithTable("Н = 0; Для Каждого Стр Из Т Цикл Если Стр.Имя = \"x\" Тогда Прервать; КонецЕсли; Н = Н + 1;\n"
                  "КонецЦикла; Результат = Н;"));
    CHECK(HasCodeReason(b, "C_IMPLICIT_FLOW"));
    CHECK(IsTainted(b.result_label));
    const auto c = CheckCodeForTest(kPolicyAB,
        WithTable("Н = 0; Для Каждого Стр Из Т Цикл Если Стр.Имя = \"x\" Тогда Продолжить; КонецЕсли; Н = Н + 1;\n"
                  "КонецЦикла; Результат = Н;"));
    CHECK(IsTainted(c.result_label));
    // An exit under an open condition taints nothing.
    const auto o = CheckCodeForTest(kPolicyAB,
        WithTable("Н = 0; Для Каждого Стр Из Т Цикл Если Н = 3 Тогда Прервать; КонецЕсли; Н = Н + 1; КонецЦикла; Результат = Н;"));
    CHECK(o.verdict == CodeVerdict::Safe);
    CHECK(!IsTainted(o.result_label));
}

TEST(BslTaint, AnalysisKeepsItsParsedBody) {
    // The Expr* keys of the result stay valid as long as the result lives.
    const auto r = CheckCodeForTest(kPolicyAB, "Результат = \"[STR-00001]\";", OneToken());
    CHECK(r.parsed && r.parsed->ok);
    lineage::bsl::PrintOptions options;
    options.bound_tokens = r.bound_tokens;
    CHECK(lineage::bsl::PrintBslCanonicalChecked(r.parsed->body, options).ok);
}

// ---- risk classes ------------------------------------------------------------------------------------

namespace {

bool OnlyRisk(const std::string& code, const char* risk) {
    const auto r = CheckCodeForTest(kPolicyAB, code);
    const bool ok = HasRisk(r, risk) && r.verdict == CodeVerdict::Confirm;
    if (!ok) Dump(code, r);
    return ok;
}

}  // namespace

TEST(BslTaint, RiskClasses) {
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = ХранилищеОбщихНастроек.Загрузить(\"MCPToolkit\", \"Х\");"),
                  "R_PROTECTION"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "ФоновыеЗадания.Выполнить(\"М.П\"); Результат = 1;"), "R_DEFERRED"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "ЗаписьЖурналаРегистрации(\"x\"); Результат = 1;"), "R_WRITE"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = СтрокаСоединенияИнформационнойБазы();"), "R_CREDENTIALS"));
}

TEST(BslTaint, RiskForcesConfirmEvenWhenOtherwiseSafe) {
    // A variable that only happens to carry a risky name: no reason at all, still a risk and a confirmation.
    const auto r = CheckCodeForTest(kPolicyAB, "Записать = 1; Результат = Записать;");
    CHECK(r.reasons.empty());
    CHECK(HasRisk(r, "R_WRITE"));
    CHECK(r.verdict == CodeVerdict::Confirm);
    const auto plain = CheckCodeForTest(kPolicyAB, "Х = 1; Результат = Х;");
    CHECK(plain.risks.empty());
    CHECK(plain.verdict == CodeVerdict::Safe);
}

TEST(BslTaint, RiskNamesAreFoundAfterDotsAndInEveryPosition) {
    CHECK(OnlyRisk("М = Новый Массив; М.Удалить(0); Результат = 1;", "R_WRITE"));
    CHECK(OnlyRisk("Результат = Модуль.УстановитьПривилегированныйРежим(Истина);", "R_PROTECTION"));
    CHECK(OnlyRisk("Результат = Менеджер.Свойство.ПарольПользователя;", "R_CREDENTIALS"));
    CHECK(OnlyRisk("Для Каждого ФоновыеЗадания Из М Цикл Результат = 1; КонецЦикла;", "R_DEFERRED"));
    CHECK(OnlyRisk("О = Новый ЗаписьТекста; Результат = 1;", "R_WRITE"));
    CHECK(OnlyRisk("Р = РегистрыСведений.X.СоздатьНаборЗаписей(); Результат = 1;", "R_WRITE"));
    CHECK(OnlyRisk("НачатьТранзакцию(); Результат = 1;", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ОбменДанными.Загрузка;", "R_WRITE"));
    CHECK(OnlyRisk("С = Новый Структура; С.ОбменДанными = Новый Структура(\"Загрузка\", Истина); Результат = 1;",
                   "R_WRITE"));
}

TEST(BslTaint, RiskNamesFoldCaseYoAndEnglish) {
    CHECK(OnlyRisk("Результат = хранилищеобщихнастроек.Загрузить(\"a\", \"b\");", "R_PROTECTION"));
    CHECK(OnlyRisk("Результат = CommonSettingsStorage.Load(\"a\", \"b\");", "R_PROTECTION"));
    CHECK(OnlyRisk("Результат = commonsettingsstorage.Load(\"a\", \"b\");", "R_PROTECTION"));
    CHECK(OnlyRisk("Результат = InfoBaseConnectionString();", "R_CREDENTIALS"));
    CHECK(OnlyRisk("Результат = BackgroundJobs.Execute(\"M.P\");", "R_DEFERRED"));
    CHECK(OnlyRisk("WriteLogEvent(\"x\"); Результат = 1;", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ВладелецФормы;", "R_PROTECTION"));
    // ё folds to е in the table too ("ПОМЕЧЕН" style names carry no ё here; use a name with е that is typed with ё).
    CHECK(OnlyRisk("Результат = ЭтотОбъект.Метод();", "R_PROTECTION"));
    CHECK(OnlyRisk("Результат = ЭтотОбъект;", "R_PROTECTION"));
}

TEST(BslTaint, RiskScanRunsWhenTheParserRefuses) {
    // Parser refusals (C_EVAL, C_PARSE, C_DYNAMIC_NEW) leave no tree; the token scan still reports the markers.
    auto r = CheckCodeForTest(kPolicyAB, "Выполнить(\"1\"); ЗаписьЖурналаРегистрации(\"x\");");
    CHECK(HasCodeReason(r, "C_EVAL"));
    CHECK(HasRisk(r, "R_WRITE"));
    r = CheckCodeForTest(kPolicyAB, "Процедура П() ПоместитьВоВременноеХранилище(1); КонецПроцедуры");
    CHECK(HasRisk(r, "R_WRITE"));
    r = CheckCodeForTest(kPolicyAB, "О = Новый(\"ЗаписьТекста\"); Результат = 1;");
    CHECK(HasCodeReason(r, "C_DYNAMIC_NEW"));
    CHECK(HasRisk(r, "R_WRITE"));
    r = CheckCodeForTest(kPolicyAB, "ДобавитьОбработчик А.Б, В; Результат = 1;");
    CHECK(HasRisk(r, "R_DEFERRED"));
    // Identifiers that mix Latin and Cyrillic are refused by the parser; the homoglyph fold still names the class.
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "С = Новый HTTPСоединение(\"x\"); Результат = 1;"), "R_CREDENTIALS"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = XранилищеОбщихНастроек.Загрузить(\"a\", \"b\");"),
                  "R_PROTECTION"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "С = Новый COMОбъект(\"x\"); Результат = 1;"), "R_CREDENTIALS"));
}

TEST(BslTaint, RiskScanFallsBackToRawTextOnUnknownTokens) {
    // A block comment is an Unknown token for the lexer: the whole text is searched, comments and strings included.
    auto r = CheckCodeForTest(kPolicyAB, "/* x */ Результат = 1; // ok\n/* ЗаписьТекста */");
    CHECK(HasRisk(r, "R_WRITE"));
    // A zero-width character inside a word does not hide it.
    r = CheckCodeForTest(kPolicyAB, std::string("{ Зап\xE2\x80\x8B") + "исать(1) }");
    CHECK(HasRisk(r, "R_WRITE"));
    // Word boundaries apply to short names in the raw scan: "Reset" is not "Set*".
    r = CheckCodeForTest(kPolicyAB, "{ Reset }");
    CHECK(!HasRisk(r, "R_WRITE"));
}

TEST(BslTaint, OldDangerousWordsMapToARiskOrAReject) {
    // The words of the host's ПроверитьОпасныеСлова: each is a risk class (or a reject for the eval primitives).
    const char* words[] = {"Удалить", "Delete", "Записать", "Write", "УстановитьПривилегированныйРежим",
                           "SetPrivilegedMode", "ПодключитьВнешнююКомпоненту", "AttachAddIn",
                           "УстановитьВнешнююКомпоненту", "InstallAddIn", "COMОбъект", "COMObject",
                           "УстановитьМонопольныйРежим", "SetExclusiveMode", "УдалитьФайлы", "DeleteFiles",
                           "КопироватьФайл", "CopyFile", "ПереместитьФайл", "MoveFile", "СоздатьКаталог",
                           "CreateDirectory", "ЗапуститьПриложение", "RunApp", "КомандаСистемы", "System"};
    for (const char* w : words) {
        const std::string call = std::string("Результат = ") + w + "(1);";
        const std::string member = std::string("Результат = М.") + w + "(1);";
        const auto a = CheckCodeForTest(kPolicyAB, call);
        const auto b = CheckCodeForTest(kPolicyAB, member);
        const bool ok = !a.risks.empty() && !b.risks.empty() && a.verdict == CodeVerdict::Confirm &&
                        b.verdict == CodeVerdict::Confirm;
        if (!ok) std::cout << "  no risk for: " << w << std::endl;
        CHECK(ok);
    }
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Выполнить(\"1\");"), "C_EVAL"));
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Результат = Вычислить(\"1\");"), "C_EVAL"));
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Execute(\"1\");"), "C_EVAL"));
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "Результат = Eval(\"1\");"), "C_EVAL"));
}

TEST(BslTaint, LegitimateQueryFlowHasNoRisks) {
    const std::string code = std::string(kQ) +
        "Т = З.Выполнить().Выгрузить(); М = Новый Массив;\n"
        "Для Каждого Стр Из Т Цикл М.Добавить(Новый Структура(\"С, Имя\", Стр.С, Стр.Имя)); КонецЦикла;\n"
        "Результат = М;";
    const auto r = CheckCodeForTest(kPolicyAB, code);
    CHECK(r.risks.empty());
    CHECK(r.verdict == CodeVerdict::Safe);
}

TEST(BslTaint, UnknownCalleeCarriesUnknownEffectRisk) {
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = МойМодуль.Сделать(1);"), "R_UNKNOWN_EFFECT"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = НеизвестнаяФункция(1);"), "R_UNKNOWN_EFFECT"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Х = ПолучитьЧто(); Результат = Х.Сделать();"), "R_UNKNOWN_EFFECT"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "О = Новый МойТип(1); Результат = 1;"), "R_UNKNOWN_EFFECT"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "М = Новый Массив; Результат = М.Сделать();"), "R_UNKNOWN_EFFECT"));
    // A property read without a call is a reason only.
    const auto r = CheckCodeForTest(kPolicyAB, "М = Новый Массив; Результат = М.Свойство;");
    CHECK(HasCodeReason(r, "C_UNKNOWN_MEMBER"));
    CHECK(!HasRisk(r, "R_UNKNOWN_EFFECT"));
}

TEST(BslTaint, RefusedCodeAlwaysCarriesUnknownEffect) {
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Процедура П() КонецПроцедуры"), "R_UNKNOWN_EFFECT"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = ;"), "R_UNKNOWN_EFFECT"));
    // The walk gave up: C_LIMIT carries it too.
    std::string deep = "Результат = ";
    for (int i = 0; i < 50; ++i) deep += "(1 + ";
    deep += "1";
    for (int i = 0; i < 50; ++i) deep += ")";
    deep += ";";
    const auto r = CheckCodeForTest(kPolicyAB, deep);
    if (HasCodeReason(r, "C_LIMIT")) CHECK(HasRisk(r, "R_UNKNOWN_EFFECT"));
}

TEST(BslTaint, SyntacticUnknownCalleePass) {
    // Independent of the type walk: a callee or type on no allowlist table.
    auto r = CheckCodeForTest(kPolicyAB, "Х = Новый ТаблицаЗначений; Результат = Х.Колонки.Добавить(\"а\");");
    CHECK(HasRisk(r, "R_UNKNOWN_EFFECT"));
    r = CheckCodeForTest(kPolicyAB, "Результат = СокрЛП(\"а\");");
    CHECK(!HasRisk(r, "R_UNKNOWN_EFFECT"));
    // The same on text the parser refuses: identifier + "(", "Новый X".
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Процедура П() Чужая(1); КонецПроцедуры"), "R_UNKNOWN_EFFECT"));
    // The Запрос method is the allowlist's own: no write risk from the Установить* family.
    const auto q = CheckCodeForTest(kPolicyAB, "З = Новый Запрос; З.УстановитьПараметр(\"П\", 1); Результат = 1;");
    CHECK(!HasRisk(q, "R_WRITE"));
    CHECK(!HasRisk(q, "R_UNKNOWN_EFFECT"));
}

TEST(BslTaint, RiskFamiliesAndAddedNames) {
    CHECK(OnlyRisk("Результат = УстановитьЧто(1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = УдалитьЧто(1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ЗаписатьЧто(1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ПолучитьCOMОбъект(\"x\");", "R_CREDENTIALS"));
    CHECK(OnlyRisk("Результат = GetCOMObject(\"x\");", "R_CREDENTIALS"));
    CHECK(OnlyRisk("Результат = ЗначениеВФайл(\"x\", 1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ValueToFile(\"x\", 1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ЗначениеИзФайла(\"x\");", "R_CREDENTIALS"));
    CHECK(OnlyRisk("Результат = ПолучитьПеременнуюСреды(\"x\");", "R_CREDENTIALS"));
    CHECK(OnlyRisk("Результат = УстановитьЧасовойПояс(\"x\");", "R_WRITE"));
    CHECK(OnlyRisk("Результат = УстановитьРежимСовместимости(1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = УстановитьПараметрыСеанса(1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ПланыОбмена.X.УдалитьРегистрациюИзменений(1);", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ПересчитатьИтоги();", "R_WRITE"));
    CHECK(OnlyRisk("Результат = ЗавершитьСеанс(1);", "R_PROTECTION"));
    CHECK(OnlyRisk("Результат = РасширенияКонфигурации.Получить();", "R_PROTECTION"));
}

TEST(BslTaint, EvalAndDynamicNewScanTheirStrings) {
    auto r = CheckCodeForTest(kPolicyAB, "Выполнить(\"ФоновыеЗадания.Выполнить()\");");
    CHECK(HasRisk(r, "R_PROTECTION"));
    CHECK(HasRisk(r, "R_DEFERRED"));
    r = CheckCodeForTest(kPolicyAB, "О = Новый(Тип(\"ЗаписьТекста\")); Результат = 1;");
    CHECK(HasRisk(r, "R_PROTECTION"));
    CHECK(HasRisk(r, "R_WRITE"));
    r = CheckCodeForTest(kPolicyAB, "О = Новый(\"Запи\" + \"сать\"); Результат = 1;");
    CHECK(HasRisk(r, "R_PROTECTION"));
    CHECK(HasRisk(r, "R_WRITE"));
}

TEST(BslTaint, InvisibleAndLookalikeCharacters) {
    const char* invisible[] = {"\xCD\x8F", "\xD8\x9C", "\xE1\xA0\x8E", "\xE2\x81\xA6", "\xE2\x81\xAF", "\xE3\x85\xA4",
                               "\xEF\xB8\x8F", "\xE2\xA0\x80", "\xF3\xA0\x80\x81", "\xF3\xA0\x81\xBF"};
    for (const char* ch : invisible) {
        const auto r = CheckCodeForTest(kPolicyAB, std::string("{ Зап") + ch + "исать(1) }");
        if (!HasRisk(r, "R_WRITE")) std::cout << "  hidden by an invisible character" << std::endl;
        CHECK(HasRisk(r, "R_WRITE"));
    }
    // Greek and Ukrainian twins of Latin letters.
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "{ \xCE\x97\xCE\xA4\xCE\xA4\xCE\xA1 }"), "R_CREDENTIALS"));        // ΝΤΤΡ
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, "Результат = \xD0\x85ystem(1);"), "R_PROTECTION"));   // Ѕystem
}

// ТаблицаЗначений.Сортировать with a literal list of open columns only reorders rows the client receives anyway. A
// sort the checker cannot prove so is refused like an unknown method (C_UNKNOWN_METHOD, R_UNKNOWN_EFFECT).
TEST(BslTaint, SortByLiteralOpenColumns) {
    const std::string q =
        "З = Новый Запрос(\"ВЫБРАТЬ Б.Сумма КАК Сумма, Б.Код КАК Код, Б.СсылкаНаА КАК С, 1 КАК Л ИЗ Справочник.Б КАК Б\");\n"
        "Т = З.Выполнить().Выгрузить();\n";
    CHECK(Safe(q + "Т.Сортировать(\"Сумма\"); Результат = Т;"));
    CHECK(Safe(q + "Т.Сортировать(\"Сумма Убыв, код возр,Л\"); Результат = Т;"));
    CHECK(Safe(q + "Т.Sort(\"Сумма DESC, Код Asc\"); Результат = Т;"));
    const auto ok = CheckCodeForTest(kPolicyAB, q + "Т.Сортировать(\"Сумма\"); Результат = Т;");
    CHECK(!HasRisk(ok, "R_UNKNOWN_EFFECT"));
    for (const char* bad : {
             "Т.Сортировать(\"С\");",                 // a reference: sorted by a protected presentation
             "Т.Сортировать(\"Нет\");",               // not a column
             "Т.Сортировать(\"Сумма Вниз\");",        // not a direction
             "Т.Сортировать(\"Сумма Убыв Возр\");",
             "Т.Сортировать(\"Сумма,\");",
             "Т.Сортировать(\"\");",
             "Т.Сортировать(\"Сумма.Код\");",
             "Т.Сортировать(\"Сумма\", 1);",          // a comparison object
             "К = \"Сумма\"; Т.Сортировать(К);",      // not a literal
         }) {
        const auto r = CheckCodeForTest(kPolicyAB, q + bad + " Результат = Т;");
        if (r.verdict != CodeVerdict::Confirm) Dump(q + bad, r);
        CHECK(r.verdict == CodeVerdict::Confirm);
    }
    CHECK(Denied(q + "Т.Сортировать(\"С\"); Результат = Т;", "C_UNKNOWN_METHOD"));
    CHECK(HasRisk(CheckCodeForTest(kPolicyAB, q + "Т.Сортировать(\"С\"); Результат = Т;"), "R_UNKNOWN_EFFECT"));
    CHECK(Denied(q + "Т.Сортировать(\"Нет\"); Результат = Т;", "C_UNKNOWN_METHOD"));
    // A protected column, a table whose columns are not known, a sort under a protected condition.
    CHECK(Denied(std::string(kQ) + "Т = З.Выполнить().Выгрузить(); Т.Сортировать(\"Имя\"); Результат = 1;",
                 "C_UNKNOWN_METHOD"));
    CHECK(Denied("Т = Новый ТаблицаЗначений; Т.Сортировать(\"А\"); Результат = 1;", "C_UNKNOWN_METHOD"));
    CHECK(Denied(std::string(kQ) + "Т = З.Выполнить().Выгрузить(); Если Т[0].Имя = 1 Тогда Т.Сортировать(\"С\"); "
                 "КонецЕсли; Результат = 1;", "C_IMPLICIT_FLOW"));
    // A reference column is accepted when every target's presentation is open.
    const auto open = FieldPolicy::FromJson(R"({"version":1,"tables":{
        "Справочник.О": {"presentation": "open", "fields": {"Ссылка": {"class": "open", "refs": ["Справочник.О"]}}}}})");
    CHECK(open.ok);
    const auto r = CheckCodeForTest(open.policy,
        "З = Новый Запрос(\"ВЫБРАТЬ О.Ссылка КАК С ИЗ Справочник.О КАК О\"); Т = З.Выполнить().Выгрузить();\n"
        "Т.Сортировать(\"С\"); Результат = Т;");
    CHECK(r.verdict == CodeVerdict::Safe);
}

// A text made only of string literals joined with "+" (any grouping) is folded; through a variable it is not (the
// attack corpus pins that: c_query_text_from_variable), and Символы is reserved, so Символы.ПС never reaches it.
TEST(BslTaint, LiteralOnlyQueryTextIsFolded) {
    const auto r = CheckCodeForTest(kPolicyAB,
        "Т = Новый Запрос(\"ВЫБРАТЬ Б.Сумма КАК Сумма \" + (\"ИЗ Справочник.Б\" + \" КАК Б\")).Выполнить().Выгрузить();\n"
        "Результат = Т;");
    CHECK(r.verdict == CodeVerdict::Safe);
    CHECK_EQ(r.query_texts.size(), static_cast<size_t>(1));
    CHECK_EQ(r.query_texts[0].text, std::string("ВЫБРАТЬ Б.Сумма КАК Сумма ИЗ Справочник.Б КАК Б"));
    CHECK(Denied("Т = \"ВЫБРАТЬ 1\"; З = Новый Запрос(\"\" + Т); Результат = 1;", "C_DYNAMIC_QUERY"));
    CHECK(Denied("З = Новый Запрос(\"ВЫБРАТЬ 1\" + 2); Результат = 1;", "C_DYNAMIC_QUERY"));
    CHECK(HasCodeReason(CheckCodeForTest(kPolicyAB, "З = Новый Запрос(\"ВЫБРАТЬ\" + Символы.ПС + \"1\"); Результат = 1;"),
                        "C_RESERVED"));
}

// A chain on "Новый Запрос(<text>)" is typed like the same steps on a variable; the query is never bound.
TEST(BslTaint, ChainOnNewQuery) {
    const auto r = CheckCodeForTest(kPolicyAB,
        "Т = Новый Запрос(\"ВЫБРАТЬ Б.Сумма КАК Сумма ИЗ Справочник.Б КАК Б\").Выполнить().Выгрузить(); Результат = Т;");
    CHECK(r.verdict == CodeVerdict::Safe);
    CHECK_EQ(r.query_texts.size(), static_cast<size_t>(1));
    CHECK_EQ(r.query_canonical.size(), static_cast<size_t>(1));
    CHECK(Safe("В = Новый Запрос(\"ВЫБРАТЬ Б.Сумма КАК Сумма ИЗ Справочник.Б КАК Б\").Выполнить().Выбрать();\n"
               "Пока В.Следующий() Цикл Результат = В.Сумма; КонецЦикла;"));
    CHECK(Denied("Результат = Новый Запрос(\"ВЫБРАТЬ 1\").Выполнить().Выгрузить()[0][0];", "C_UNKNOWN_INDEX"));
    CHECK(Denied("Т = \"ВЫБРАТЬ 1\"; Результат = Новый Запрос(Т).Выполнить().Выгрузить();", "C_DYNAMIC_QUERY"));
    CHECK(Denied("Результат = Новый Запрос(\"ВЫБРАТЬ 1\").Текст;", "C_UNKNOWN_MEMBER"));
    CHECK(Denied("Результат = Новый Запрос(\"ВЫБРАТЬ А.Имя КАК Имя ИЗ Справочник.А КАК А ГДЕ А.Имя = \"\"x\"\"\").Выполнить()"
                 ".Выгрузить();", "Q_EQ_LITERAL"));
    // A chain that ends on the Запрос itself is an escape like any other use of it as a value.
    CHECK(Denied("Х = Новый Массив; Х.Добавить(Новый Запрос(\"ВЫБРАТЬ 1\")); Результат = 1;", "C_QUERY_ESCAPES"));
}

// A parameter name literal outside the alphabet of a parameter after '&' is refused, an open value included; a
// mixed-script name inside it is not (the query text names another parameter: the query parser refuses mixed
// scripts on its own).
TEST(BslTaint, ParamNameOutsideTheParameterAlphabet) {
    for (const char* name : {"П ", " П", "П\xC2\xA0", "\xE2\x80\x8BП", "", "1П", "П-1", "\xE2\x84\xAA"}) {
        CHECK(Denied(std::string("З = Новый Запрос(\"ВЫБРАТЬ &П КАК А\"); З.УстановитьПараметр(\"") + name +
                         "\", 1); Результат = З.Выполнить().Выгрузить();",
                     "C_BAD_PARAM_NAME"));
    }
    CHECK(Safe("З = Новый Запрос(\"ВЫБРАТЬ &П КАК А\"); З.УстановитьПараметр(\"П\", 1); "
               "З.УстановитьПараметр(\"ДатаUTC\", 1); "
               "Результат = З.Выполнить().Выгрузить();"));
}
