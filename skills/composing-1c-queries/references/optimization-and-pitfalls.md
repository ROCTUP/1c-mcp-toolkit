# Optimization and Pitfalls

1C-specific query optimization rules. These differ from generic SQL and are critical for production performance.

## Contents

- [Index-aware filtering (main vs additional conditions)](#index-aware-filtering)
- [ИЛИ (OR) performance trap](#или-or-performance-trap)
- [Compound-type dereferencing](#compound-type-dereferencing)
- [Virtual table parameters](#virtual-table-parameters)
- [Nested joins prohibition](#nested-joins-prohibition)
- [Temporary table best practices](#temporary-table-best-practices)
- [RLS impact](#rls-impact)
- [Subquery restrictions](#subquery-restrictions)
- [DBMS portability](#dbms-portability)

---

## Index-Aware Filtering

1C query optimizer splits WHERE conditions into two categories:

### Main condition (основное условие)
- **Used for index lookup** — determines which index range to scan
- Must be the most restrictive part of the filter
- Allowed operators: `=`, `>`, `<`, `>=`, `<=`, `ПОДОБНО`, `МЕЖДУ`, `В`
- Combined with other main conditions using `И` only
- **Forbidden in main condition:** `ИЛИ`, `ВЫБОР`, arithmetic expressions, function calls (like `ПОДСТРОКА`), `НЕ`

### Additional condition (дополнительное условие)
- Applied **after** index scan — filters rows one by one
- Can use any operators, `ИЛИ`, `ВЫБОР`, arithmetic, functions
- Combined with the main condition using `И`

### Strategy
Structure your WHERE as: `<main_condition> И <additional_condition>`

```
// Main: indexed field with =
// Additional: complex logic
ГДЕ Организация = &Орг                              // main (indexed)
    И (Сумма > 1000 ИЛИ ПометкаУдаления = ИСТИНА)   // additional
```

If a query is slow, check if conditions prevent index use. Fix by:
1. Using `ВЫРАЗИТЬ` to narrow compound types
2. Moving complex logic to additional condition
3. Rewriting with temporary tables

---

## ИЛИ (OR) Performance Trap

`ИЛИ` in the main condition **prevents index use** on all fields involved.

**Allowed only when:**
- Applied to the **last or only** field of an index
- AND can be replaced with `В (...)`:

```
// BAD — breaks index on both Контрагент and Организация:
ГДЕ Контрагент = &К1 ИЛИ Организация = &Орг

// GOOD — use В instead:
ГДЕ Контрагент В (&К1, &К2, &К3)

// GOOD — split into UNION ALL:
ВЫБРАТЬ ... ГДЕ Контрагент = &К1
ОБЪЕДИНИТЬ ВСЕ
ВЫБРАТЬ ... ГДЕ Организация = &Орг
```

When replacing `ИЛИ` with `ОБЪЕДИНИТЬ ВСЕ`, verify that the result is equivalent (no missing/duplicate rows).

---

## Compound-Type Dereferencing

When a field has a composite reference type (can point to multiple tables), accessing sub-fields through dots creates implicit JOINs to **every** possible target table.

**Impact:**
- Query complexity explodes (many extra joins)
- Performance degrades severely, especially with RLS
- Behavior varies across DBMS

**Solutions:**

1. **ВЫРАЗИТЬ (CAST):** Narrow to specific type before dereferencing:
   ```
   ВЫРАЗИТЬ(Регистратор КАК Документ.Реализация).Номер
   ```

2. **ССЫЛКА + ВЫБОР:** Handle multiple types explicitly:
   ```
   ВЫБОР
       КОГДА Рег ССЫЛКА Документ.Реализация
           ТОГДА ВЫРАЗИТЬ(Рег КАК Документ.Реализация).Контрагент
       КОГДА Рег ССЫЛКА Документ.Поступление
           ТОГДА ВЫРАЗИТЬ(Рег КАК Документ.Поступление).Контрагент
   КОНЕЦ
   ```

3. **Temporary tables:** Pre-filter by type, then join:
   ```
   ВЫБРАТЬ Регистратор КАК Док ПОМЕСТИТЬ ВТ_Реализации
   ИЗ РегистрНакопления.Товары
   ГДЕ Регистратор ССЫЛКА Документ.Реализация;

   ВЫБРАТЬ ВЫРАЗИТЬ(ВТ.Док КАК Документ.Реализация).Контрагент
   ИЗ ВТ_Реализации КАК ВТ
   ```

4. **Denormalization:** For frequently accessed related data, add a dedicated field to the source table (configuration-level change).

---

## Virtual Table Parameters

Virtual tables (Остатки, Обороты, СрезПоследних, etc.) accept parameters that act as **pre-filters** at the storage engine level.

**CRITICAL: Always pass conditions into virtual table parameters, NOT into WHERE.**

```
// BAD — full table scan, then filter:
ВЫБРАТЬ * ИЗ РегистрНакопления.Товары.Остатки() КАК Ост
ГДЕ Ост.Склад = &Склад

// GOOD — pre-filtered at storage level:
ВЫБРАТЬ * ИЗ РегистрНакопления.Товары.Остатки(, Склад = &Склад) КАК Ост
```

**Parameter rules:**
- Use simple expressions only: `Dimension = Value`
- Multiple conditions separated by commas (implicit AND)
- NO subqueries inside parameters (use temp table + `В (ВЫБРАТЬ ... ИЗ ВТ)` pattern)
- NO joins inside parameters
- For Остатки without date parameter — returns current (latest) balances

---

## Nested Joins Prohibition

**Never nest joins** — i.e., do not join to a subquery that itself contains joins.

```
// BAD — nested join:
ЛЕВОЕ СОЕДИНЕНИЕ (
    ВЫБРАТЬ ... ИЗ Т1 ВНУТРЕННЕЕ СОЕДИНЕНИЕ Т2 ПО ...
) КАК Подзапрос
ПО ...

// GOOD — use temp table:
ВЫБРАТЬ ... ПОМЕСТИТЬ ВТ ИЗ Т1 ВНУТРЕННЕЕ СОЕДИНЕНИЕ Т2 ПО ...;
... ЛЕВОЕ СОЕДИНЕНИЕ ВТ ПО ...
```

**Also avoid:**
- Joining to subqueries with `В` that contain joins
- Complex conditions in `ПО` (join ON clause) — especially subqueries

Replace all such patterns with sequential queries using temporary tables.

---

## Temporary Table Best Practices

**When to use:**
- Replace nested joins or subqueries in conditions
- Pre-filter large datasets before joining
- Break complex multi-join queries into steps
- Store intermediate results for reuse in batch queries

**Rules:**
- `ИНДЕКСИРОВАТЬ ПО <field>` — always add when table has >1000 rows and is used in JOIN or `В` subquery
- Minimize data volume: select only needed fields
- Minimize row count: apply filters early
- **Never create/drop temp tables in a loop** — pre-compute or batch-process instead
- Don't copy a temp table just to rename it — use the original
- For very large datasets, process in portions rather than loading everything into one temp table

---

## RLS Impact

Row-Level Security (RLS) adds implicit conditions to every query. When RLS rules contain subqueries or joins:

- Query plan becomes significantly more complex
- Compound-type dereferencing cost multiplies
- Joins become slower

**Mitigation:**
- Avoid roles with contradictory RLS on the same object
- Use `РАЗРЕШЕННЫЕ` in SELECT to apply RLS (without it, access errors may occur)
- If RLS causes severe slowdown: rewrite with temp tables in privileged mode (configuration-level)
- Keep RLS conditions simple (avoid subqueries and joins in RLS templates)

---

## Subquery Restrictions

When using subqueries (in `В`, `СУЩЕСТВУЕТ`, or as a source), avoid:

- Joins inside the subquery
- Compound-type field access ("any reference" patterns)
- Accessing document header fields from a tabular section subquery
- Complex RLS conditions that apply to subquery tables

All of these inflate the SQL generated by the 1C engine. Replace with temporary tables.

---

## DBMS Portability

1C runs on multiple DBMS backends (MS SQL, PostgreSQL, IBM DB2, Oracle). Some behaviors differ:

- **String comparison:** Generally case-insensitive in 1C, but edge cases may differ by DBMS
- **Date arithmetic precision:** May vary
- **Arithmetic precision:** Rounding differences possible
- **Query plan optimization:** Different DBMS may choose different execution plans for the same 1C query

**Recommendation:** Always use `ВЫРАЗИТЬ` for explicit type casting. Test queries on the target DBMS. Avoid relying on DBMS-specific behaviors.
