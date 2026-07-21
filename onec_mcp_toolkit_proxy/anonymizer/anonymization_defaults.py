"""
Default anonymization policy values collected in one place.

This module is the single source of truth for default lists, dictionaries and
markers used by anonymization logic. Runtime flags and env-based overrides stay
in config.py; regexes and algorithms stay in their implementation modules.
"""

# Default MCP/REST tools whose responses are anonymized.
# Controls which tool results are post-processed by anonymization by default.
DEFAULT_ANONYMIZATION_TOOLS = (
    "execute_query",
    "execute_code",
    "get_object_by_link",
    "get_event_log",
    "find_references_to_object",
    "get_access_rights",
)


# Technical top-level response keys whose values stay unchanged.
# Prevents rewriting service metadata on the first response level.
TOP_LEVEL_SKIP_KEYS = frozenset({
    "success", "truncated", "has_more", "count", "limit",
    "returned", "offset", "next_offset", "last_date",
    "next_same_second_offset",
    # execute_query schema: column names/types are technical metadata, not PII
    "schema",
})


# ---------------------------------------------------------------------------
# Key-aware detection defaults
# ---------------------------------------------------------------------------

# Key substrings that explicitly mark fields as technical/non-sensitive.
# Used only when no sensitive key rule matched first.
_DEFAULT_NON_SENSITIVE_KEY_SUBSTRINGS = frozenset({
    "файл", "модуль", "шаблон",
    "таблиц", "колонк", "измерени", "ресурс",
    "реквизит", "макет", "конфигурац", "расширени",
    "ключ", "индекс", "нумератор", "количеств",
    "metadata", "config", "template", "column", "field",
})

# Key substrings that classify values by sensitivity category.
# These are the main defaults for key-aware anonymization.
_DEFAULT_SENSITIVE_KEY_SUBSTRINGS = {
    # Personal identifiers
    "инн": "INN",         "inn": "INN",
    "кпп": "KPP",         "kpp": "KPP",
    "огрн": "OGRN",       "ogrn": "OGRN",
    "снилс": "SNILS",     "snils": "SNILS",
    "страховойномер": "SNILS",
    "паспорт": "PASSPORT", "passport": "PASSPORT",
    "телефон": "PHONE",    "phone": "PHONE",
    "email": "EMAIL",
    "фио": "PER",          "fio": "PER",
    "фамилия": "PER",      "lastname": "PER",
    "инициалы": "PER",
    "отчество": "PER",     "middlename": "PER",
    "firstname": "PER",
    "датарожд": "DATE",
    # Free-text fields (usually not needed for the agent; may contain PII)
    "комментарий": "STR",
    "содержание": "STR",
    "назначениеплатежа": "STR",
    "описание": "STR",
    # Organizations / counterparties
    "контрагент": "ORG",    "counterparty": "ORG",
    "корреспондент": "ORG",
    "организация": "ORG",   "organization": "ORG",
    # Responsible persons
    "ответственный": "PER",  "автор": "PER",
    "исполнител": "PER",
    "изменил": "PER",
    "редакт": "PER",
    "физлицо": "PER",
    "физическоелицо": "PER",
    "месторожд": "LOC",
    # Could be person or organization
    "получатель": "STR",     "отправитель": "STR",
    # 1C special fields
    "_presentation": "STR",
    # Document issuer (passport / identity docs): key-aware only
    "кемвыдан": "STR",

    # Addresses (key-aware only; inline address detection is out of scope)
    # Avoid generic "адрес" substring because it matches too much noise.
    "адреспо": "LOC",           # АдресПоПрописке...
    "адреспропис": "LOC",       # АдресПрописки...
    "адреспрож": "LOC",         # АдресПроживания...
    "адресрег": "LOC",          # АдресРегистрации...
    "адресфакт": "LOC",         # АдресФактический...
    "адресдостав": "LOC",       # АдресДоставки...
    "адресполуч": "LOC",        # АдресПолучателя...
    "адресместа": "LOC",        # АдресМестаПроживания/Работы...
    "адресдляинформ": "LOC",    # АдресДляИнформирования...
    "адресорганизац": "LOC",    # АдресОрганизации...
    "адресместонахожд": "LOC",  # АдресМестонахождения...
    "адресмеждународ": "LOC",   # АдресМеждународный...
    "местонахожд": "LOC",       # Местонахождение...

    # Address components (explicit keys / substrings)
    "город": "LOC",
    "страна": "LOC",
    "улиц": "LOC",
    "корпус": "LOC",
    "квартир": "LOC",
    "регион": "LOC",
}

# Exact key names with fixed anonymization category.
# Used before substring matching for special neutral cases.
_DEFAULT_SENSITIVE_KEY_EXACT = {
    # Neutral classification: we want to anonymize, but avoid misleading the agent.
    "имя": "STR",
}

# Keys whose values must not be whole-string tokenized.
# Inline anonymization is also skipped for them except for "error".
SKIP_KEYS = frozenset({
    "_ref", "_objectRef", "_type",
    "УникальныйИдентификатор", "ТипОбъекта",
    "success", "error", "truncated", "has_more",
    "count", "limit", "returned", "offset",
    "next_offset", "last_date", "next_same_second_offset",
    "session", "level", "event", "application",
    "transaction_status",
})

# Key prefixes that mark values as technical/status-like and usually safe.
# A match here suppresses whole-value anonymization.
_DEFAULT_SKIP_PREFIXES = (
    "алгоритм", "база",
    "вариант", "важность", "вероятность", "вид",
    "действие", "единица",
    "категория", "код",
    "метод", "модуль",
    "настройка", "ндс", "ндфл",
    "порядок", "полфизическ", "причина", "признак", "приоритет",
    "раздел", "результат", "режим", "роль",
    "счетучета",
    "способ", "состояние", "ставка", "статус", "степень",
    "тип",
    "формат", "форма",
    "характер", "цвет", "цель", "частота",
    "уровень",
    "этап", "юрфизлицо", "юридическоефизическоелицо",
    "level", "event", "application", "transaction_status",
)

# Literal scalar values that should never be anonymized.
# Protects common booleans, statuses and enum-like text from false positives.
KNOWN_SAFE_VALUES = frozenset({
    "Да", "Нет", "Истина", "Ложь", "True", "False", "true", "false",
    "Проведен", "НеПроведен", "Записан",
    "Committed", "RolledBack", "NotApplicable", "Unfinished",
    "Information", "Warning", "Error", "Note",
    "USD", "EUR", "руб.",
    "Евро", "Доллар США", "Российский рубль",
})


# ---------------------------------------------------------------------------
# Dictionary-based anonymization defaults
# ---------------------------------------------------------------------------

# 1C sources used to preload canonical names for dictionary replacement.
# Each source adds known terms for ORG/PER tokenization.
DEFAULT_DICTIONARY_SOURCES = [
    {"from": "Справочник.Контрагенты",    "category": "ORG", "extra_fields": ["НаименованиеПолное"]},
    {"from": "Справочник.Организации",    "category": "ORG", "extra_fields": ["НаименованиеПолное"]},
    {"from": "Справочник.ФизическиеЛица", "category": "PER", "extra_fields": ["Фамилия", "Имя", "Отчество", "НаименованиеСлужебное"]},
    {"from": "Справочник.Сотрудники",     "category": "PER", "extra_fields": []},
    {"from": "Справочник.Пользователи",   "category": "PER", "extra_fields": []},
]

# Key substrings where dictionary matching is allowed to run.
# Limits dictionary replacement to name/presentation-like fields.
DICT_APPLY_KEY_SUBSTRINGS = (
    "наименован",
    "представлен",
    "_presentation",
    "note",
)


# ---------------------------------------------------------------------------
# Context heuristics defaults
# ---------------------------------------------------------------------------

# Name-like keys that may be force-tokenized from surrounding object context.
FORCE_NAME_KEYS = frozenset({
    "наименование",
    "наименованиеполное",
    "представление",
    "_presentation",
})

# Column-name anchors that indicate organization-related groups in flat query rows.
ROW_GROUP_ORG_ANCHOR_SUBSTRINGS = (
    "контрагент",
    "корреспондент",
    "организац",
    "покупател",
    "поставщик",
    "партнер",
    "counterparty",
    "correspondent",
    "organization",
    "buyer",
    "seller",
    "partner",
)

# Evidence substrings that prove a flat row group belongs to an organization.
# Used with anchors to safely force ORG anonymization for adjacent name fields.
ROW_GROUP_ORG_EVIDENCE_KEY_SUBSTRINGS = (
    "инн", "inn",
    "кпп", "kpp",
    "огрн", "ogrn",
)

# Generic name keys allowed for single-group ORG fallback in flat query rows.
# Prevents over-tokenizing unrelated "name" columns.
ROW_GROUP_ORG_GENERIC_NAME_KEYS = frozenset({
    "наименование",
    "полноенаименование",
    "краткоенаименование",
    "представление",
})

# Type markers inside object references that mean the object is an organization/counterparty.
COUNTERPARTY_TYPE_MARKERS = (
    "контрагенты",
    "организации",
    "корреспонденты",
    "counterparties",
    "organizations",
)

# Type markers inside object references that mean the object is a person/user/employee.
PERSON_TYPE_MARKERS = (
    "физическиелица",
    "сотрудники",
    "пользователи",
    "employees",
    "users",
)


# ---------------------------------------------------------------------------
# Guardrails for NER and error-text handling
# ---------------------------------------------------------------------------

# Technical substrings that help reject false NER matches in identifiers and errors.
_1C_TECH_ENTITY_SUBSTRINGS = (
    # 1C metadata/object kinds + UI/module plumbing
    "справочник", "документ", "регистр", "перечислен", "отчет", "обработк",
    "внешняяобработка", "форма", "модуль",
    "catalog", "document", "register", "enum", "report", "dataprocessor",
    # common query/field identifiers that must stay readable in errors
    "ссылка", "наименован", "пометка", "удален",
    "юридическ", "адрес", "телефон", "email", "инн", "кпп", "огрн", "снилс", "паспорт",
    # project-specific tech names that often appear in stack traces
    "mcp", "toolkit",
)

# Stopword substrings that disable NER replacement inside 1C error text.
# Protects query syntax, metadata names and technical fragments from tokenization.
_NER_ERROR_STOPWORD_SUBSTRINGS = (
    # 1C query keywords / syntax tokens that must never be NER-tokenized
    "выбрать", "из ", "из", "где", "упорядоч", "по ", "первые", "первых", "убыв",
    "объедин", "все",
    "как ", "как", "левое", "соединен", "подобно", "не ",
    # common 1C field names used in queries and errors
    "ссылка", "код", "наименован", "пометкаудаления", "организац",
    # common business/process fields that appear in queries (identifiers, not values)
    "дата", "автор", "тема", "ответствен", "исполнител",
    # common personal field names (these are identifiers in queries, not values)
    "фамил", "имя", "отчеств", "датарожд", "страховойномер", "пфр",
    # org/HR structure identifiers
    "подраздел", "должност",
    # typical error/stack technical words
    "поле", "не найден", "ошибка", "метод", "контекст", "выполнить",
    # accounting register / query identifiers
    "регистратор", "период", "субконто", "сумма", "регистрбухгалтер", "хозрасчет",
    # common document fields that are identifiers in queries, not values
    "номер", "контрагент", "водител", "комментарий", "доверенност", "содержание",
    # contact info / group flags
    "контактнаяинформац", "этогруппа",
)


# ---------------------------------------------------------------------------
# Общие исключения из inline-анонимизации технических слов в поле "error"
# ---------------------------------------------------------------------------

# Точные технические слова из собственных статических сообщений приложения,
# которые нельзя анонимизировать внутри поля "error" (защита от ложных
# срабатываний inline-детекторов, напр. 8-буквенных слов под SWIFT-регуляркой).
# Единый набор для всех детекторов: не делить на SWIFT_/ORG_/PERSON_ и т. п.
# Новое техническое слово добавляется только сюда. Сравнение точное и
# регистрозависимое — другой регистр при необходимости добавлять явно.
ERROR_INLINE_ANONYMIZATION_EXCLUSIONS = frozenset({
    "rejected",
    "contains",
    "keywords",
})


def is_error_inline_anonymization_exclusion(candidate: str, parent_key) -> bool:
    """True — если найденный фрагмент нельзя токенизировать внутри поля "error".

    Категория детектора в решении не участвует: одно слово защищено независимо
    от того, классифицировал его SWIFT / ORG / PERSON или будущий детектор.
    Сопоставление точное и регистрозависимое.
    """
    return parent_key == "error" and candidate in ERROR_INLINE_ANONYMIZATION_EXCLUSIONS
