# Tools Full Reference

Complete parameter tables, curl examples, and response structures for all 12 REST API endpoints.

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
| `types_limit` | integer | 100 for a typed field | 1–200; single typed field only | Page size for the full list of allowed types; does not change the 20-type preview in `Тип` |
| `types_offset` | integer | 0 for a typed field | 0–1000000; single typed field only | Offset in the allowed types list; continue with `data.types.next_offset` while `data.types.has_more=true` |
| `sections` | string[] | null | Requires `filter`; incompatible with `attribute_mask` | Detail sections: `properties`, `forms`, `commands`, `layouts`, `predefined`, `movements`, `characteristics`. Note: `movements` only applies to `Документ` objects — silently ignored for other types |
| `extension_name` | string | null | No whitespace-only | `null`=main config, `""`=list extensions, `"Name"`=extension objects |
| `attribute_mask` | string | null | Incompatible with `sections` | Case-insensitive substring search across all attribute names/synonyms (реквизиты, измерения, ресурсы, реквизиты ТЧ). Returns same list contract as Mode 2. Compatible with `meta_type`, `name_mask`, `filter` (root object only), `extension_name`. `extension_name=""` takes priority (returns extension list, ignores `attribute_mask`). |

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

#### Compact type descriptions in structures

Object and tabular-section structures still return all fields. Each field's `Тип` contains at most **20 actual type representations**, including qualifiers such as `Число(15,3)`. It contains no count labels or truncation messages.

- With at most 20 types, the representation and platform order stay unchanged.
- With more than 20 types, `Тип` contains the first 20 representations in deterministic, case-insensitive sorted order.
- Counters are decided **per array** (`Реквизиты`, `Измерения`, `Ресурсы`, each tabular section's `Реквизиты`, etc.). If any field has more than 20 types, **all rows of that array** get scalar `total_types_count` (all allowed types) and `shown_types_count` (types included in `Тип`). Otherwise neither column is emitted. Standard and user attributes in the same array share this decision.
- For successfully read types, equal counters mean the full list is shown; unequal counters mean more types are available. Absent counters mean no field in that array was shortened. A row with `type_error` must not be interpreted as a complete or empty type list; its counters, when present, are `null`.

These flat, uniform counter columns preserve eligibility for TOON tables; no nested type-page object is added to structure rows. Other existing fields such as `extension_marks` may independently affect TOON formatting. Fetch full type pages for a single field using [Mode 6](#mode-6-paginated-types-of-one-typed-field).

### Mode 3a: Collection element (filter with full path)

Collection names use singular segment names: `Реквизит`, `Измерение`, `Ресурс`, `ТабличнаяЧасть`, `СтандартныйРеквизит`, `РеквизитАдресации`.

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
    "Тип": "Строка(12)",
    "types": {
      "count": 1,
      "limit": 100,
      "offset": 0,
      "returned": 1,
      "truncated": false,
      "has_more": false,
      "next_offset": 1,
      "items": ["Строка(12)"]
    }
  }
}
```

The `types` block is included for a typed field even when `types_limit` and `types_offset` are omitted. See [Mode 6](#mode-6-paginated-types-of-one-typed-field) for pagination. An element without a type description has no `types` block; addressing a whole tabular section returns its field structure instead.

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

### Mode 5: Attribute search (attribute_mask)

Search all attribute names/synonyms across all objects (or scoped to one object via `filter`).
Returns the same list contract as Mode 2. `ПолноеИмя` uses singular segment names and can be passed directly to `filter` for round-trip detail lookup.

```sh
# Find all attributes whose name or synonym contains "контраг"
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"attribute_mask":"контраг"}'

# Scoped to one object
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"attribute_mask":"дата","filter":"Документ.Реализация"}'

# With pagination
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"attribute_mask":"сумма","limit":50,"offset":0}'

# Round-trip: find attribute, then get its detail
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"attribute_mask":"контраг","limit":1}'
# → data[0]["ПолноеИмя"] = "Документ.Реализация.Реквизит.Контрагент"
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Документ.Реализация.Реквизит.Контрагент","sections":["properties"]}'
```

Response:
```json
{
  "success": true,
  "truncated": false,
  "limit": 100,
  "offset": 0,
  "returned": 2,
  "count": 2,
  "has_more": false,
  "next_offset": 2,
  "data": [
    {"ПолноеИмя": "Документ.Реализация.Реквизит.Контрагент", "Синоним": "Контрагент"},
    {"ПолноеИмя": "Справочник.Контрагенты.Реквизит.ОсновнойДоговор", "Синоним": "Основной договор"}
  ]
}
```

Notes:
- **Incompatible with `sections`** — returns error. Use round-trip instead: get `ПолноеИмя` from attribute search, then pass it to `filter` with `sections`.
- Compatible with `meta_type` (restrict object types), `name_mask` (filter object names), `filter` (restrict to one root object), `extension_name` (specific extension).
- `extension_name=""` (list extensions) takes priority — `attribute_mask` is ignored in that case.

### Mode 6: Paginated types of one typed field

Use an existing full field path in `filter`, such as `Справочник.Объект.Реквизит.Имя`, a register's `Измерение`/`Ресурс`, `СтандартныйРеквизит`, `РеквизитАдресации`, or `Документ.Объект.ТабличнаяЧасть.Товары.Реквизит.Имя`. The same contract applies inside a named extension; keep `extension_name` and `filter` unchanged between pages.

The response has a fixed `data.Тип` preview (at most 20 types, following the structure rules) and a separate `data.types` page. Pagination changes only `types`, not the preview. The default page is **100 types starting at offset 0**, including when both new arguments are omitted. `total_types_count`/`shown_types_count` are structure-row counters; the addressed field reports its total in `data.types.count`.

| Field in `data.types` | Meaning |
|-----------------------|---------|
| `count` | Total number of allowed types for this field |
| `limit` | Effective page size |
| `offset` | Requested offset |
| `returned` | Number of entries in this page's `items` |
| `truncated` | `returned < count`; this page alone does not contain the entire list |
| `has_more` | `offset + returned < count`; more entries follow this page |
| `next_offset` | `offset + returned`; use for the next request when `has_more=true` |
| `items` | Array of full type representation strings, including qualifiers, in deterministic case-insensitive sorted order |

**Stop on `has_more=false`, not on `truncated=false`.** The last page of a multi-page list still has `truncated=true`. An offset at or beyond `count` returns an empty `items` array, `returned=0`, `has_more=false`, and `next_offset` equal to the requested offset.

```sh
# First page of one field's full type list (POST).
# Use a field that exists in the connected configuration.
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Справочник.ВидыПроверок.Реквизит.Свойство1","types_limit":200,"types_offset":0}'

# Next page (GET): 200 here is next_offset from the preceding response.
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "filter=Справочник.ВидыПроверок.Реквизит.Свойство1" \
  --data-urlencode "types_limit=200" \
  --data-urlencode "types_offset=200"

# Alternatively, fetch only what a shortened structure row has not shown.
# 20 here is that row's shown_types_count; continue using types.next_offset.
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Справочник.ВидыПроверок.Реквизит.Свойство1","types_limit":100,"types_offset":20}'
```

To collect a machine-readable full list, start at offset 0 and concatenate page `items`. Do not split `Тип` on commas: qualifiers such as `Число(15,3)` also contain commas. Starting at the structure row's `shown_types_count` is useful when the preview is already sufficient for the first 20 types and only the rest need inspection.

`types` belongs to the **single-field response**, not to every row of `Реквизиты` or other structure arrays. In TOON mode, `data` is a string: decode TOON before reading `data.types`; in JSON mode it is an object. The response example in Mode 3a shows the decoded/JSON shape.

Validation:

- `types_limit` accepts 1–200; `types_offset` accepts 0–1000000. Omitted or JSON `null` values use the defaults for an addressed typed field. Explicit `types_offset=0` still counts as supplying a pagination argument; `types_limit=0` is invalid.
- These arguments require a single typed field. Whole-object and whole-tabular-section structure requests, and elements without a type description, reject them.
- They are incompatible with nonempty `attribute_mask`, `meta_type`, `name_mask`, and with `extension_name=""` (extension listing). A specific named extension is supported.
- POST requires JSON numbers: booleans, numeric strings and fractional numbers are rejected; whole-valued numbers such as `100.0` are accepted. GET uses integer query-string values; different repeated values for the same type-pagination parameter are rejected. Send each parameter once.
- Ordinary `limit`/`offset` do not paginate types and do not change the default type page. To change that page, use `types_limit`/`types_offset`.

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
| `execution_context` | string | `"server"` | `"server"` or `"client"` | Execution context: `"server"` — `&НаСервереБезКонтекста` (DB access, 1C objects); `"client"` — `&НаКлиенте` (form attributes, UI functions, no DB queries) |

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
# Simple expression (server context, default)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Результат = ТекущаяДата();"}'

# Multi-line code
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Запрос = Новый Запрос; Запрос.Текст = \"ВЫБРАТЬ 1 КАК Поле\"; Результат = Запрос.Выполнить().Выгрузить().Количество();"}'

# Client context — open a form, read form attributes
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"ОткрытьФорму(\"Справочник.Номенклатура.ФормаСписка\"); Результат = \"OK\";","execution_context":"client"}'

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

---

## 9. get_bsl_syntax_help — `POST /api/get_bsl_syntax_help`

Search the built-in BSL language reference. Returns candidates (breadcrumb paths); when exactly one candidate matches, returns Markdown content. Requires SyntaxHelpReader component loaded on the 1C side.

### Parameters

| Parameter | Type | Required | Default | Constraints | Description |
|-----------|------|----------|---------|-------------|-------------|
| `keywords` | string[] | Yes | — | Non-empty array | Search terms, or a single exact candidate path / link target |
| `match` | string | No | `"all"` | `"all"` / `"any"` | `"all"`: all keywords must appear; `"any"`: any keyword matches |
| `limit` | integer | No | 100 | 1–300 | Max candidates per page |
| `offset` | integer | No | 0 | 0–1000000 | Candidates to skip (pagination) |
| `content_page` | integer | No | 1 | ≥ 1 | Page of content to return (1-based) |

### Search logic

- Multiple candidates → `content` is `null`. Narrow keywords or use a candidate path directly.
- Exactly one candidate → `content` contains Markdown reference page.
- Candidate paths (e.g. `Массив/Методы/Найти`) and link targets from content (`topic:Path`) can be passed as `keywords` for exact lookup — pass the full string including `topic:` prefix as-is.

### Examples

```sh
# Broad search
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_bsl_syntax_help?channel=$CHANNEL" $J \
  -d '{"keywords":["Найти"]}'

# Narrow to methods of a type
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_bsl_syntax_help?channel=$CHANNEL" $J \
  -d '{"keywords":["Найти","Массив"]}'

# Exact lookup by candidate path
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_bsl_syntax_help?channel=$CHANNEL" $J \
  -d '{"keywords":["Массив/Методы/Найти"]}'

# Follow a link from content (topic: prefix passed as-is)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_bsl_syntax_help?channel=$CHANNEL" $J \
  -d '{"keywords":["topic:Массив/Методы/Найти"]}'

# Find all methods of a type
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_bsl_syntax_help?channel=$CHANNEL" $J \
  -d '{"keywords":["ТаблицаЗначений","Методы"]}'

# Get next content page
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_bsl_syntax_help?channel=$CHANNEL" $J \
  -d '{"keywords":["Запрос"],"content_page":2}'
```

### Response — multiple candidates

```json
{
  "success": true,
  "data": {
    "candidates": ["Массив/Методы/Найти", "Строка/Методы/Найти"],
    "total": 2,
    "offset": 0,
    "limit": 100,
    "has_more": false,
    "content": null
  }
}
```

### Response — one match, single content page

```json
{
  "success": true,
  "data": {
    "candidates": ["Массив/Методы/Найти"],
    "total": 1,
    "offset": 0,
    "limit": 100,
    "has_more": false,
    "content": "# Найти\n\n**Синтаксис:** `Найти(<Что>)`\n\n...",
    "content_page": 1,
    "content_total_pages": 1,
    "content_has_more": false
  }
}
```

### Response — one match, content paginated

```json
{
  "success": true,
  "data": {
    "candidates": ["Запрос"],
    "total": 1,
    "has_more": false,
    "content": "# Запрос\n\n...",
    "content_page": 1,
    "content_total_pages": 4,
    "content_has_more": true
  }
}
```

Fields `content_page`, `content_total_pages`, `content_has_more` are present **only when `content` is not null**.

---

## 10. submit_for_deanonymization — `POST /api/submit_for_deanonymization`

Submit the final user-facing response for de-anonymization display. **Available only when anonymization is enabled.**

> **Note:** This tool returns `{"received": true}` on success (not `{"success": true, "data": ...}`).

### Parameters

| Parameter | Type | Required | Default | Constraints | Description |
|-----------|------|----------|---------|-------------|-------------|
| `text` | string | Yes | - | String (empty allowed) | The complete final response text containing anonymization tokens |

### Examples

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/submit_for_deanonymization?channel=$CHANNEL" $J \
  -d '{"text":"Компания [ORG-00001], ИНН [INN-00001], директор: [PER-00001]"}'
```

Response (success):
```json
{
  "received": true
}
```

Error (anonymization disabled):
```json
{
  "success": false,
  "error": "Tool is not available: anonymization is disabled"
}
```

Error (missing or invalid parameter):
```json
{
  "success": false,
  "error": "Ошибка валидации параметров: Field 'text': ..."
}
```

Error (wrong method, built-in mode):
```json
{
  "success": false,
  "error": "Method not allowed: submit_for_deanonymization requires POST"
}
```

---

## 11. restart_1c_session — `POST /api/restart_1c_session`

Restart the current 1C session. A new session starts automatically with the same database and connection settings; anonymization state is preserved. The old session shuts down once the new one is ready.

> **IMPORTANT**: Do NOT call on your own initiative. Only invoke when explicitly instructed by the user or as a defined step in a pipeline specification.

### Parameters

None. Send an empty body `{}` or omit the body entirely.

### Timeout

The operation can take up to 120 seconds (new session startup). Use `curl --max-time 200` (or above the proxy timeout).

### Example

```sh
curl --max-time 200 -sS --noproxy $BASE_HOST "$BASE_URL/api/restart_1c_session?channel=$CHANNEL" $J \
  -d '{}'
```

### Response (success)

```json
{
  "success": true,
  "data": "Session restarted successfully. Note: the first MCP request to the new session may fail - retry once if it does."
}
```

### Error responses

```json
{"success": false, "error": "Restart timeout: new session did not start in 120 seconds"}
{"success": false, "error": "Failed to launch new session: <OS error detail>"}
{"success": false, "error": "Cannot restart: data processor path is not available (not running from file?)"}
{"success": false, "error": "Cannot restart: data processor file not found: /path/to/file.epf"}
{"success": false, "error": "Windows authentication is not available on the current OS"}
```

HTTP 500 (concurrent call):
```json
{"success": false, "error": "Restart or close already in progress"}
```

---

## 12. close_1c_session — `POST /api/close_1c_session`

Close the current 1C session and receive a launcher script command to start a new one. Use when exclusive database access is needed (e.g., configuration update).

On success the session closes immediately, and `data` contains the shell command to launch a fresh session. Run it synchronously: exit 0 = session ready; non-zero = startup failed.

> **IMPORTANT**: Do NOT call on your own initiative. Only invoke when explicitly instructed.

### Parameters

None. Send an empty body `{}` or omit the body entirely.

### Timeout

Use `curl --max-time 200`. The endpoint responds as soon as the launcher script is prepared and the old session begins shutting down (fast).

### Example

```sh
curl --max-time 200 -sS --noproxy $BASE_HOST "$BASE_URL/api/close_1c_session?channel=$CHANNEL" $J \
  -d '{}'
```

### Response (success)

`data` is a multi-line **string** containing the command and usage notes:

```json
{
  "success": true,
  "data": "Session closed. To start a new session, run:\npowershell -ExecutionPolicy Bypass -File 'C:\\path\\to\\launcher.ps1'\nRun synchronously. Exit 0 = session ready. Non-zero = startup failed. On Windows, run this command in PowerShell (not cmd.exe). On timeout: startup state is unknown — check whether 1C was already started before launching another. On non-timeout exit 1: the launcher either did not start 1C, or the failed new instance was closed — safe to retry."
}
```

### Launcher script behavior

| Exit code | Meaning |
|-----------|---------|
| 0 | New session started successfully |
| 1 (non-timeout) | Pre-launch error or failed startup — safe to retry after fixing the cause |
| 1 (timeout after 120 s) | State unknown — check whether a 1C process was already started before launching another |

- **Windows**: run the command in PowerShell (not cmd.exe)
- **Linux with password auth**: `python3` must be available on PATH

### Error responses

```json
{"success": false, "error": "Cannot close: data processor path not available"}
{"success": false, "error": "Cannot close: data processor file not found: /path/to/file.epf"}
{"success": false, "error": "Windows authentication is not available on the current OS"}
{"success": false, "error": "close_1c_session error: <detail>"}
```

HTTP 500 (concurrent call):
```json
{"success": false, "error": "Restart or close already in progress"}
```


