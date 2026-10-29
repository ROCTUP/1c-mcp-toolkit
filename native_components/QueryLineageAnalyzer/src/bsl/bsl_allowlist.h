#pragma once
#include <cstddef>
#include <cstdint>

// Allowlist data of the strict BSL checker. Data only: no logic, no lookups; bsl_types.cpp builds
// its tables from the arrays below. Default deny: a global function, a "Новый" type, a member or a method that is
// not listed here is a reason code and a confirmation, never a pass. Every name is given in Russian and in the
// English platform synonym; matching is case-insensitive and folds ё to е (AllowlistKey in bsl_types.h).
//
// A method is matched by the receiver's TRACKED type, never by name alone. Rules for the result type:
//   - a column of a query result is a Ref when the label step reports target tables, otherwise Unknown;
//   - the element of a collection (Array.Получить, Структура[...], Соответствие.Получить) is Unknown.
//
// Taint (consumed by Tasks 25/26, not by the type pass):
//   Propagate: the result carries the labels of the receiver and the arguments (values move, nothing is derived);
//   Open:      the arguments must be open or client data; over protected data the call needs confirmation
//              (C_FUNC_OVER_PROTECTED); the result is open;
//   Confirm:   always needs confirmation.
// Hand-off to the taint pass: a Ref argument to Строка / Формат / Лев / ... reads its presentation from the database; it must be
// labelled as the target table's presentation class (protected unless the policy says open). The Open rows below apply
// only to non-reference arguments.
// The taint pass may refine individual rows (for example the one-bit results of ЗначениеЗаполнено and Количество).

namespace lineage::bsl {

enum class Ty : uint8_t {
    Unknown,          // nothing can be proven: no member, method or index is allowed on it
    Undefined,
    Null,
    Boolean,
    Number,
    String,
    Date,
    TypeValue,        // result of Тип / ТипЗнч
    Query,            // Запрос
    QueryResult,      // РезультатЗапроса
    Selection,        // ВыборкаИзРезультатаЗапроса
    ValueTable,       // ТаблицаЗначений
    TableRow,         // СтрокаТаблицыЗначений
    ColumnCollection, // КоллекцияКолонокТаблицыЗначений (the .Колонки property)
    Array,
    Structure,
    Map,
    ValueList,        // created, but no member is allowed on it
    Uuid,
    Ref,              // a reference value (a column that the label step reports as a reference)
    MetaObject        // result of Ref.Метаданные()
};

enum class Taint : uint8_t { Propagate, Open, Confirm };

// Constraint on the first argument of a method or function.
enum class ArgRule : uint8_t {
    Any,
    LiteralString   // must be a string literal (a name: query parameter, structure key, type name, column)
};

struct GlobalRule {
    const char* ru;
    const char* en;
    Ty result;
    Taint taint;
    int min_args;
    int max_args;
    ArgRule arg0;
};

struct NewRule {
    const char* ru;
    const char* en;
    Ty result;
    int min_args;
    int max_args;
};

struct MethodRule {
    Ty receiver;
    const char* ru;
    const char* en;
    Ty result;
    Taint taint;
    int min_args;
    int max_args;
    ArgRule arg0;
};

// A property read without a call (".Колонки"). Columns of a selection or a row are handled by the column set.
struct PropertyRule {
    Ty receiver;
    const char* ru;
    const char* en;
    Ty result;
};

// Writable property: only Запрос.Текст, handled by the checker (a constant-folded literal is required).
inline constexpr const char* kQueryTextRu = "Текст";
inline constexpr const char* kQueryTextEn = "Text";

// Out-parameter method: Структура.Свойство("Ключ", Переменная) assigns the second argument.
inline constexpr const char* kPropertyMethodRu = "Свойство";
inline constexpr const char* kPropertyMethodEn = "Property";

// Evaluation members: refused with C_EVAL on any receiver, whatever its type.
inline constexpr const char* kEvalMemberRu = "Вычислить";
inline constexpr const char* kEvalMemberEn = "Eval";

// Names that denote the execution context rather than a module: an unassigned use is C_UNKNOWN_IDENT, not
// C_COMMON_MODULE.
inline constexpr const char* kContextNames[] = {"ЭтотОбъект", "ЭтаФорма", "ThisObject", "ThisForm"};

inline constexpr GlobalRule kGlobalRules[] = {
    {"ЗначениеЗаполнено", "ValueIsFilled", Ty::Boolean, Taint::Propagate, 1, 1, ArgRule::Any},
    {"ТипЗнч", "TypeOf", Ty::TypeValue, Taint::Propagate, 1, 1, ArgRule::Any},
    {"Тип", "Type", Ty::TypeValue, Taint::Propagate, 1, 1, ArgRule::LiteralString},
    {"Строка", "String", Ty::String, Taint::Open, 1, 1, ArgRule::Any},
    {"Число", "Number", Ty::Number, Taint::Open, 1, 1, ArgRule::Any},
    {"Дата", "Date", Ty::Date, Taint::Open, 1, 6, ArgRule::Any},
    {"Формат", "Format", Ty::String, Taint::Open, 2, 2, ArgRule::Any},
    {"Мин", "Min", Ty::Unknown, Taint::Open, 1, 64, ArgRule::Any},
    {"Макс", "Max", Ty::Unknown, Taint::Open, 1, 64, ArgRule::Any},
    {"Окр", "Round", Ty::Number, Taint::Open, 1, 3, ArgRule::Any},
    {"Цел", "Int", Ty::Number, Taint::Open, 1, 1, ArgRule::Any},
    {"Лев", "Left", Ty::String, Taint::Open, 2, 2, ArgRule::Any},
    {"Прав", "Right", Ty::String, Taint::Open, 2, 2, ArgRule::Any},
    {"Сред", "Mid", Ty::String, Taint::Open, 2, 3, ArgRule::Any},
    {"СтрДлина", "StrLen", Ty::Number, Taint::Open, 1, 1, ArgRule::Any},
    {"ВРег", "Upper", Ty::String, Taint::Open, 1, 1, ArgRule::Any},
    {"НРег", "Lower", Ty::String, Taint::Open, 1, 1, ArgRule::Any},
    {"СокрЛП", "TrimAll", Ty::String, Taint::Open, 1, 1, ArgRule::Any},
    {"СтрЗаменить", "StrReplace", Ty::String, Taint::Open, 3, 3, ArgRule::Any},
    {"Найти", "Find", Ty::Number, Taint::Open, 2, 2, ArgRule::Any},
    {"КодСимвола", "CharCode", Ty::Number, Taint::Open, 1, 2, ArgRule::Any},
    {"Символ", "Char", Ty::String, Taint::Open, 1, 1, ArgRule::Any},
};

inline constexpr NewRule kNewRules[] = {
    {"Запрос", "Query", Ty::Query, 0, 1},
    {"Массив", "Array", Ty::Array, 0, 1},
    {"Структура", "Structure", Ty::Structure, 0, 64},
    {"Соответствие", "Map", Ty::Map, 0, 1},
    {"ТаблицаЗначений", "ValueTable", Ty::ValueTable, 0, 0},
    {"СписокЗначений", "ValueList", Ty::ValueList, 0, 0},
    {"УникальныйИдентификатор", "UUID", Ty::Uuid, 0, 1},
};

inline constexpr MethodRule kMethodRules[] = {
    {Ty::Query, "Выполнить", "Execute", Ty::QueryResult, Taint::Propagate, 0, 0, ArgRule::Any},
    {Ty::Query, "УстановитьПараметр", "SetParameter", Ty::Undefined, Taint::Propagate, 2, 2, ArgRule::LiteralString},

    {Ty::QueryResult, "Выбрать", "Select", Ty::Selection, Taint::Propagate, 0, 0, ArgRule::Any},
    {Ty::QueryResult, "Выгрузить", "Unload", Ty::ValueTable, Taint::Propagate, 0, 0, ArgRule::Any},
    {Ty::QueryResult, "Пустой", "IsEmpty", Ty::Boolean, Taint::Open, 0, 0, ArgRule::Any},

    {Ty::Selection, "Следующий", "Next", Ty::Boolean, Taint::Open, 0, 0, ArgRule::Any},
    {Ty::Selection, "Количество", "Count", Ty::Number, Taint::Open, 0, 0, ArgRule::Any},

    {Ty::ValueTable, "Количество", "Count", Ty::Number, Taint::Open, 0, 0, ArgRule::Any},
    {Ty::ValueTable, "ВыгрузитьКолонку", "UnloadColumn", Ty::Array, Taint::Propagate, 1, 1, ArgRule::LiteralString},
    // Only a literal list of open columns; any other sort is refused by the checker like an unknown method.
    {Ty::ValueTable, "Сортировать", "Sort", Ty::Undefined, Taint::Propagate, 1, 1, ArgRule::LiteralString},
    {Ty::ColumnCollection, "Количество", "Count", Ty::Number, Taint::Open, 0, 0, ArgRule::Any},

    {Ty::Array, "Добавить", "Add", Ty::Undefined, Taint::Propagate, 1, 1, ArgRule::Any},
    {Ty::Array, "Количество", "Count", Ty::Number, Taint::Open, 0, 0, ArgRule::Any},
    {Ty::Array, "Получить", "Get", Ty::Unknown, Taint::Propagate, 1, 1, ArgRule::Any},

    {Ty::Structure, "Вставить", "Insert", Ty::Undefined, Taint::Propagate, 2, 2, ArgRule::Any},
    {Ty::Structure, "Свойство", "Property", Ty::Boolean, Taint::Propagate, 1, 2, ArgRule::LiteralString},
    {Ty::Structure, "Количество", "Count", Ty::Number, Taint::Open, 0, 0, ArgRule::Any},

    {Ty::Map, "Вставить", "Insert", Ty::Undefined, Taint::Propagate, 2, 2, ArgRule::Any},
    {Ty::Map, "Получить", "Get", Ty::Unknown, Taint::Propagate, 1, 2, ArgRule::Any},
    {Ty::Map, "Количество", "Count", Ty::Number, Taint::Open, 0, 0, ArgRule::Any},

    {Ty::Ref, "УникальныйИдентификатор", "UniqueIdentifier", Ty::Uuid, Taint::Open, 0, 0, ArgRule::Any},
    {Ty::Ref, "Пустая", "IsEmpty", Ty::Boolean, Taint::Open, 0, 0, ArgRule::Any},
    {Ty::Ref, "Метаданные", "Metadata", Ty::MetaObject, Taint::Open, 0, 0, ArgRule::Any},
    {Ty::MetaObject, "ПолноеИмя", "FullName", Ty::String, Taint::Open, 0, 0, ArgRule::Any},
};

inline constexpr PropertyRule kPropertyRules[] = {
    {Ty::ValueTable, "Колонки", "Columns", Ty::ColumnCollection},
};

// ---- Risk classes ----------------------------------------------------------------------------
// A risk is independent of the reasons: the NAME alone raises it, wherever it stands (variable, global function,
// "Новый" type, loop variable and every member or method name after a dot), whatever the receiver's type is. A risk
// always forces Confirm. The table is deliberately wider than the allowlist (default deny): an over-report shows an
// extra marker in the gate, a miss would hide a side effect. Names are matched on AllowlistKey (case-insensitive,
// ё = е). Modes: Exact (the whole name), Prefix, Contains (substring). Russian and English spellings are separate
// rows. Codes: Protection R_PROTECTION, Deferred R_DEFERRED, Write R_WRITE, Credentials R_CREDENTIALS.
// Relaunch (R_RELAUNCH) and the client context (R_CLIENT_CONTEXT) are appended by the host, not found in code.
enum class RiskClass : uint8_t { Protection, Deferred, Write, Credentials };
enum class RiskMatch : uint8_t { Exact, Prefix, Contains };

struct RiskRule {
    const char* name;
    RiskClass cls;
    RiskMatch match;
};

inline constexpr RiskRule kRiskRules[] = {
    // ---- Protection
    {"ЗавершитьСеанс", RiskClass::Protection, RiskMatch::Exact},
    {"EndSession", RiskClass::Protection, RiskMatch::Exact},
    {"TerminateSession", RiskClass::Protection, RiskMatch::Exact},
    {"РасширенияКонфигурации", RiskClass::Protection, RiskMatch::Exact},
    {"ConfigurationExtensions", RiskClass::Protection, RiskMatch::Exact},
    {"ХранилищеОбщихНастроек", RiskClass::Protection, RiskMatch::Exact},
    {"CommonSettingsStorage", RiskClass::Protection, RiskMatch::Exact},
    {"ХранилищеСистемныхНастроек", RiskClass::Protection, RiskMatch::Exact},
    {"SystemSettingsStorage", RiskClass::Protection, RiskMatch::Exact},
    {"ХранилищеПользовательскихНастроекДинамическихСписков", RiskClass::Protection, RiskMatch::Exact},
    {"DynamicListsUserSettingsStorage", RiskClass::Protection, RiskMatch::Exact},
    {"ХранилищеНастроекДанныхФорм", RiskClass::Protection, RiskMatch::Exact},
    {"FormDataSettingsStorage", RiskClass::Protection, RiskMatch::Exact},
    {"ХранилищеПользовательск", RiskClass::Protection, RiskMatch::Prefix},
    {"ХранилищеВариант", RiskClass::Protection, RiskMatch::Prefix},
    {"ХранилищеНастроек", RiskClass::Protection, RiskMatch::Prefix},
    {"ХранилищеОбщ", RiskClass::Protection, RiskMatch::Prefix},
    {"ХранилищеСистем", RiskClass::Protection, RiskMatch::Prefix},
    {"SettingsStorage", RiskClass::Protection, RiskMatch::Contains},
    {"VariantsStorage", RiskClass::Protection, RiskMatch::Contains},
    {"ЭтаФорма", RiskClass::Protection, RiskMatch::Exact},
    {"ThisForm", RiskClass::Protection, RiskMatch::Exact},
    {"ЭтотОбъект", RiskClass::Protection, RiskMatch::Exact},
    {"ThisObject", RiskClass::Protection, RiskMatch::Exact},
    {"ВладелецФормы", RiskClass::Protection, RiskMatch::Exact},
    {"FormOwner", RiskClass::Protection, RiskMatch::Exact},
    {"ПолучитьФорму", RiskClass::Protection, RiskMatch::Exact},
    {"GetForm", RiskClass::Protection, RiskMatch::Exact},
    {"ПолучитьОбщуюФорму", RiskClass::Protection, RiskMatch::Exact},
    {"GetCommonForm", RiskClass::Protection, RiskMatch::Exact},
    {"ОткрытьФорму", RiskClass::Protection, RiskMatch::Prefix},
    {"OpenForm", RiskClass::Protection, RiskMatch::Prefix},
    {"ОбщиеФормы", RiskClass::Protection, RiskMatch::Exact},
    {"CommonForms", RiskClass::Protection, RiskMatch::Exact},
    {"ОбщиеМодули", RiskClass::Protection, RiskMatch::Exact},
    {"CommonModules", RiskClass::Protection, RiskMatch::Exact},
    {"Обработки", RiskClass::Protection, RiskMatch::Exact},
    {"DataProcessors", RiskClass::Protection, RiskMatch::Exact},
    {"Отчеты", RiskClass::Protection, RiskMatch::Exact},
    {"Reports", RiskClass::Protection, RiskMatch::Exact},
    {"ПараметрыСеанса", RiskClass::Protection, RiskMatch::Exact},
    {"SessionParameters", RiskClass::Protection, RiskMatch::Exact},
    {"ПараметрЗапуска", RiskClass::Protection, RiskMatch::Prefix},
    {"ПараметрыЗапуска", RiskClass::Protection, RiskMatch::Prefix},
    {"LaunchParameter", RiskClass::Protection, RiskMatch::Prefix},
    {"ЗавершитьРаботуСистемы", RiskClass::Protection, RiskMatch::Exact},
    {"Exit", RiskClass::Protection, RiskMatch::Exact},
    {"ПрекратитьРаботуСистемы", RiskClass::Protection, RiskMatch::Exact},
    {"Terminate", RiskClass::Protection, RiskMatch::Prefix},
    {"Привилегирован", RiskClass::Protection, RiskMatch::Contains},
    {"Privileged", RiskClass::Protection, RiskMatch::Contains},
    {"БезопасныйРежим", RiskClass::Protection, RiskMatch::Contains},
    {"БезопасногоРежима", RiskClass::Protection, RiskMatch::Contains},
    {"SafeMode", RiskClass::Protection, RiskMatch::Contains},
    {"Монопольн", RiskClass::Protection, RiskMatch::Contains},
    {"ExclusiveMode", RiskClass::Protection, RiskMatch::Contains},
    {"Внешн", RiskClass::Protection, RiskMatch::Contains},
    {"External", RiskClass::Protection, RiskMatch::Contains},
    {"Компонент", RiskClass::Protection, RiskMatch::Contains},
    {"AddIn", RiskClass::Protection, RiskMatch::Contains},
    {"Запустить", RiskClass::Protection, RiskMatch::Prefix},
    {"RunApp", RiskClass::Protection, RiskMatch::Prefix},
    {"RunSystem", RiskClass::Protection, RiskMatch::Prefix},
    {"НачатьЗапуск", RiskClass::Protection, RiskMatch::Prefix},
    {"BeginRunning", RiskClass::Protection, RiskMatch::Prefix},
    {"КомандаСистемы", RiskClass::Protection, RiskMatch::Exact},
    {"System", RiskClass::Protection, RiskMatch::Exact},
    {"ПерейтиПо", RiskClass::Protection, RiskMatch::Prefix},
    {"GotoURL", RiskClass::Protection, RiskMatch::Prefix},
    {"ОткрытьНавигационнуюСсылку", RiskClass::Protection, RiskMatch::Exact},
    {"OpenNavigationLink", RiskClass::Protection, RiskMatch::Exact},
    // ---- Deferred
    {"ФоновыеЗадания", RiskClass::Deferred, RiskMatch::Exact},
    {"BackgroundJobs", RiskClass::Deferred, RiskMatch::Exact},
    {"РегламентныеЗадания", RiskClass::Deferred, RiskMatch::Exact},
    {"ScheduledJobs", RiskClass::Deferred, RiskMatch::Exact},
    {"ДлительныеОперации", RiskClass::Deferred, RiskMatch::Exact},
    {"TimeConsumingOperations", RiskClass::Deferred, RiskMatch::Exact},
    {"ПодключитьОбработчик", RiskClass::Deferred, RiskMatch::Prefix},
    {"ОтключитьОбработчик", RiskClass::Deferred, RiskMatch::Prefix},
    {"AttachIdleHandler", RiskClass::Deferred, RiskMatch::Prefix},
    {"AttachNotificationHandler", RiskClass::Deferred, RiskMatch::Prefix},
    {"DetachIdleHandler", RiskClass::Deferred, RiskMatch::Prefix},
    {"DetachNotificationHandler", RiskClass::Deferred, RiskMatch::Prefix},
    {"ОписаниеОповещения", RiskClass::Deferred, RiskMatch::Exact},
    {"NotifyDescription", RiskClass::Deferred, RiskMatch::Exact},
    {"ВыполнитьОбработкуОповещения", RiskClass::Deferred, RiskMatch::Exact},
    {"ExecuteNotifyProcessing", RiskClass::Deferred, RiskMatch::Exact},
    {"Оповестить", RiskClass::Deferred, RiskMatch::Prefix},
    {"Notify", RiskClass::Deferred, RiskMatch::Prefix},
    {"Начать", RiskClass::Deferred, RiskMatch::Prefix},
    {"Begin", RiskClass::Deferred, RiskMatch::Prefix},
    {"ДобавитьОбработчик", RiskClass::Deferred, RiskMatch::Exact},
    {"AddHandler", RiskClass::Deferred, RiskMatch::Exact},
    {"УдалитьОбработчик", RiskClass::Deferred, RiskMatch::Exact},
    {"RemoveHandler", RiskClass::Deferred, RiskMatch::Exact},
    // ---- Write
    {"ЗначениеВФайл", RiskClass::Write, RiskMatch::Exact},
    {"ValueToFile", RiskClass::Write, RiskMatch::Exact},
    {"ПересчитатьИтоги", RiskClass::Write, RiskMatch::Exact},
    {"RecalcTotals", RiskClass::Write, RiskMatch::Exact},
    {"RefreshTotals", RiskClass::Write, RiskMatch::Exact},
    {"RecalculateTotals", RiskClass::Write, RiskMatch::Exact},
    {"УстановитьЧасовойПояс", RiskClass::Write, RiskMatch::Exact},
    {"SetTimeZone", RiskClass::Write, RiskMatch::Exact},
    {"УстановитьРежимСовместимости", RiskClass::Write, RiskMatch::Exact},
    {"SetCompatibilityMode", RiskClass::Write, RiskMatch::Exact},
    {"УстановитьПараметрыСеанса", RiskClass::Write, RiskMatch::Exact},
    {"УдалитьРегистрациюИзменений", RiskClass::Write, RiskMatch::Exact},
    {"Записать", RiskClass::Write, RiskMatch::Prefix},
    {"Write", RiskClass::Write, RiskMatch::Prefix},
    {"Удалить", RiskClass::Write, RiskMatch::Prefix},
    {"Delete", RiskClass::Write, RiskMatch::Prefix},
    {"УстановитьПометкуУдаления", RiskClass::Write, RiskMatch::Exact},
    {"SetDeletionMark", RiskClass::Write, RiskMatch::Exact},
    {"Провести", RiskClass::Write, RiskMatch::Exact},
    {"Post", RiskClass::Write, RiskMatch::Exact},
    {"ОтменитьПроведение", RiskClass::Write, RiskMatch::Exact},
    {"UndoPosting", RiskClass::Write, RiskMatch::Exact},
    {"Установить", RiskClass::Write, RiskMatch::Prefix},
    {"Set", RiskClass::Write, RiskMatch::Prefix},
    {"Константы", RiskClass::Write, RiskMatch::Exact},
    {"Constants", RiskClass::Write, RiskMatch::Exact},
    {"НаборЗаписей", RiskClass::Write, RiskMatch::Contains},
    {"RecordSet", RiskClass::Write, RiskMatch::Contains},
    {"МенеджерЗаписи", RiskClass::Write, RiskMatch::Contains},
    {"RecordManager", RiskClass::Write, RiskMatch::Contains},
    {"ЗаписьЖурналаРегистрации", RiskClass::Write, RiskMatch::Exact},
    {"WriteLogEvent", RiskClass::Write, RiskMatch::Exact},
    {"ПоместитьВоВременноеХранилище", RiskClass::Write, RiskMatch::Exact},
    {"PutToTempStorage", RiskClass::Write, RiskMatch::Exact},
    {"УдалитьИзВременногоХранилища", RiskClass::Write, RiskMatch::Exact},
    {"DeleteFromTempStorage", RiskClass::Write, RiskMatch::Exact},
    {"ПоместитьФайл", RiskClass::Write, RiskMatch::Exact},
    {"PutFile", RiskClass::Write, RiskMatch::Exact},
    {"ЗаписьТекста", RiskClass::Write, RiskMatch::Exact},
    {"TextWriter", RiskClass::Write, RiskMatch::Exact},
    {"ЗаписьXML", RiskClass::Write, RiskMatch::Exact},
    {"XMLWriter", RiskClass::Write, RiskMatch::Exact},
    {"ЗаписьJSON", RiskClass::Write, RiskMatch::Exact},
    {"JSONWriter", RiskClass::Write, RiskMatch::Exact},
    {"ЗаписьДанных", RiskClass::Write, RiskMatch::Exact},
    {"DataWriter", RiskClass::Write, RiskMatch::Exact},
    {"ТекстовыйДокумент", RiskClass::Write, RiskMatch::Exact},
    {"TextDocument", RiskClass::Write, RiskMatch::Exact},
    {"ТабличныйДокумент", RiskClass::Write, RiskMatch::Exact},
    {"SpreadsheetDocument", RiskClass::Write, RiskMatch::Exact},
    {"ДвоичныеДанные", RiskClass::Write, RiskMatch::Exact},
    {"BinaryData", RiskClass::Write, RiskMatch::Exact},
    {"КопироватьФайл", RiskClass::Write, RiskMatch::Exact},
    {"CopyFile", RiskClass::Write, RiskMatch::Exact},
    {"ПереместитьФайл", RiskClass::Write, RiskMatch::Exact},
    {"MoveFile", RiskClass::Write, RiskMatch::Exact},
    {"УдалитьФайлы", RiskClass::Write, RiskMatch::Exact},
    {"DeleteFiles", RiskClass::Write, RiskMatch::Exact},
    {"СоздатьКаталог", RiskClass::Write, RiskMatch::Exact},
    {"CreateDirectory", RiskClass::Write, RiskMatch::Exact},
    {"ПолучитьИмяВременногоФайла", RiskClass::Write, RiskMatch::Exact},
    {"GetTempFileName", RiskClass::Write, RiskMatch::Exact},
    {"Транзакци", RiskClass::Write, RiskMatch::Contains},
    {"Transaction", RiskClass::Write, RiskMatch::Contains},
    {"Блокировк", RiskClass::Write, RiskMatch::Contains},
    {"DataLock", RiskClass::Write, RiskMatch::Contains},
    {"Заблокировать", RiskClass::Write, RiskMatch::Prefix},
    {"Разблокировать", RiskClass::Write, RiskMatch::Prefix},
    {"LockDataForEdit", RiskClass::Write, RiskMatch::Exact},
    {"UnlockDataForEdit", RiskClass::Write, RiskMatch::Exact},
    {"Lock", RiskClass::Write, RiskMatch::Exact},
    {"Unlock", RiskClass::Write, RiskMatch::Exact},
    {"ПолучитьОбъект", RiskClass::Write, RiskMatch::Exact},
    {"GetObject", RiskClass::Write, RiskMatch::Exact},
    {"СоздатьЭлемент", RiskClass::Write, RiskMatch::Exact},
    {"CreateItem", RiskClass::Write, RiskMatch::Exact},
    {"СоздатьДокумент", RiskClass::Write, RiskMatch::Exact},
    {"CreateDocument", RiskClass::Write, RiskMatch::Exact},
    {"СоздатьГруппу", RiskClass::Write, RiskMatch::Exact},
    {"CreateFolder", RiskClass::Write, RiskMatch::Exact},
    {"УстановитьНовыйНомер", RiskClass::Write, RiskMatch::Exact},
    {"SetNewNumber", RiskClass::Write, RiskMatch::Exact},
    {"УстановитьНовыйКод", RiskClass::Write, RiskMatch::Exact},
    {"SetNewCode", RiskClass::Write, RiskMatch::Exact},
    {"ЗаполнитьЗначенияСвойств", RiskClass::Write, RiskMatch::Exact},
    {"FillPropertyValues", RiskClass::Write, RiskMatch::Exact},
    {"ОбменДанными", RiskClass::Write, RiskMatch::Exact},
    {"DataExchange", RiskClass::Write, RiskMatch::Exact},
    {"Загрузка", RiskClass::Write, RiskMatch::Exact},
    {"Load", RiskClass::Write, RiskMatch::Exact},
    {"Загрузить", RiskClass::Write, RiskMatch::Prefix},
    {"Сохранить", RiskClass::Write, RiskMatch::Exact},
    {"Save", RiskClass::Write, RiskMatch::Exact},
    // ---- Credentials
    {"ЗначениеИзФайла", RiskClass::Credentials, RiskMatch::Exact},
    {"ValueFromFile", RiskClass::Credentials, RiskMatch::Exact},
    {"ПолучитьПеременнуюСреды", RiskClass::Credentials, RiskMatch::Exact},
    {"GetEnvironmentVariable", RiskClass::Credentials, RiskMatch::Exact},
    {"ПользователиИнформационнойБазы", RiskClass::Credentials, RiskMatch::Exact},
    {"InfoBaseUsers", RiskClass::Credentials, RiskMatch::Exact},
    {"ИнформационнойБаз", RiskClass::Credentials, RiskMatch::Contains},
    {"ИнформационнуюБаз", RiskClass::Credentials, RiskMatch::Contains},
    {"ИнформационныеБаз", RiskClass::Credentials, RiskMatch::Contains},
    {"ИнформационныхБаз", RiskClass::Credentials, RiskMatch::Contains},
    {"InfoBase", RiskClass::Credentials, RiskMatch::Contains},
    {"СтрокаСоединения", RiskClass::Credentials, RiskMatch::Contains},
    {"ConnectionString", RiskClass::Credentials, RiskMatch::Contains},
    {"Парол", RiskClass::Credentials, RiskMatch::Contains},
    {"Password", RiskClass::Credentials, RiskMatch::Contains},
    {"ТекущийПользователь", RiskClass::Credentials, RiskMatch::Exact},
    {"CurrentUser", RiskClass::Credentials, RiskMatch::Exact},
    {"ИмяПользователя", RiskClass::Credentials, RiskMatch::Prefix},
    {"UserName", RiskClass::Credentials, RiskMatch::Prefix},
    {"ПолноеИмяПользователя", RiskClass::Credentials, RiskMatch::Exact},
    {"UserFullName", RiskClass::Credentials, RiskMatch::Exact},
    {"ИмяКомпьютера", RiskClass::Credentials, RiskMatch::Exact},
    {"ComputerName", RiskClass::Credentials, RiskMatch::Exact},
    {"ИнтернетПрокси", RiskClass::Credentials, RiskMatch::Exact},
    {"InternetProxy", RiskClass::Credentials, RiskMatch::Exact},
    {"HTTPСоединение", RiskClass::Credentials, RiskMatch::Exact},
    {"HTTPConnection", RiskClass::Credentials, RiskMatch::Exact},
    {"HTTPЗапрос", RiskClass::Credentials, RiskMatch::Exact},
    {"HTTPRequest", RiskClass::Credentials, RiskMatch::Exact},
    {"HTTP", RiskClass::Credentials, RiskMatch::Prefix},
    {"FTP", RiskClass::Credentials, RiskMatch::Prefix},
    {"ИнтернетПочт", RiskClass::Credentials, RiskMatch::Prefix},
    {"InternetMail", RiskClass::Credentials, RiskMatch::Prefix},
    {"COMОбъект", RiskClass::Credentials, RiskMatch::Contains},
    {"СОМОбъект", RiskClass::Credentials, RiskMatch::Contains},
    {"COMObject", RiskClass::Credentials, RiskMatch::Contains},
    {"WSПрокси", RiskClass::Credentials, RiskMatch::Exact},
    {"WSProxy", RiskClass::Credentials, RiskMatch::Exact},
    {"WSОпределения", RiskClass::Credentials, RiskMatch::Exact},
    {"WSDefinitions", RiskClass::Credentials, RiskMatch::Exact},
    {"ЗащищенноеСоединение", RiskClass::Credentials, RiskMatch::Prefix},
    {"OpenSSL", RiskClass::Credentials, RiskMatch::Contains},
    {"ВнешниеИсточникиДанных", RiskClass::Credentials, RiskMatch::Exact},
    {"ExternalDataSources", RiskClass::Credentials, RiskMatch::Exact},
    {"УстановитьОбщиеПараметрыСоединения", RiskClass::Credentials, RiskMatch::Exact},
    {"SetCommonConnectionSettings", RiskClass::Credentials, RiskMatch::Exact},
    {"Соединени", RiskClass::Credentials, RiskMatch::Contains},
    {"Connection", RiskClass::Credentials, RiskMatch::Contains},
    {"Сертификат", RiskClass::Credentials, RiskMatch::Contains},
    {"Certificate", RiskClass::Credentials, RiskMatch::Contains},
    {"Криптограф", RiskClass::Credentials, RiskMatch::Contains},
    {"Crypto", RiskClass::Credentials, RiskMatch::Contains},
    {"ЧтениеТекста", RiskClass::Credentials, RiskMatch::Exact},
    {"TextReader", RiskClass::Credentials, RiskMatch::Exact},
    {"ЧтениеДанных", RiskClass::Credentials, RiskMatch::Exact},
    {"DataReader", RiskClass::Credentials, RiskMatch::Exact},
    {"ЧтениеXML", RiskClass::Credentials, RiskMatch::Exact},
    {"XMLReader", RiskClass::Credentials, RiskMatch::Exact},
    {"Файл", RiskClass::Credentials, RiskMatch::Prefix},
    {"File", RiskClass::Credentials, RiskMatch::Prefix},
    {"НайтиФайлы", RiskClass::Credentials, RiskMatch::Exact},
    {"FindFiles", RiskClass::Credentials, RiskMatch::Exact},
    {"ПолучитьФайл", RiskClass::Credentials, RiskMatch::Prefix},
    {"GetFile", RiskClass::Credentials, RiskMatch::Prefix},
    {"Каталог", RiskClass::Credentials, RiskMatch::Prefix},
};

}  // namespace lineage::bsl
