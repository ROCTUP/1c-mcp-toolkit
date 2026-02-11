# Functions and Expressions

Complete reference of functions and expression syntax available in 1C query language.

## Contents

- [Type casting (ВЫРАЗИТЬ)](#type-casting-выразить)
- [Aggregate functions](#aggregate-functions)
- [Date and time functions](#date-and-time-functions)
- [String functions](#string-functions)
- [Type functions](#type-functions)
- [Logical and NULL functions](#logical-and-null-functions)
- [Math functions](#math-functions)
- [Trigonometric functions](#trigonometric-functions)
- [Special functions](#special-functions)
- [Arithmetic operators](#arithmetic-operators)
- [Constants and literals](#constants-and-literals)

---

## Type Casting (ВЫРАЗИТЬ)

Explicitly converts a value to a specific type. **Essential for compound-type fields.**

```
ВЫРАЗИТЬ(<expression> КАК <type>)
```

Examples:
```
// Narrow compound reference to specific type:
ВЫРАЗИТЬ(Регистратор КАК Документ.РеализацияТоваровУслуг)

// Cast number to specific precision:
ВЫРАЗИТЬ(Сумма КАК ЧИСЛО(15, 2))

// Cast to string:
ВЫРАЗИТЬ(Код КАК СТРОКА(10))

// Cast to boolean:
ВЫРАЗИТЬ(Значение КАК БУЛЕВО)

// Cast to date:
ВЫРАЗИТЬ(Значение КАК ДАТА)
```

Primitive types for casting: `ЧИСЛО(<length>, <precision>)`, `СТРОКА(<length>)`, `БУЛЕВО`, `ДАТА`.

---

## Aggregate Functions

Used with `СГРУППИРОВАТЬ ПО`, `ИМЕЮЩИЕ`, `ИТОГИ`, and `УПОРЯДОЧИТЬ ПО`.

| Function | Description | Example |
|----------|-------------|---------|
| `СУММА(<field>)` | Sum | `СУММА(Количество)` |
| `СРЕДНЕЕ(<field>)` | Average | `СРЕДНЕЕ(Цена)` |
| `МИНИМУМ(<field>)` | Minimum value | `МИНИМУМ(Дата)` |
| `МАКСИМУМ(<field>)` | Maximum value | `МАКСИМУМ(Дата)` |
| `КОЛИЧЕСТВО(*)` | Count all rows | `КОЛИЧЕСТВО(*)` |
| `КОЛИЧЕСТВО(<field>)` | Count non-NULL values | `КОЛИЧЕСТВО(Контрагент)` |
| `КОЛИЧЕСТВО(РАЗЛИЧНЫЕ <field>)` | Count distinct values | `КОЛИЧЕСТВО(РАЗЛИЧНЫЕ Контрагент)` |

**Rule:** Do not use grouping fields inside aggregate functions (except nested table fields).

---

## Date and Time Functions

### Component extraction

| Function | Returns | Example |
|----------|---------|---------|
| `ГОД(<date>)` | Year | `ГОД(Дата)` → 2024 |
| `КВАРТАЛ(<date>)` | Quarter (1-4) | `КВАРТАЛ(Дата)` → 1 |
| `МЕСЯЦ(<date>)` | Month (1-12) | `МЕСЯЦ(Дата)` → 3 |
| `ДЕНЬГОДА(<date>)` | Day of year (1-366) | `ДЕНЬГОДА(Дата)` |
| `ДЕНЬ(<date>)` | Day of month (1-31) | `ДЕНЬ(Дата)` → 15 |
| `ДЕНЬНЕДЕЛИ(<date>)` | Day of week (1=Mon..7=Sun) | `ДЕНЬНЕДЕЛИ(Дата)` |
| `ЧАС(<date>)` | Hour (0-23) | `ЧАС(Дата)` |
| `МИНУТА(<date>)` | Minute (0-59) | `МИНУТА(Дата)` |
| `СЕКУНДА(<date>)` | Second (0-59) | `СЕКУНДА(Дата)` |

### Period boundaries

| Function | Description |
|----------|-------------|
| `НАЧАЛОПЕРИОДА(<date>, <period>)` | Start of period. `<period>`: МИНУТА, ЧАС, ДЕНЬ, НЕДЕЛЯ, МЕСЯЦ, КВАРТАЛ, ГОД, ДЕКАДА, ПОЛУГОДИЕ |
| `КОНЕЦПЕРИОДА(<date>, <period>)` | End of period (same period options) |

```
НАЧАЛОПЕРИОДА(Дата, МЕСЯЦ)    // first day of month
КОНЕЦПЕРИОДА(Дата, КВАРТАЛ)    // last moment of quarter
```

### Date arithmetic

| Function | Description |
|----------|-------------|
| `ДОБАВИТЬКДАТЕ(<date>, <period>, <count>)` | Add N periods to date |
| `РАЗНОСТЬДАТ(<date1>, <date2>, <period>)` | Difference between dates in periods |

```
ДОБАВИТЬКДАТЕ(Дата, МЕСЯЦ, 3)         // add 3 months
ДОБАВИТЬКДАТЕ(Дата, ДЕНЬ, -7)          // subtract 7 days
РАЗНОСТЬДАТ(ДатаНач, ДатаКон, ДЕНЬ)   // days between dates
```

Period options: `СЕКУНДА`, `МИНУТА`, `ЧАС`, `ДЕНЬ`, `МЕСЯЦ`, `КВАРТАЛ`, `ГОД`.

---

## String Functions

| Function | Description | Example |
|----------|-------------|---------|
| `ПОДСТРОКА(<str>, <start>, <length>)` | Substring (1-based) | `ПОДСТРОКА(Код, 1, 3)` |
| `ДЛИНАСТРОКИ(<str>)` | String length | `ДЛИНАСТРОКИ(Наименование)` |
| `ЛЕВ(<str>, <n>)` | Left N characters | `ЛЕВ(Код, 5)` |
| `ПРАВ(<str>, <n>)` | Right N characters | `ПРАВ(Код, 3)` |
| `СТРЗАМЕНИТЬ(<str>, <find>, <replace>)` | Replace substring | `СТРЗАМЕНИТЬ(Имя, " ", "_")` |
| `СОКРЛ(<str>)` | Trim leading spaces | `СОКРЛ(Код)` |
| `СОКРП(<str>)` | Trim trailing spaces | `СОКРП(Код)` |
| `СОКРЛП(<str>)` | Trim both sides | `СОКРЛП(Код)` |
| `ВРЕГ(<str>)` | Uppercase | `ВРЕГ(Наименование)` |
| `НРЕГ(<str>)` | Lowercase | `НРЕГ(Наименование)` |

**Note:** `ПОДСТРОКА` in WHERE main condition prevents index usage. Use only in additional conditions.

---

## Type Functions

| Function | Description | Example |
|----------|-------------|---------|
| `ТИПЗНАЧЕНИЯ(<expr>)` | Returns the type of a value | `ТИПЗНАЧЕНИЯ(Ссылка)` |
| `ТИП(<type_name>)` | Returns a type object for comparison | `ТИП("Справочник.Контрагенты")` |
| `ССЫЛКА` | Type check operator for references | `Рег ССЫЛКА Документ.Реализация` |
| `ПРЕДСТАВЛЕНИЕ(<expr>)` | Human-readable string of a reference | `ПРЕДСТАВЛЕНИЕ(Контрагент)` |
| `УНИКАЛЬНЫЙИДЕНТИФИКАТОР(<ref>)` | UUID of a reference as string | `УНИКАЛЬНЫЙИДЕНТИФИКАТОР(Ссылка)` |

```
// Filter by value type:
ГДЕ ТИПЗНАЧЕНИЯ(Субконто1) = ТИП("Справочник.Контрагенты")
```

---

## Logical and NULL Functions

| Function | Description |
|----------|-------------|
| `ЕСТЬNULL(<expr>, <default>)` | Returns `<default>` if `<expr>` is NULL (like SQL COALESCE for two args) |

```
ЕСТЬNULL(Ост.КоличествоОстаток, 0)
```

**ВЫБОР (CASE):** See [query-syntax-reference.md](query-syntax-reference.md#выбор-case).

---

## Math Functions

| Function | Description |
|----------|-------------|
| `ОКР(<number>, <precision>)` | Round to N decimal places |
| `ЦЕЛ(<number>)` | Truncate to integer (floor toward zero) |

```
ОКР(Сумма / Количество, 2)   // round to 2 decimals
ЦЕЛ(Сумма / 100)             // integer part
```

**Arithmetic precision:** At least 8 decimal places. Results may vary slightly across DBMS.

---

## Trigonometric Functions

Standard math functions — same semantics as in most languages.

| Function | Description |
|----------|-------------|
| `SIN(<x>)` | Sine (radians) |
| `COS(<x>)` | Cosine |
| `TAN(<x>)` | Tangent |
| `ASIN(<x>)` | Arcsine |
| `ACOS(<x>)` | Arccosine |
| `ATAN(<x>)` | Arctangent |

---

## Special Functions

| Function | Description |
|----------|-------------|
| `EXP(<x>)` | e^x |
| `LOG(<x>)` | Natural logarithm |
| `LOG10(<x>)` | Base-10 logarithm |
| `POW(<base>, <exp>)` | Power |
| `SQRT(<x>)` | Square root |
| `АВТОНОМЕРЗАПИСИ()` | Auto row number in result |
| `РАЗМЕРХРАНИМЫХДАННЫХ(<field>)` | Storage size of a field in bytes |
| `СГРУППИРОВАНОПО(<field>)` | TRUE if current row was grouped by this field (for grouping sets) |

---

## Arithmetic Operators

`+`, `-`, `*`, `/` — standard arithmetic. Precision: at least 8 decimal places.

**In WHERE:** Only use in additional conditions (prevents index use in main condition).

---

## Constants and Literals

| Type | Syntax | Example |
|------|--------|---------|
| Boolean | `ИСТИНА` / `ЛОЖЬ` | `ГДЕ Проведен = ИСТИНА` |
| Number | Decimal literal | `1000`, `3.14` |
| String | Double quotes | `"Текст"` |
| String with quotes | Escape `"` by doubling | `"Компания ""Рога и Копыта"""` |
| Date | `ДАТАВРЕМЯ(<Y>, <M>, <D> [, <H>, <Min>, <S>])` | `ДАТАВРЕМЯ(2024, 1, 15)` |
| NULL | `NULL` | `ЕСТЬNULL(X, 0)` |
| Undefined | `НЕОПРЕДЕЛЕНО` | |
| Enum/predefined | `ЗНАЧЕНИЕ(...)` | `ЗНАЧЕНИЕ(Перечисление.Типы.Оптовая)` |
| Empty reference | `ЗНАЧЕНИЕ(Справочник.X.ПустаяСсылка)` | |

**Parameters:** `&Name` — external values passed to query. Always prefer parameters over string concatenation.
