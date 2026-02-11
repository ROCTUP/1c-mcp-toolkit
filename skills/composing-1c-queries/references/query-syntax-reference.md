# Query Syntax Reference

Detailed syntax for clauses covered briefly in the main SKILL.md.

## Contents

- [ВЫБОР (CASE)](#выбор-case)
- [ОБЪЕДИНИТЬ (UNION)](#объединить-union)
- [УПОРЯДОЧИТЬ ПО (ORDER BY)](#упорядочить-по-order-by)
- [АВТОУПОРЯДОЧИВАНИЕ](#автоупорядочивание)
- [ИТОГИ (TOTALS)](#итоги-totals)
- [Grouping sets](#grouping-sets)
- [ПОДОБНО (LIKE)](#подобно-like)
- [ССЫЛКА (type check)](#ссылка-type-check)
- [Subqueries in В (IN)](#subqueries-in-в-in)
- [ПУСТАЯТАБЛИЦА (empty table)](#пустаятаблица-empty-table)

---

## ВЫБОР (CASE)

Conditional expression — equivalent to SQL CASE.

```
ВЫБОР
    КОГДА <condition1> ТОГДА <result1>
    КОГДА <condition2> ТОГДА <result2>
    ИНАЧЕ <default>
КОНЕЦ
```

Can be used in field list, HAVING, TOTALS. **Cannot** be used as part of indexed (main) WHERE condition.

Example:
```
ВЫБРАТЬ
    Наименование,
    ВЫБОР
        КОГДА Сумма > 100000 ТОГДА "Крупный"
        КОГДА Сумма > 10000 ТОГДА "Средний"
        ИНАЧЕ "Мелкий"
    КОНЕЦ КАК Категория
ИЗ Документ.РеализацияТоваровУслуг
```

---

## ОБЪЕДИНИТЬ (UNION)

Merges results of two or more SELECT queries into one result set.

```
ВЫБРАТЬ Поле1, Поле2 ИЗ Таблица1
ОБЪЕДИНИТЬ [ВСЕ]
ВЫБРАТЬ Поле1, Поле2 ИЗ Таблица2
```

**Rules:**
- `ОБЪЕДИНИТЬ` — removes duplicates (like UNION)
- `ОБЪЕДИНИТЬ ВСЕ` — keeps duplicates (like UNION ALL, faster)
- Column count must match across all queries
- Column names and types are taken from the **first** query
- If nested table structures differ, use `ПУСТАЯТАБЛИЦА` in the query that lacks the nested table

---

## УПОРЯДОЧИТЬ ПО (ORDER BY)

```
УПОРЯДОЧИТЬ ПО <field_or_alias> [ВОЗР | УБЫВ] [, ...]
```

- `ВОЗР` — ascending (default)
- `УБЫВ` — descending
- Can reference field aliases from SELECT
- Can use aggregate function results when grouping
- **Do NOT use aggregate functions directly** for comparison in ORDER BY

Example:
```
ВЫБРАТЬ Контрагент, СУММА(Сумма) КАК Итого
ИЗ Документ.РеализацияТоваровУслуг
СГРУППИРОВАТЬ ПО Контрагент
УПОРЯДОЧИТЬ ПО Итого УБЫВ
```

---

## АВТОУПОРЯДОЧИВАНИЕ

Adds automatic ordering by "presentation" (human-readable name) of reference fields. Placed after ORDER BY or standalone.

```
ВЫБРАТЬ Контрагент, Сумма
ИЗ Документ.РеализацияТоваровУслуг
АВТОУПОРЯДОЧИВАНИЕ
```

Rarely needed — explicit `УПОРЯДОЧИТЬ ПО` is preferred for predictable results.

---

## ИТОГИ (TOTALS)

Computes aggregate subtotal rows and interleaves them with detail rows.

```
ИТОГИ
    [<aggregate_expression> [КАК <Alias>] [, ...]]
ПО
    [ОБЩИЕ]
    [, <control_point> [, ...]]
```

**Components:**
- `ОБЩИЕ` — grand total across entire result
- Control points — fields to group subtotals by. Can be hierarchical
- `ПЕРИОДАМИ(<Period>, <StartDate>, <EndDate>)` — for date-type fields; `<Period>` is one of: `СЕКУНДА`, `МИНУТА`, `ЧАС`, `ДЕНЬ`, `НЕДЕЛЯ`, `МЕСЯЦ`, `КВАРТАЛ`, `ГОД`, `ДЕКАДА`, `ПОЛУГОДИЕ`

Example:
```
ВЫБРАТЬ
    Контрагент, Номенклатура, Количество, Сумма
ИЗ Документ.РеализацияТоваровУслуг.Товары
ИТОГИ СУММА(Количество), СУММА(Сумма)
ПО ОБЩИЕ, Контрагент
```

**Periodic register totals:** Only enable (`РАЗРЕШИТЬ ИТОГИ`) when large data volume, no date in slice queries, and simple RLS.

---

## Grouping Sets

Multiple groupings in a single pass for performance.

```
СГРУППИРОВАТЬ ПО ГРУППИРУЮЩИМ НАБОРАМ
    (
        (НаборПолей1),
        (НаборПолей2)
    )
```

Field order within a set and set order both matter. Use `СГРУППИРОВАНОПО(<field>)` function to determine which grouping set produced a given row.

Example:
```
ВЫБРАТЬ Контрагент, Номенклатура, СУММА(Сумма) КАК Итого
ИЗ Документ.РеализацияТоваровУслуг.Товары
СГРУППИРОВАТЬ ПО ГРУППИРУЮЩИМ НАБОРАМ
    (
        (Контрагент),
        (Номенклатура),
        (Контрагент, Номенклатура)
    )
```

---

## ПОДОБНО (LIKE)

String pattern matching operator.

```
ГДЕ Наименование ПОДОБНО "Шаблон"
```

**Wildcards:**

| Character | Meaning | Example |
|-----------|---------|---------|
| `%` | Any string (0+ chars) | `"%молоко%"` — contains "молоко" |
| `_` | Exactly one character | `"__-__"` — matches "12-34" |
| `[abc]` | One char from set | `"[АБВ]%"` — starts with А, Б, or В |
| `[^abc]` | One char NOT in set | `"[^0-9]%"` — starts with non-digit |
| `[a-z]` | One char in range | `"[А-Я]%"` — starts with capital Cyrillic |

**Escape character:** Use `СПЕЦСИМВОЛ` to escape wildcards:
```
ГДЕ Код ПОДОБНО "%#_%" СПЕЦСИМВОЛ "#"
// matches literal underscore in Код
```

**Performance:** Pattern must NOT start with `%` or `_` if used in indexed (main) condition — such patterns prevent index usage.

---

## ССЫЛКА (Type Check)

Checks if a reference-type value points to a specific table. Used with compound-type fields.

```
ГДЕ Регистратор ССЫЛКА Документ.РеализацияТоваровУслуг
```

Commonly paired with `ВЫРАЗИТЬ`:
```
ВЫБРАТЬ
    ВЫБОР
        КОГДА Регистратор ССЫЛКА Документ.Реализация
            ТОГДА ВЫРАЗИТЬ(Регистратор КАК Документ.Реализация).Контрагент
        КОГДА Регистратор ССЫЛКА Документ.Поступление
            ТОГДА ВЫРАЗИТЬ(Регистратор КАК Документ.Поступление).Контрагент
    КОНЕЦ КАК Контрагент
ИЗ РегистрНакопления.ОстаткиТоваров
```

---

## Subqueries in В (IN)

Check membership against a subquery result:

```
ГДЕ Контрагент В (
    ВЫБРАТЬ Ссылка ИЗ Справочник.Контрагенты ГДЕ Город = &Город
)
```

**Rules:**
- Subquery must return exactly one column
- Avoid complex subqueries (joins, compound type access, nested subqueries). Use temporary tables instead
- For large filter sets, a temp table with `В (ВЫБРАТЬ ... ИЗ ВТ)` is more efficient than a literal list

---

## ПУСТАЯТАБЛИЦА (Empty Table)

Used in UNION when one query has a nested table (tabular section) that the other doesn't.

```
// Query 1 has Товары nested table
ВЫБРАТЬ Ссылка, Товары.(Номенклатура, Количество)
ИЗ Документ.РеализацияТоваровУслуг

ОБЪЕДИНИТЬ ВСЕ

// Query 2 lacks it — provide matching empty table
ВЫБРАТЬ Ссылка, ПУСТАЯТАБЛИЦА.(Номенклатура, Количество)
ИЗ Документ.ВозвратТоваров
```

Alias names inside `ПУСТАЯТАБЛИЦА.(...)` must match the aliases from the corresponding real nested table in the other query.
