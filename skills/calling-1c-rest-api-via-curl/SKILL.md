---
name: calling-1c-rest-api-via-curl
description: >
  Access a 1C:Enterprise database through the 1C MCP Toolkit REST API using curl.
  Provides 8 endpoints under /api/ for querying data (execute_query), exploring metadata
  (get_metadata), reading event logs (get_event_log), checking access rights
  (get_access_rights), finding object references (find_references_to_object), navigating
  objects by link (get_object_by_link, get_link_of_object), and executing 1C code
  (execute_code). Use when the agent needs to interact with a 1C database via HTTP but
  does not speak MCP. Supports channel isolation for multi-database routing.
---

# 1C MCP Toolkit REST API via curl

REST API under `/api/*` mirrors the MCP tools. No authentication required. All endpoints use the same channel routing and validation logic as MCP.

## Setup

```sh
BASE_HOST=localhost
BASE_URL="http://$BASE_HOST:6003"
CHANNEL="default"
J='-H Content-Type:application/json'

# Health check
curl -sS --noproxy $BASE_HOST "$BASE_URL/health"
```

Set `curl --max-time` above the proxy timeout (default 180s), e.g. `--max-time 200`.

## Core Concepts

### Response Envelope

Every response is JSON with this structure:

```json
{"success": true, "data": <result>}
{"success": false, "error": "description"}
```

Some endpoints add extra fields: `count`, `truncated`, `has_more`, `last_date`, `next_same_second_offset`, `configuration`, `schema`.

### TOON Format

Controlled by server env `RESPONSE_FORMAT` (default: `toon`):
- **toon** (default): `data` is a TOON-encoded **string** inside the JSON envelope (saves 30–60% tokens)
- **json**: `data` is a normal JSON value (array/object/primitive)

The outer envelope (`success`, `error`, extra fields) is always standard JSON regardless of format.

### Object Description

A JSON structure that identifies a 1C object reference. It flows between tools:

```json
{
  "_objectRef": true,
  "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
  "ТипОбъекта": "СправочникСсылка.Контрагенты",
  "Представление": "ООО Рога и Копыта"
}
```

- **Appears as output** in: `execute_query` results, `find_references_to_object` hits
- **Required as input** for: `get_link_of_object`, `find_references_to_object`, `get_event_log` (object filter)

See [references/object-description-format.md](references/object-description-format.md) for full specification.

### Channel Routing

Pass `?channel=<name>` in the URL query string. Regex: `^[a-zA-Z0-9_-]{1,64}$`. Default: `default`.

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=dev" $J -d '{"query":"ВЫБРАТЬ 1"}'
```

---

## Tools Quick Reference

> Full parameter tables, all curl variations, and complete response examples: [references/tools-full-reference.md](references/tools-full-reference.md)

### 1. get_metadata — `GET/POST /api/get_metadata`

Explore database structure. Four modes:

| Mode | Params | Returns |
|------|--------|---------|
| **Summary** | none | Root types with counts + configuration info |
| **List** | `meta_type` and/or `name_mask` | Array of `{ПолноеИмя, Синоним}` with pagination |
| **Detail** | `filter` = full object name | Full object structure (attributes, dimensions, tabular sections) |
| **Collection element** | `filter` = full path to element | Single element info (e.g., `Справочник.Контрагенты.Реквизит.ИНН`) |

Key params: `filter`, `meta_type` (string or array, `"*"` for all types), `name_mask`, `sections` (requires filter): `properties`/`forms`/`commands`/`layouts`/`predefined`/`movements`/`characteristics` (`movements` only for `Документ`), `limit` (default 100, max 1000), `offset`, `extension_name`.

Request rules:
- **GET**: parameters come from the URL query string; request body is ignored
- **POST**: parameters come from the JSON body; query string is ignored **except** `?channel=<id>`

Notes:
- For **GET**, use `--data-urlencode` for each parameter (except `channel`). To list extensions via GET, pass an explicit empty value: `--data-urlencode "extension_name="`.
- For `extension_name="MyExtension"` (specific extension, not `""`), responses include a top-level `extension` field in all modes (summary/list/details).

```sh
# Summary
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL"

# List documents matching "реализ"
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "meta_type=Документ" \
  --data-urlencode "name_mask=реализ"

# Detail with sections (POST mode)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Справочник.Номенклатура","sections":["properties"]}'

# Collection element filter (Mode 3a)
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Справочник.Контрагенты.Реквизит.ИНН","sections":["properties"]}'
```

---

### 2. execute_query — `POST /api/execute_query`

Execute 1C query language queries. For query language syntax, table naming, and optimization rules consult the `composing-1c-queries` skill.

Key params: `query` (**required**), `params` (key-value), `limit` (default 100, max 1000), `include_schema` (boolean, adds column types).

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка, Наименование ИЗ Справочник.Контрагенты","limit":5}'
```

Response:
```json
{
  "success": true,
  "data": [
    {
      "Ссылка": {"_objectRef": true, "УникальныйИдентификатор": "ba7e5a3d-...", "ТипОбъекта": "СправочникСсылка.Контрагенты", "Представление": "ООО Рога и Копыта"},
      "Наименование": "ООО Рога и Копыта"
    }
  ],
  "count": 1
}
```

**Tip**: Use `"ВЫБРАТЬ ПЕРВЫЕ 0 * ИЗ ..."` with `include_schema: true` to inspect table structure without loading data.

---

### 3. execute_code — `POST /api/execute_code`

Execute arbitrary 1C code. Must assign result to `Результат` variable. Cannot declare `Процедура`/`Функция` or use `Возврат`.

**Dangerous keywords** blocked by default: `Удалить`, `Delete`, `Записать`, `Write`, `УстановитьПривилегированныйРежим`, `SetPrivilegedMode`, `COMОбъект`, `COMObject`, `УдалитьФайлы`, `DeleteFiles`, and others (see [references/tools-full-reference.md](references/tools-full-reference.md#3-execute_code--post-apiexecute_code)).

**CRITICAL: Query text inside execute_code must be a single line.** Always write the entire query in one line without line breaks. Do not use multiline formatting.

```
// CORRECT:
Запрос.Текст = "ВЫБРАТЬ Ссылка, Наименование ИЗ Справочник.Контрагенты ГДЕ НЕ ПометкаУдаления";
```

```sh
# Simple code (no query):
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Результат = ТекущаяДата();"}'

# Code with query — single line:
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_code?channel=$CHANNEL" $J \
  -d '{"code":"Запрос = Новый Запрос;\nЗапрос.Текст = \"ВЫБРАТЬ Ссылка, Наименование ИЗ Справочник.Контрагенты ГДЕ НЕ ПометкаУдаления\";\nРезультат = Запрос.Выполнить().Выгрузить().Количество();"}'
```

Response: `{"success": true, "data": "2024-01-15T10:30:00"}`

---

### 4. get_object_by_link — `POST /api/get_object_by_link`

Retrieve complete object data by navigation link.

Link format: `e1cib/data/Type.Name?ref=<32 hex chars>`

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_object_by_link?channel=$CHANNEL" $J \
  -d '{"link":"e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5"}'
```

Response: `{"success": true, "data": {"_type": "Справочник.Контрагенты", "Код": "001", "Наименование": "ООО Рога и Копыта", ...}}`

---

### 5. get_link_of_object — `POST /api/get_link_of_object`

Generate a navigation link from an `object_description` (from execute_query results).

**Note**: on success, `data` is a **string** (navigation link), not an object.

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_link_of_object?channel=$CHANNEL" $J \
  -d '{"object_description":{"_objectRef":true,"УникальныйИдентификатор":"ba7e5a3d-1234-5678-9abc-def012345678","ТипОбъекта":"СправочникСсылка.Контрагенты"}}'
```

Response: `{"success": true, "data": "e1cib/data/Справочник.Контрагенты?ref=ba7e5a3d12345678..."}`

---

### 6. find_references_to_object — `POST /api/find_references_to_object`

Find all references to an object across the database.

Key params: `target_object_description` (**required**, see [object description](#object-description)), `search_scope` (**required**, array from: `documents`, `catalogs`, `information_registers`, `accumulation_registers`, `accounting_registers`, `calculation_registers`), `meta_filter` (`{names, name_mask}`), `limit_hits` (default 200), `limit_per_meta` (default 20), `timeout_budget_sec` (default 30).

```sh
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
```

Response:
```json
{
  "success": true,
  "data": {
    "hits": [
      {"found_in_meta": "Документ.РеализацияТоваровУслуг", "found_in_object": {"_objectRef": true, "...": "..."}, "path": "Контрагент", "match_kind": "attribute", "note": "Реализация №001"}
    ],
    "total_hits": 1, "candidates_checked": 5, "timeout_exceeded": false, "skipped_names": []
  }
}
```

**Quick check** — is an object used at all? Set `limit_hits: 1`.

---

### 7. get_access_rights — `POST /api/get_access_rights`

Get role permissions for a metadata object, optionally with effective user rights.

Key params: `metadata_object` (**required**, format `Type.Name`), `user_name` (case-insensitive search by IB user or Пользователи catalog), `rights_filter`, `roles_filter`.

**Limitations**: `effective_rights` = sum of roles (RLS, contextual restrictions NOT considered). Admin rights required.

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_access_rights?channel=$CHANNEL" $J \
  -d '{"metadata_object":"Справочник.Контрагенты","user_name":"Иванов"}'
```

Response:
```json
{
  "success": true,
  "data": {
    "metadata_object": "Справочник.Контрагенты",
    "roles": [{"name": "Менеджер", "rights": {"Чтение": true, "Изменение": true, "Удаление": false}}],
    "user": {"name": "Иванов", "roles": ["Менеджер"], "effective_rights": {"Чтение": true, "Изменение": true, "Удаление": false}}
  }
}
```

---

### 8. get_event_log — `POST /api/get_event_log`

Read event log entries with cursor-based pagination.

Key params: `start_date`, `end_date` (ISO 8601: `YYYY-MM-DDTHH:MM:SS`), `levels` (`Information`/`Warning`/`Error`/`Note`), `events`, `limit` (default 100, max 1000), `user`, `metadata_type`, `application`, `computer`, `comment_contains`, `transaction_status` (`Committed`/`RolledBack`/`NotApplicable`/`Unfinished`), `object_description`/`link` (filter by object).

**Pagination**: use `last_date` → `start_date` and `next_same_second_offset` → `same_second_offset` for next page. Stop when `has_more=false`.

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-01T00:00:00","levels":["Error","Warning"],"limit":100}'
```

Response:
```json
{
  "success": true,
  "data": [{"date": "2024-01-15T10:30:00", "level": "Error", "event": "_$Data$_.Update", "comment": "...", "user": "Иванов", "metadata": "Документ.РеализацияТоваровУслуг"}],
  "count": 1, "last_date": "2024-01-15T10:30:00", "next_same_second_offset": 1, "has_more": false
}
```

---

## Workflow Patterns

> Full curl commands and responses at each step: [references/workflow-examples.md](references/workflow-examples.md)

### Workflow 1: Explore an Unfamiliar Database

**Trigger**: "what's in this database", "show me the structure"

1. `GET /api/health` — verify connection
2. `GET /api/get_metadata` — get root summary (types + counts) and configuration info
3. `GET /api/get_metadata?meta_type=Документ` — list objects of a specific type (use --data-urlencode for params)
4. `POST /api/get_metadata` with `filter` + `sections` — get detailed structure of an object
5. `POST /api/execute_query` — sample real data

### Workflow 2: Investigate Object Dependencies

**Trigger**: "where is this object used", "can I delete this"

1. `POST /api/execute_query` — find the object, extract `object_description` from result
2. `POST /api/find_references_to_object` — find all references (tip: `limit_hits: 1` for quick yes/no)
3. `POST /api/get_access_rights` — check who can modify/delete
4. `POST /api/get_event_log` with `object_description` — check recent change history

### Workflow 3: Diagnose Event Log Errors

**Trigger**: "what errors happened", "investigate recent problems"

1. `POST /api/get_event_log` with `levels: ["Error"]` and date range — fetch errors
2. Paginate: use `last_date` + `next_same_second_offset` until `has_more=false`
3. `POST /api/get_object_by_link` or `execute_query` — examine objects mentioned in errors
4. `POST /api/execute_query` — gather context data around problematic objects

---

## Error Handling

### HTTP status codes

| Status | Meaning | Action |
|--------|---------|--------|
| 200 + `success:true` | OK | Use `data` |
| 200 + `success:false` | Business error / timeout from 1C | Read `error` string |
| 400 | Invalid JSON | Fix request body |
| 415 | Wrong Content-Type for POST | Add `-H Content-Type:application/json` |
| 422 | Validation error | Read `error` and `details`, fix params |
| 500 | Server error | Check proxy logs |

### Agent parsing logic

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J -d '{"query":"ВЫБРАТЬ 1"}' \
  | jq -r 'if .success then .data else ("ERROR: " + .error) end'
```

### Timeout

Default proxy timeout is 180s. Set `curl --max-time` slightly above (e.g., 200). For `execute_code` with long operations, increase both.

---

## String Escaping Rules for curl

When building curl commands with JSON payloads containing 1C code and queries, multiple quoting levels are involved. Follow these rules to avoid errors.

### Rule 1: Use single quotes for `-d` payload (recommended)

Single quotes prevent bash from interpreting `$`, `!`, `&`, backticks and other special characters inside the payload. Double quotes also work but require extra care with special characters.

```sh
# Recommended — single quotes, bash doesn't touch anything inside:
curl ... -d '{"query":"ВЫБРАТЬ 1"}'

# Also works, but bash interprets special characters — be careful:
curl ... -d "{\"query\":\"ВЫБРАТЬ 1\"}"
```

### Rule 2: Always use parameters for string values in queries

Instead of embedding string literals directly in query text (which requires complex escaping), **always pass them as query parameters**. This avoids nested quoting entirely.

```sh
# GOOD — value passed as parameter, no nested quoting:
curl ... -d '{"query":"ВЫБРАТЬ Ссылка ИЗ Справочник.Контрагенты ГДЕ Статус = &Статус", "params":{"Статус":"Активный"}}'

# GOOD — same for execute_code:
curl ... -d '{"code":"Запрос = Новый Запрос;\nЗапрос.Текст = \"ВЫБРАТЬ Ссылка ИЗ Справочник.Контрагенты ГДЕ Статус = &Статус\";\nЗапрос.УстановитьПараметр(\"Статус\", \"Активный\");\nРезультат = Запрос.Выполнить().Выгрузить();"}'
```

### Rule 3: Avoid `!` in string values

Bash interprets `!` as history expansion even inside some quote contexts. **Never use `!` in string literals.** Replace with safe alternatives.

```sh
# BAD — ! triggers history expansion:
-d '{"code":"...ТОГДА \"!!! ВЫСОКАЯ\"..."}'

# GOOD — no exclamation marks:
-d '{"code":"...ТОГДА \"ВЫСОКАЯ\"..."}'
```

### Rule 4: String literals inside query (edge case)

If you absolutely must embed a string literal in query text without a parameter, the escaping depends on context:

**execute_query** — one level of JSON escaping (`\"`):
```sh
curl ... -d '{"query":"ВЫБРАТЬ Ссылка ИЗ Справочник.Контрагенты ГДЕ Наименование ПОДОБНО \"%Рога%\""}'
```

**execute_code** — the 1C string `""` escaping + JSON escaping (`\"\"`):
```sh
curl ... -d '{"code":"Запрос.Текст = \"ВЫБРАТЬ Ссылка ИЗ Справочник.Контрагенты ГДЕ Наименование ПОДОБНО \"\"%Рога%\"\"\";"}'
```

**Prefer Rule 2 (parameters) over this whenever possible.**

### Quick reference

| Character | Problem | Solution |
|-----------|---------|----------|
| `"` inside query string | Nested escaping | Pass value as parameter (Rule 2) |
| `!` | Bash history expansion | Avoid completely |
| `&` | Bash interprets in double quotes | Safe inside single-quoted `-d` payload |
| `\n` | Line break in JSON string | Use for separating 1C statements, NOT inside query text |

---

## References

- [Full tool reference](references/tools-full-reference.md) — complete parameter tables, all curl variations, response structures
- [Object description format](references/object-description-format.md) — the `object_description` structure and how it flows between tools
- [Workflow examples](references/workflow-examples.md) — detailed multi-step workflows with full curl commands and responses
