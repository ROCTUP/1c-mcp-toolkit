---
name: composing-1c-queries
description: >
  Compose correct and optimized 1C:Enterprise query language queries. Covers
  ВЫБРАТЬ/ИЗ/ГДЕ structure, table naming (catalogs, documents, registers,
  virtual tables), field selection, joins, grouping, totals, temporary tables,
  and parameter usage. Use when the agent needs to compose a 1C query for
  execute_query, or when the user asks to query, filter, aggregate, or analyze
  data from a 1C database. Triggered by requests involving 1C data retrieval,
  report building, or query debugging.
---

# Composing 1C Queries

Rules and patterns for writing correct 1C:Enterprise query language (язык запросов 1С).
Queries are executed via the `execute_query` tool/endpoint.

## CRITICAL: Verify Metadata Before Querying

**NEVER guess or invent metadata object names, attribute names, or tabular section names.** 1C configurations vary greatly — object and field names are unique to each database.

Before composing a query, if you are not certain about the exact names, **use `get_metadata` or any other available tool to retrieve 1C configuration metadata** to discover:
- Available metadata objects (catalogs, documents, registers, etc.)
- Object attributes (реквизиты) and their types
- Tabular sections (табличные части) and their columns
- Register dimensions, resources, and attributes

**Workflow:** retrieve metadata → confirm names → compose query → execute query

A query with a wrong object or field name will fail with a runtime error. Spending one call on metadata retrieval is always cheaper than debugging a failed query.

**Formatting:** Always write the entire query text in a **single line**. Do not use line breaks or multiline formatting.

## Query Structure

Canonical order of clauses (required section marked, rest optional):

```
ВЫБРАТЬ [РАЗРЕШЕННЫЕ] [РАЗЛИЧНЫЕ] [ПЕРВЫЕ <N>]
    <field list>
ИЗ
    <data sources>
[ГДЕ <condition>]
[СГРУППИРОВАТЬ ПО <fields>]
[ИМЕЮЩИЕ <condition>]

[ОБЪЕДИНИТЬ [ВСЕ]
ВЫБРАТЬ ...]

[УПОРЯДОЧИТЬ ПО <fields> [ВОЗР | УБЫВ]]
[АВТОУПОРЯДОЧИВАНИЕ]

[ИТОГИ <aggregate expressions> ПО [ОБЩИЕ] <control points>]
```

**Keywords:**

| Keyword | Meaning |
|---------|---------|
| `ВЫБРАТЬ` | SELECT — mandatory, starts the query |
| `РАЗРЕШЕННЫЕ` | Respect row-level security (RLS). Use by default for safety |
| `РАЗЛИЧНЫЕ` | DISTINCT — remove duplicate rows |
| `ПЕРВЫЕ N` | Return only first N rows (1C has no LIMIT keyword — this is the only way) |
| `ИЗ` | FROM — data sources |
| `ГДЕ` | WHERE — row filter |
| `СГРУППИРОВАТЬ ПО` | GROUP BY |
| `ИМЕЮЩИЕ` | HAVING — group filter |
| `ОБЪЕДИНИТЬ [ВСЕ]` | UNION [ALL] |
| `УПОРЯДОЧИТЬ ПО` | ORDER BY. `ВОЗР` = ASC, `УБЫВ` = DESC |
| `АВТОУПОРЯДОЧИВАНИЕ` | Auto-order by presentation (rarely used) |
| `ИТОГИ` | TOTALS — compute aggregate subtotals |

Comments: `// single-line only`.

Minimal example:

```
ВЫБРАТЬ Ссылка, Наименование
ИЗ Справочник.Контрагенты
ГДЕ НЕ ПометкаУдаления
```

## Table Names and Sources

Every 1C metadata object has a fixed query-name pattern. Use exact names from `get_metadata`.

### Real tables

| Object type | Query name pattern | Example |
|-------------|-------------------|---------|
| Catalog | `Справочник.<Name>` | `Справочник.Номенклатура` |
| Document | `Документ.<Name>` | `Документ.РеализацияТоваровУслуг` |
| Document tabular section | `Документ.<Name>.<Section>` | `Документ.РеализацияТоваровУслуг.Товары` |
| Accumulation register | `РегистрНакопления.<Name>` | `РегистрНакопления.ОстаткиТоваров` |
| Information register | `РегистрСведений.<Name>` | `РегистрСведений.КурсыВалют` |
| Accounting register | `РегистрБухгалтерии.<Name>` | `РегистрБухгалтерии.Хозрасчетный` |
| Chart of accounts | `ПланСчетов.<Name>` | `ПланСчетов.Хозрасчетный` |
| Chart of characteristic types | `ПланВидовХарактеристик.<Name>` | `ПланВидовХарактеристик.ВидыСубконто` |
| Calculation register | `РегистрРасчета.<Name>` | `РегистрРасчета.Начисления` |
| Business process | `БизнесПроцесс.<Name>` | `БизнесПроцесс.Согласование` |
| Task | `Задача.<Name>` | `Задача.ЗадачаИсполнителя` |

### Enum values and predefined items

```
// Enum value (used in conditions, not as a table):
ЗНАЧЕНИЕ(Перечисление.ТипыЦен.Оптовая)

// Predefined catalog item:
ЗНАЧЕНИЕ(Справочник.Валюты.USD)

// Empty reference:
ЗНАЧЕНИЕ(Справочник.Контрагенты.ПустаяСсылка)
```

### Virtual tables

Virtual tables are computed on-the-fly from real register data. **Always pass filter conditions as virtual table parameters, NOT in WHERE** — this is critical for performance.

**Accumulation register (Регистр накопления):**

```
// Current balances (no date = latest):
РегистрНакопления.ОстаткиТоваров.Остатки(, Номенклатура = &Ном)

// Balances at specific date:
РегистрНакопления.ОстаткиТоваров.Остатки(&Дата, Склад = &Склад)

// Turnovers for period:
РегистрНакопления.ОстаткиТоваров.Обороты(&НачДата, &КонДата, , Номенклатура = &Ном)

// Balances and turnovers:
РегистрНакопления.ОстаткиТоваров.ОстаткиИОбороты(&НачДата, &КонДата, , , Номенклатура = &Ном)
```

**Information register (Регистр сведений):**

```
// Latest values (slice of last):
РегистрСведений.КурсыВалют.СрезПоследних(&Дата, Валюта = &Валюта)

// Earliest values (slice of first):
РегистрСведений.КурсыВалют.СрезПервых(&Дата, Валюта = &Валюта)
```

Parameter syntax inside virtual tables: `Dimension = Value` joined by commas. Keep it simple — no subqueries or joins inside parameters.

**Accumulation register resource suffixes:**

When using virtual tables, resource fields get automatic suffixes:

| Virtual table | Suffix format | Example |
|---------------|---------------|---------|
| `.Остатки()` | `<Resource>Остаток` | `КоличествоОстаток` |
| `.Обороты()` | `<Resource>Оборот` | `КоличествоОборот` |
| `.ОстаткиИОбороты()` | `<Resource>НачальныйОстаток` | `КоличествоНачальныйОстаток` |
| `.ОстаткиИОбороты()` | `<Resource>Приход` | `КоличествоПриход` |
| `.ОстаткиИОбороты()` | `<Resource>Расход` | `КоличествоРасход` |
| `.ОстаткиИОбороты()` | `<Resource>Оборот` | `КоличествоОборот` |
| `.ОстаткиИОбороты()` | `<Resource>КонечныйОстаток` | `КоличествоКонечныйОстаток` |

```
// Example — turnovers with all suffixes:
ВЫБРАТЬ
    Об.Номенклатура,
    Об.КоличествоПриход,
    Об.КоличествоРасход,
    Об.КоличествоОборот,
    Об.КоличествоНачальныйОстаток,
    Об.КоличествоКонечныйОстаток
ИЗ
    РегистрНакопления.ТоварыНаСкладах.ОстаткиИОбороты(&Нач, &Кон, , , ) КАК Об
```

### Table aliases

```
Справочник.Номенклатура КАК Товары
```

Always assign short aliases — improves readability and is required when self-joining or using the same table twice.

### Hierarchical catalogs

Catalogs with hierarchy support nested groups:

```
// Direct children (one level) — preferred:
ВЫБРАТЬ Наименование
ИЗ Справочник.Номенклатура
ГДЕ Родитель = &Родитель
// params: {"Родитель": {"_objectRef": true, "УникальныйИдентификатор": "...",
//                        "ТипОбъекта": "СправочникСсылка.Номенклатура", "Представление": "..."}}

// Or by name:
ГДЕ Родитель.Наименование = "Бытовая техника"

// All items in group and subgroups — preferred:
ВЫБРАТЬ Наименование
ИЗ Справочник.Номенклатура
ГДЕ Ссылка В ИЕРАРХИИ (&Группа)
// params: {"Группа": {"_objectRef": true, "УникальныйИдентификатор": "...",
//                      "ТипОбъекта": "СправочникСсылка.Номенклатура", "Представление": "..."}}

// Or by name:
ГДЕ Ссылка В ИЕРАРХИИ (
    ВЫБРАТЬ Ссылка ИЗ Справочник.Номенклатура
    ГДЕ Наименование = "Бытовая техника"
)

// Only leaf items (not groups):
ГДЕ ЭтоГруппа = ЛОЖЬ

// Only groups:
ГДЕ ЭтоГруппа = ИСТИНА
```

**Rules:**
- `Родитель = &Родитель` — direct children only (one level)
- `Ссылка В ИЕРАРХИИ (&Группа)` — all nested levels
- `ЭтоГруппа` — `ИСТИНА` = groups only, `ЛОЖЬ` = leaf items only
- **AVOID:** `В ИЕРАРХИИ (ПустаяСсылка)` — very slow

## Field Selection

- `*` selects all real fields (virtual fields are NOT included)
- Aliases: `<expression> КАК <Alias>` — aliases must be unique within the query
- Access nested fields with dot notation: `Товары.Номенклатура.Наименование`

### CRITICAL: Compound-type field dereferencing

When a field can reference multiple table types (compound/composite type), accessing it through dots creates implicit JOINs to **ALL** possible target tables. This severely degrades performance.

**Always narrow the type with ВЫРАЗИТЬ:**

```
// BAD — joins to every possible Регистратор table:
ВЫБРАТЬ Регистратор.Номер ИЗ РегистрНакопления.ОстаткиТоваров

// GOOD — single join:
ВЫБРАТЬ ВЫРАЗИТЬ(Регистратор КАК Документ.РеализацияТоваровУслуг).Номер
ИЗ РегистрНакопления.ОстаткиТоваров
ГДЕ Регистратор ССЫЛКА Документ.РеализацияТоваровУслуг
```

### NULL handling

- Use `ЕСТЬNULL(field, default)` for fields that may be NULL (e.g., from LEFT JOIN)
- Check NULL with `ЕСТЬ NULL` / `НЕ ЕСТЬ NULL` (not `= NULL`)

### Empty table for UNION

When unioning queries with different nested table structures:
```
ПУСТАЯТАБЛИЦА.(Номенклатура, Количество, Цена)
```

## WHERE and HAVING

### Operators

| Operator | 1C syntax | Notes |
|----------|-----------|-------|
| Equals | `=` | |
| Not equals | `<>` | |
| Comparison | `<`, `>`, `<=`, `>=` | |
| AND | `И` | |
| OR | `ИЛИ` | Avoid in WHERE — see pitfalls |
| NOT | `НЕ` | |
| Range | `МЕЖДУ X И Y` | Inclusive both ends |
| In list | `В (val1, val2, ...)` | |
| In subquery | `В (ВЫБРАТЬ ...)` | |
| NULL check | `ЕСТЬ NULL` / `НЕ ЕСТЬ NULL` | Never use `= NULL` |
| Type check | `ССЫЛКА Документ.Реализация...` | Checks reference type |
| Pattern | `ПОДОБНО "шаблон"` | `%` any string, `_` one char |
| CASE | `ВЫБОР КОГДА ... ТОГДА ... ИНАЧЕ ... КОНЕЦ` | Only in non-indexed conditions |

**Operator precedence:** `НЕ` > `И` > `ИЛИ`. Use parentheses `()` for explicit priority.

### Key rules

- **ИЛИ (OR) degrades index usage.** Replace with `В (...)` where possible, or split into `ОБЪЕДИНИТЬ ВСЕ`
- **ВЫБОР (CASE) in WHERE:** Only in additional (non-indexed) conditions
- **String comparison:** Case-insensitive, ignores trailing spaces
- **Arithmetic in WHERE:** Only in additional conditions (prevents index use)
- **Pattern ПОДОБНО:** Must not start with `%` or `_` if in indexed condition

For index-aware optimization strategy, see [optimization-and-pitfalls.md](references/optimization-and-pitfalls.md).

## Joins

```
ИЗ Документ.РеализацияТоваровУслуг КАК Док
    ЛЕВОЕ СОЕДИНЕНИЕ Справочник.Контрагенты КАК Конт
    ПО Док.Контрагент = Конт.Ссылка
```

| Type | 1C syntax |
|------|-----------|
| Inner | `ВНУТРЕННЕЕ СОЕДИНЕНИЕ ... ПО` |
| Left outer | `ЛЕВОЕ [ВНЕШНЕЕ] СОЕДИНЕНИЕ ... ПО` |
| Right outer | `ПРАВОЕ [ВНЕШНЕЕ] СОЕДИНЕНИЕ ... ПО` |
| Full outer | `ПОЛНОЕ [ВНЕШНЕЕ] СОЕДИНЕНИЕ ... ПО` |

**Rules:**
- **NEVER use nested joins** (join of a subquery that itself contains joins). Replace with temporary tables or sequential joins
- For LEFT JOIN: index the right table's join fields
- For INNER JOIN: index the larger table's join fields

## Grouping and Totals

### GROUP BY

```
ВЫБРАТЬ
    Контрагент,
    СУММА(СуммаДокумента) КАК Итого,
    КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Ссылка) КАК КолДок
ИЗ Документ.РеализацияТоваровУслуг
СГРУППИРОВАТЬ ПО Контрагент
ИМЕЮЩИЕ СУММА(СуммаДокумента) > 1000
```

**Aggregate functions:** `СУММА` (SUM), `СРЕДНЕЕ` (AVG), `МИНИМУМ` (MIN), `МАКСИМУМ` (MAX), `КОЛИЧЕСТВО` (COUNT), `КОЛИЧЕСТВО(РАЗЛИЧНЫЕ ...)` (COUNT DISTINCT).

Every non-aggregated field in SELECT must appear in GROUP BY.

### TOTALS (brief)

```
ИТОГИ СУММА(СуммаДокумента) ПО ОБЩИЕ, Контрагент
```

Computes subtotal rows interleaved with detail rows. For full syntax (ПЕРИОДАМИ, hierarchical control points, grouping sets), see [query-syntax-reference.md](references/query-syntax-reference.md).

## Temporary Tables

Use temporary tables to break complex queries into steps, avoid nested subqueries, and improve performance.

```
// Step 1: collect items into temp table
ВЫБРАТЬ Ссылка КАК Номенклатура
ПОМЕСТИТЬ ВТ_Товары
ИЗ Справочник.Номенклатура
ГДЕ Наименование ПОДОБНО &Маска
ИНДЕКСИРОВАТЬ ПО Номенклатура
;
// Step 2: get balances for those items
ВЫБРАТЬ
    Ост.Номенклатура КАК Номенклатура,
    Ост.КоличествоОстаток КАК Остаток
ИЗ РегистрНакопления.ОстаткиТоваров.Остатки(
        ,
        Номенклатура В (ВЫБРАТЬ Номенклатура ИЗ ВТ_Товары)
    ) КАК Ост
```

**Rules:**
- Separate queries in a batch with `;`
- `ПОМЕСТИТЬ <Name>` creates the temp table (place right after SELECT fields, before ИЗ)
- `ИНДЕКСИРОВАТЬ ПО <field>` — add after the query for temp tables with >1000 rows used in joins or `В` subqueries
- Minimize data volume and field count in temp tables
- **NEVER** create/drop temp tables in a loop
- Prefer temp tables over nested subqueries in JOIN conditions

## Parameters

Parameters are external values passed into the query. Syntax: `&ParameterName`.

```
ВЫБРАТЬ * ИЗ Справочник.Контрагенты ГДЕ Наименование ПОДОБНО &Маска
```

Passed via `execute_query` params: `{"Маска": "%Рога%"}`.

**Rules:**
- Always use parameters for external values — never concatenate strings into query text
- Enum values: use `ЗНАЧЕНИЕ()` in query text, not as parameter: `ГДЕ Тип = ЗНАЧЕНИЕ(Перечисление.ТипыЦен.Оптовая)`
- Date format in params: ISO 8601 (`"2024-01-15"` or `"2024-01-15T10:30:00"`)
- Boolean: `ИСТИНА` / `ЛОЖЬ` in query text, or pass `true`/`false` as parameter

## Comparing Reference Fields

When comparing reference fields (link-type attributes), **pass the reference as a parameter**:

```
// GOOD — reference passed as parameter from previous query result:
ВЫБРАТЬ * ИЗ Документ.ПродажаТоваров
ГДЕ Контрагент = &Контрагент
// params: {"Контрагент": {"_objectRef": true, "УникальныйИдентификатор": "...",
//                           "ТипОбъекта": "СправочникСсылка.Контрагенты", "Представление": "..."}}

// ALSO GOOD — compare via primitive attribute:
ГДЕ Контрагент.Наименование = "ООО Ромашка"
ГДЕ Контрагент.Код = "000001"

// AVOID — direct comparison with another table's reference:
ГДЕ Документ.Контрагент = Справочник.Контрагенты.Ссылка
```

**Rules:**
- **First choice:** pass reference object from previous query result as `&Parameter`
- **Fallback:** compare via primitive fields (Наименование, Код, etc.)
- Never compare reference fields directly from different sources

## Common Patterns

**1. Find by name (fuzzy):**
```
ВЫБРАТЬ Ссылка, Наименование ИЗ Справочник.Контрагенты ГДЕ Наименование ПОДОБНО &Маска
// params: {"Маска": "%рога%"}
```

**2. Latest N documents:**
```
ВЫБРАТЬ ПЕРВЫЕ 10 Ссылка, Дата, Номер, СуммаДокумента
ИЗ Документ.РеализацияТоваровУслуг
УПОРЯДОЧИТЬ ПО Дата УБЫВ
```

**3. Current balances (virtual table, no date = now):**
```
ВЫБРАТЬ Номенклатура, КоличествоОстаток
ИЗ РегистрНакопления.ОстаткиТоваров.Остатки(, Номенклатура = &Ном) КАК Ост
```

**4. Latest register slice:**
```
ВЫБРАТЬ Валюта, Курс ИЗ РегистрСведений.КурсыВалют.СрезПоследних(&Дата,) КАК Курсы
```

**5. Count by group:**
```
ВЫБРАТЬ Контрагент, КОЛИЧЕСТВО(*) КАК Кол
ИЗ Документ.РеализацияТоваровУслуг
СГРУППИРОВАТЬ ПО Контрагент
УПОРЯДОЧИТЬ ПО Кол УБЫВ
```

**6. Check existence:**
```
ВЫБРАТЬ ПЕРВЫЕ 1 Ссылка ИЗ Справочник.Контрагенты ГДЕ ИНН = &ИНН
```

**7. Exclude items marked for deletion:**
```
ГДЕ НЕ ПометкаУдаления
```

**8. Explore table structure (zero-row query with schema):**
```
ВЫБРАТЬ ПЕРВЫЕ 0 * ИЗ Справочник.Контрагенты
// use with include_schema: true in execute_query
```

## References

- [Query syntax reference](references/query-syntax-reference.md) — ВЫБОР (CASE), ОБЪЕДИНИТЬ, УПОРЯДОЧИТЬ ПО, ИТОГИ, ПОДОБНО patterns, ССЫЛКА, subqueries
- [Optimization and pitfalls](references/optimization-and-pitfalls.md) — index strategy, ИЛИ alternatives, compound types, virtual table rules, RLS impact
- [Functions and expressions](references/functions-and-expressions.md) — aggregate, date, string, type, math functions and type casting with ВЫРАЗИТЬ
