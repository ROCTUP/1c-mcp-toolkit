# Tools Full Reference

Complete parameter tables, curl examples, and response structures for all 8 REST API endpoints.

> **Convention**: all examples use the variables from Quick Start:
> ```sh
> BASE_HOST=localhost
> BASE_URL="http://$BASE_HOST:6003"
> CHANNEL="default"
> J='-H Content-Type:application/json'
> ```

---

## 1. get_metadata — `GET/POST /api/get_metadata`

Explore database structure. Summary/list/details modes depend on parameters; the configuration scope is controlled via `extension_name`.

Request rules:
- **GET**: parameters come from the URL query string; the request body is ignored
- **POST**: parameters come from the JSON body; the query string is ignored **except** `?channel=<id>`

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `filter` | string | null | — | Exact object name for detailed structure (e.g., `Справочник.Номенклатура`) or full path to a collection element (e.g., `Справочник.Контрагенты.Реквизит.ИНН`) |
| `meta_type` | string or string[] | null | Use `"*"` for all types | Root metadata type(s): Справочник, Документ, РегистрСведений, РегистрНакопления, РегистрБухгалтерии, РегистрРасчета, ПланВидовХарактеристик, ПланСчетов, ПланВидовРасчета, ПланОбмена, БизнесПроцесс, Задача, Константа, Перечисление, Отчет, Обработка, РегламентноеЗадание, ПараметрыСеанса |
| `name_mask` | string | null | — | Case-insensitive search in name/synonym |
| `limit` | integer | 100 | 1–1000 | Max objects in list mode |
| `offset` | integer | 0 | 0–1000000 | Pagination offset in list mode |
| `sections` | string[] | null | Requires `filter` | Detail sections: `properties`, `forms`, `commands`, `layouts`, `predefined`, `movements`, `characteristics`. Note: `movements` only applies to `Документ` objects — silently ignored for other types |
| `extension_name` | string | null | No whitespace-only | `null`=main config, `""`=list extensions, `"Name"`=extension objects |

### Mode 1: Summary (no filter/meta_type/name_mask)

```sh
# Root type counts
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL"
```

Response:
```json
{
  "success": true,
  "configuration": {
    "platform_version": "8.3.25.1000",
    "infobase_name": "MyDB",
    "metadata": {"Имя": "MyConfiguration", "Синоним": "My Configuration"}
  },
  "data": [
    {"Тип": "Справочник", "Количество": 265},
    {"Тип": "Документ", "Количество": 27},
    {"Тип": "РегистрСведений", "Количество": 150}
  ]
}
```

Notes:
- If you call summary **inside a specific extension** (`extension_name="MyExtension"` and no `filter/meta_type/name_mask`), the response includes the same `data` (root type counts) plus top-level fields `extension` and `configuration`.
- `extension_name=""` is **not** summary — it returns the list of connected extensions (see “Extensions” below).

### Mode 2: List (meta_type and/or name_mask, without filter)

```sh
# All documents containing "реализ"
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "meta_type=Документ" \
  --data-urlencode "name_mask=реализ" \
  --data-urlencode "limit=50"

# Multiple types via comma in GET
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "meta_type=Документ,РегистрСведений" \
  --data-urlencode "limit=50" \
  --data-urlencode "offset=0"

# All objects across all types
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "meta_type=*" \
  --data-urlencode "limit=200"

# POST variant
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"meta_type":"Справочник","name_mask":"номенклат","limit":50}'

# Multiple types via POST (array)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"meta_type":["Документ","РегистрСведений"],"limit":200}'
```

Response:
```json
{
  "success": true,
  "truncated": false,
  "limit": 50,
  "offset": 0,
  "returned": 2,
  "has_more": false,
  "next_offset": 2,
  "data": [
    {"ПолноеИмя": "Документ.РеализацияТоваровУслуг", "Синоним": "Реализация товаров и услуг"},
    {"ПолноеИмя": "Документ.РеализацияОтгруженныхТоваров", "Синоним": "Реализация отгруженных товаров"}
  ]
}
```

### Mode 3: Detail (filter specified)

```sh
# Basic detail
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "filter=РегистрНакопления.ОстаткиТоваров"

# Rich card with all sections
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Справочник.Номенклатура","sections":["properties","forms","commands","layouts","predefined","movements","characteristics"]}'
```

Response (detail):
```json
{
  "success": true,
  "data": {
    "ТипОбъектаМетаданных": "РегистрНакопления",
    "Имя": "ОстаткиТоваров",
    "Синоним": "Остатки товаров",
    "ПолноеИмя": "РегистрНакопления.ОстаткиТоваров",
    "Измерения": [
      {"Имя": "Номенклатура", "Синоним": "Номенклатура", "Тип": "СправочникСсылка.Номенклатура"},
      {"Имя": "Склад", "Синоним": "Склад", "Тип": "СправочникСсылка.Склады"}
    ],
    "Ресурсы": [
      {"Имя": "Количество", "Синоним": "Количество", "Тип": "Число(15,3)"}
    ],
    "Реквизиты": [
      {"Имя": "Партия", "Синоним": "Партия", "Тип": "СправочникСсылка.Партии"}
    ]
  }
}
```

### Mode 3a: Collection element (filter with full path)

Collection names use the platform `ПолноеИмя()` singular format: `Реквизит`, `Измерение`, `Ресурс`, `ТабличнаяЧасть`, `СтандартныйРеквизит`, `РеквизитАдресации`.

```sh
# Catalog attribute
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Справочник.Контрагенты.Реквизит.ИНН"}'

# Register dimension with extended properties
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"РегистрНакопления.Остатки.Измерение.Номенклатура","sections":["properties"]}'

# Nested tabular section attribute
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Документ.Реализация.ТабличнаяЧасть.Товары.Реквизит.Номенклатура"}'
```

Response (collection element):
```json
{
  "success": true,
  "data": {
    "ПолноеИмя": "Справочник.Контрагенты.Реквизит.ИНН",
    "Имя": "ИНН",
    "Синоним": "ИНН",
    "Тип": "Строка(12)"
  }
}
```

### Extensions

```sh
# List all extensions
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"extension_name":""}'

# Objects inside a specific extension (list mode)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"extension_name":"MyExtension","meta_type":"Справочник"}'
```

Extension list response (`extension_name=""`):
```json
{
  "success": true,
  "data": [
    {"Имя": "MyExtension", "Синоним": "My Extension", "УникальныйИдентификатор": "a1b2c3d4-..."}
  ]
}
```

Note: for `extension_name="MyExtension"` (specific extension), responses include a top-level `extension` field in all modes (summary/list/details). In list mode it looks like this:
```json
{
  "success": true,
  "extension": "MyExtension",
  "truncated": false,
  "limit": 50,
  "returned": 2,
  "count": 2,
  "offset": 0,
  "has_more": false,
  "next_offset": 2,
  "data": [
    {"ПолноеИмя": "Справочник.МойСправочник", "Синоним": "Мой справочник"}
  ]
}
```

---

## 2. execute_query — `POST /api/execute_query`

Execute 1C query language queries and return results.

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `query` | string | **required** | min 1 char | 1C query language text |
| `params` | object | null | — | Query parameters as key-value pairs |
| `limit` | integer | 100 | 1–1000 | Max rows to return |
| `include_schema` | boolean | false | Strict boolean only | Include column type schema in response |

### Examples

```sh
# Simple query
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Код, Наименование ИЗ Справочник.Номенклатура","limit":5}'

# With parameters
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ * ИЗ Справочник.Контрагенты ГДЕ Наименование ПОДОБНО &Маска","params":{"Маска":"%Рога%"},"limit":10}'

# With schema (useful for understanding data types)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ ПЕРВЫЕ 0 * ИЗ Справочник.Номенклатура","include_schema":true}'

# Selecting references (returns object_description in rows)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка, Контрагент ИЗ Документ.РеализацияТоваровУслуг","limit":3}'
```

Response (with schema):
```json
{
  "success": true,
  "data": [
    {"Код": "001", "Наименование": "Товар 1", "Цена": 100.50}
  ],
  "schema": {
    "columns": [
      {"name": "Код", "types": ["Строка"]},
      {"name": "Наименование", "types": ["Строка"]},
      {"name": "Цена", "types": ["Число"]}
    ]
  },
  "count": 1
}
```

Response (with object_description in data):
```json
{
  "success": true,
  "data": [
    {
      "Ссылка": {
        "_objectRef": true,
        "УникальныйИдентификатор": "a1b2c3d4-5678-9012-3456-789012345678",
        "ТипОбъекта": "ДокументСсылка.РеализацияТоваровУслуг",
        "Представление": "Реализация №001 от 15.01.2024"
      },
      "Контрагент": {
        "_objectRef": true,
        "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
        "ТипОбъекта": "СправочникСсылка.Контрагенты",
        "Представление": "ООО Рога и Копыта"
      }
    }
  ],
  "count": 1
}
```

### Tip: explore table structure without loading data

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ ПЕРВЫЕ 0 * ИЗ Документ.РеализацияТоваровУслуг","include_schema":true}'
```

---

## 3. execute_code — `POST /api/execute_code`

Execute arbitrary 1C code (statement block via `Выполнить`).

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `code` | string | **required** | min 1 char | 1C code to execute |

### Rules

- Must assign result to `Результат` variable: `Результат = <expression>;`
- Cannot declare `Процедура` / `Функция`
- Cannot use `Возврат`

### Dangerous keywords

The following keywords are blocked by default (configurable via `DANGEROUS_KEYWORDS` env var):

`Удалить`, `Delete`, `Записать`, `Write`, `УстановитьПривилегированныйРежим`, `SetPrivilegedMode`, `ПодключитьВнешнююКомпоненту`, `AttachAddIn`, `УстановитьВнешнююКомпоненту`, `InstallAddIn`, `COMОбъект`, `COMObject`, `УстановитьМонопольныйРежим`, `SetExclusiveMode`, `УдалитьФайлы`, `DeleteFiles`, `КопироватьФайл`, `CopyFile`, `ПереместитьФайл`, `MoveFile`, `СоздатьКаталог`, `CreateDirectory`

Behavior when dangerous keyword detected:
- `ALLOW_DANGEROUS_WITH_APPROVAL=false` (default): returns `success=false` immediately
- `ALLOW_DANGEROUS_WITH_APPROVAL=true`: sends to 1C with `requires_approval=true`, waits for user decision in 1C UI

### Examples

```sh
# Simple expression
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Результат = ТекущаяДата();"}'

# Multi-line code
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Запрос = Новый Запрос; Запрос.Текст = \"ВЫБРАТЬ 1 КАК Поле\"; Результат = Запрос.Выполнить().Выгрузить().Количество();"}'

# With extended timeout (for long operations)
curl --max-time 200 -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Результат = 1;"}'
```

Response:
```json
{
  "success": true,
  "data": "2024-01-15T10:30:00"
}
```

Error response (dangerous keyword):
```json
{
  "success": false,
  "error": "Код содержит потенциально опасные операции: ['Записать']. Выполнение запрещено / Code contains potentially dangerous operations: ['Записать']. Execution denied."
}
```

---

## 4. get_object_by_link — `POST /api/get_object_by_link`

Retrieve complete 1C object data by navigation link.

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `link` | string | **required** | Must match `e1cib/data/Type.Name?ref=HexGUID32` | Navigation link |

### Link format

```
e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5
         ├─ prefix: e1cib/data/
         ├─ type:   Справочник.Контрагенты
         └─ ref:    32 hex characters (HexGUID)
```

### Examples

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_object_by_link?channel=$CHANNEL" $J \
  -d '{"link":"e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5"}'
```

Response:
```json
{
  "success": true,
  "data": {
    "_type": "Справочник.Контрагенты",
    "_presentation": "ООО Рога и Копыта",
    "Код": "001",
    "Наименование": "ООО Рога и Копыта",
    "ИНН": "7701234567",
    "КонтактныеЛица": [
      {"Имя": "Иванов И.И.", "Телефон": "+7-999-123-45-67"}
    ]
  }
}
```

---

## 5. get_link_of_object — `POST /api/get_link_of_object`

Generate a navigation link from an object description.

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `object_description` | object | **required** | Must have `_objectRef`, `УникальныйИдентификатор`, `ТипОбъекта` | Object description from execute_query results. See [object-description-format.md](object-description-format.md). |

### Examples

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_link_of_object?channel=$CHANNEL" $J \
  -d '{"object_description":{"_objectRef":true,"УникальныйИдентификатор":"ba7e5a3d-1234-5678-9abc-def012345678","ТипОбъекта":"СправочникСсылка.Контрагенты"}}'
```

Response (**`data` is a string**, not an object):
```json
{
  "success": true,
  "data": "e1cib/data/Справочник.Контрагенты?ref=ba7e5a3d12345678def012345678"
}
```

---

## 6. find_references_to_object — `POST /api/find_references_to_object`

Find all references to a given object across metadata collections.

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `target_object_description` | object | **required** | See [object-description-format.md](object-description-format.md) | Object to search for |
| `search_scope` | string[] | **required** | Min 1 element | Areas to search |
| `meta_filter` | object | null | — | Filter by metadata objects |
| `meta_filter.names` | string[] | null | Format: `ТипМетаданных.ИмяОбъекта` | Exact metadata names (priority over name_mask) |
| `meta_filter.name_mask` | string | null | — | Substring search in name/synonym |
| `limit_hits` | integer | 200 | 1–10000 | Max total hits |
| `limit_per_meta` | integer | 20 | 1–1000 | Max hits per metadata object |
| `timeout_budget_sec` | integer | 30 | 5–300 | Time budget in seconds |

### Valid search_scope values

| Value | Searches in |
|-------|-------------|
| `documents` | Documents (attributes, tabular sections) |
| `catalogs` | Catalogs (attributes, tabular sections) |
| `information_registers` | Information registers (dimensions, resources, attributes) |
| `accumulation_registers` | Accumulation registers (dimensions, resources, attributes) |
| `accounting_registers` | Accounting registers (dimensions, resources, attributes) |
| `calculation_registers` | Calculation registers (dimensions, resources, attributes) |

### Examples

```sh
# Find documents referencing a customer
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/find_references_to_object?channel=$CHANNEL" $J \
  -d '{
    "target_object_description": {
      "_objectRef": true,
      "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
      "ТипОбъекта": "СправочникСсылка.Контрагенты"
    },
    "search_scope": ["documents"],
    "limit_hits": 50
  }'

# Search everywhere with meta_filter
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/find_references_to_object?channel=$CHANNEL" $J \
  -d '{
    "target_object_description": {
      "_objectRef": true,
      "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
      "ТипОбъекта": "СправочникСсылка.Номенклатура"
    },
    "search_scope": ["documents", "accumulation_registers", "information_registers"],
    "meta_filter": {"names": ["Документ.РеализацияТоваровУслуг", "РегистрНакопления.ОстаткиТоваров"]},
    "limit_per_meta": 10,
    "timeout_budget_sec": 60
  }'

# Quick check: is the object referenced at all?
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/find_references_to_object?channel=$CHANNEL" $J \
  -d '{
    "target_object_description": {
      "_objectRef": true,
      "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
      "ТипОбъекта": "СправочникСсылка.Контрагенты"
    },
    "search_scope": ["documents", "catalogs", "information_registers", "accumulation_registers"],
    "limit_hits": 1,
    "timeout_budget_sec": 10
  }'
```

### Response structure

```json
{
  "success": true,
  "data": {
    "hits": [
      {
        "found_in_meta": "Документ.РеализацияТоваровУслуг",
        "found_in_object": {
          "_objectRef": true,
          "УникальныйИдентификатор": "...",
          "ТипОбъекта": "ДокументСсылка.РеализацияТоваровУслуг",
          "Представление": "Реализация №001 от 01.01.2024"
        },
        "path": "Контрагент",
        "match_kind": "attribute",
        "note": "Реализация №001 от 01.01.2024"
      },
      {
        "found_in_meta": "РегистрНакопления.ОстаткиТоваров",
        "found_in_object": null,
        "record_key": {
          "Номенклатура": {"_objectRef": true, "ТипОбъекта": "СправочникСсылка.Номенклатура", "Представление": "Товар А"},
          "Склад": {"_objectRef": true, "ТипОбъекта": "СправочникСсылка.Склады", "Представление": "Основной"},
          "Период": "2024-01-15T00:00:00"
        },
        "path": "Номенклатура",
        "match_kind": "dimension",
        "note": "Номенклатура=Товар А; Склад=Основной; Период=15.01.2024"
      }
    ],
    "total_hits": 2,
    "candidates_checked": 12,
    "timeout_exceeded": false,
    "skipped_names": []
  }
}
```

Hit fields for documents/catalogs: `found_in_object` is an object_description, `match_kind` is `attribute` or `tabular_section`.

Hit fields for registers: `found_in_object` is null, `record_key` contains dimensions/period/registrar, `match_kind` is `dimension`, `resource`, or `requisite`.

---

## 7. get_access_rights — `POST /api/get_access_rights`

Get role permissions for a metadata object, optionally with effective rights for a user.

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `metadata_object` | string | **required** | Format `Type.Name`, must contain dot | Full metadata object name |
| `user_name` | string | null | Case-insensitive search | User name (from IB users or Пользователи catalog) |
| `rights_filter` | string[] | null | — | Show only these rights (null = default list for object type) |
| `roles_filter` | string[] | null | Case-insensitive match | Show only these roles (null = all roles with rights) |

### Limitations

- `effective_rights` is "sum of roles", NOT a guarantee of actual access
- Row-Level Security (RLS) is NOT taken into account
- Contextual restrictions (by organizations, departments) are NOT considered
- Admin rights required for user-specific queries
- Privileged mode forbidden (would make all results `true`)

### Examples

```sh
# Role permissions for a catalog
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_access_rights?channel=$CHANNEL" $J \
  -d '{"metadata_object":"Справочник.Контрагенты"}'

# With user effective rights
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_access_rights?channel=$CHANNEL" $J \
  -d '{"metadata_object":"Документ.РеализацияТоваровУслуг","user_name":"Иванов"}'

# Filtered by specific rights and roles
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_access_rights?channel=$CHANNEL" $J \
  -d '{"metadata_object":"Справочник.Контрагенты","rights_filter":["Чтение","Изменение","Добавление","Удаление"],"roles_filter":["ПолныеПрава","Менеджер"]}'
```

Response:
```json
{
  "success": true,
  "data": {
    "metadata_object": "Справочник.Контрагенты",
    "metadata_type": "Справочник",
    "applicable_rights": ["Чтение", "Изменение", "Добавление", "Удаление", "Просмотр", "ИнтерактивноеДобавление"],
    "roles": [
      {
        "name": "Менеджер",
        "rights": {"Чтение": true, "Изменение": true, "Добавление": true, "Удаление": false}
      },
      {
        "name": "ПолныеПрава",
        "rights": {"Чтение": true, "Изменение": true, "Добавление": true, "Удаление": true}
      }
    ],
    "total_roles": 2,
    "roles_with_rights": 2,
    "user": {
      "name": "Иванов",
      "full_name": "Иванов Иван Иванович",
      "roles": ["Менеджер"],
      "effective_rights": {
        "Чтение": true,
        "Изменение": true,
        "Добавление": true,
        "Удаление": false
      }
    }
  }
}
```

---

## 8. get_event_log — `POST /api/get_event_log`

Get event log entries with filtering and cursor-based pagination.

### Parameters

| Parameter | Type | Default | Constraints | Description |
|-----------|------|---------|-------------|-------------|
| `start_date` | string | null | ISO 8601: `YYYY-MM-DDTHH:MM:SS` | Start date |
| `end_date` | string | null | ISO 8601: `YYYY-MM-DDTHH:MM:SS` | End date |
| `levels` | string[] | null | `Information`, `Warning`, `Error`, `Note` | Importance levels |
| `events` | string[] | null | e.g., `_$Data$_.New`, `_$Data$_.Update` | Event types |
| `limit` | integer | 100 | 1–1000 | Max records per page |
| `same_second_offset` | integer | 0 | 0–10000, requires `start_date` | Skip N records at same second (for pagination) |
| `object_description` | object | null | See [object-description-format.md](object-description-format.md) | Filter by object (priority 1) |
| `link` | string | null | `e1cib/data/...?ref=HexGUID32` | Filter by nav link (priority 2) |
| `data` | string | null | — | Filter by nav link (priority 3, backward compat) |
| `metadata_type` | string or string[] | null | e.g., `Документ.РеализацияТоваровУслуг` | Filter by metadata object type |
| `user` | string[] | null | — | Filter by user name(s) |
| `session` | integer[] | null | — | Filter by session number(s) |
| `application` | string[] | null | See valid values below | Filter by application type |
| `computer` | string | null | — | Filter by computer name |
| `comment_contains` | string | null | — | Substring search in comments |
| `transaction_status` | string | null | `Committed`, `RolledBack`, `NotApplicable`, `Unfinished` | Transaction status filter |

### Valid application values

`ThinClient`, `WebClient`, `ThickClient`, `BackgroundJob`, `Designer`, `COMConnection`, `Server`, `WebService`, `HTTPService`, `ODataInterface`, `MobileAppClient`, `MobileAppServer`, `MobileAppBackgroundJob`, `MobileClient`, `MobileStandaloneServer`, `FileVariantBackgroundJob`, `FileVariantServerSide`, `WebSocket`, `FileVariantWebSocket`, `1CV8C`, `1CV8`

### Examples

```sh
# Errors in a date range
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-01T00:00:00","end_date":"2024-01-31T23:59:59","levels":["Error","Warning"],"limit":100}'

# Events for a specific user
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-15T00:00:00","user":["Иванов"],"limit":50}'

# Events for a specific object (using object_description)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"object_description":{"_objectRef":true,"УникальныйИдентификатор":"ba7e5a3d-1234-5678-9abc-def012345678","ТипОбъекта":"СправочникСсылка.Контрагенты"},"limit":50}'

# Events for a specific metadata type
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-01T00:00:00","metadata_type":["Документ.РеализацияТоваровУслуг"],"limit":100}'
```

Response:
```json
{
  "success": true,
  "data": [
    {
      "date": "2024-01-15T10:30:00",
      "level": "Error",
      "event": "_$Data$_.Update",
      "comment": "Ошибка при проведении документа",
      "user": "Иванов",
      "metadata": "Документ.РеализацияТоваровУслуг",
      "data_presentation": "Реализация №001 от 15.01.2024",
      "session": 12345,
      "application": "ThinClient",
      "computer": "WORKSTATION01",
      "transaction_status": "RolledBack"
    }
  ],
  "count": 1,
  "last_date": "2024-01-15T10:30:00",
  "next_same_second_offset": 1,
  "has_more": true
}
```

### Cursor pagination

To get the next page, use `last_date` as `start_date` and `next_same_second_offset` as `same_second_offset`:

```sh
# Page 1
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-01T00:00:00","levels":["Error"],"limit":100}'
# → returns last_date="2024-01-15T10:30:00", next_same_second_offset=3, has_more=true

# Page 2
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-15T10:30:00","same_second_offset":3,"levels":["Error"],"limit":100}'
# → returns last_date="2024-01-20T15:45:00", next_same_second_offset=1, has_more=false
```

**Stop condition**: `has_more=false` means no more records.
