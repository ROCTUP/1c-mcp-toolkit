# Workflow Examples

Detailed multi-step workflows with full curl commands and expected responses at each step.

> **Convention**: all examples use the variables from Quick Start:
> ```sh
> BASE_HOST=localhost
> BASE_URL="http://$BASE_HOST:6003"
> CHANNEL="default"
> J='-H Content-Type:application/json'
> ```

---

## Workflow 1: Explore an Unfamiliar Database

**Trigger**: user asks "what's in this 1C database", "show me the structure", "what tables are available"

### Step 1: Health check

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/health"
```

Response:
```json
{"status": "ok", "channels_count": 1}
```

### Step 2: Get root summary

```sh
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL"
```

Response:
```json
{
  "success": true,
  "configuration": {
    "platform_version": "8.3.25.1000",
    "infobase_name": "TradeDB",
    "metadata": {"Имя": "УправлениеТорговлей", "Синоним": "Управление торговлей"}
  },
  "data": [
    {"Тип": "Справочник", "Количество": 265},
    {"Тип": "Документ", "Количество": 27},
    {"Тип": "РегистрСведений", "Количество": 150},
    {"Тип": "РегистрНакопления", "Количество": 42}
  ]
}
```

**Decision**: The database is "Управление торговлей" with 27 documents and 265 catalogs. Drill into documents.

### Step 3: List documents

```sh
curl -sS -G --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" \
  --data-urlencode "meta_type=Документ" \
  --data-urlencode "limit=50"
```

Response:
```json
{
  "success": true,
  "returned": 27,
  "has_more": false,
  "data": [
    {"ПолноеИмя": "Документ.ЗаказПокупателя", "Синоним": "Заказ покупателя"},
    {"ПолноеИмя": "Документ.РеализацияТоваровУслуг", "Синоним": "Реализация товаров и услуг"},
    {"ПолноеИмя": "Документ.ПоступлениеТоваровУслуг", "Синоним": "Поступление товаров и услуг"}
  ]
}
```

### Step 4: Get detailed structure of an interesting object

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_metadata?channel=$CHANNEL" $J \
  -d '{"filter":"Документ.РеализацияТоваровУслуг","sections":["properties"]}'
```

Response:
```json
{
  "success": true,
  "data": {
    "Тип": "Документ",
    "Имя": "РеализацияТоваровУслуг",
    "ПолноеИмя": "Документ.РеализацияТоваровУслуг",
    "Реквизиты": [
      {"Имя": "Контрагент", "Тип": "СправочникСсылка.Контрагенты"},
      {"Имя": "Склад", "Тип": "СправочникСсылка.Склады"},
      {"Имя": "Валюта", "Тип": "СправочникСсылка.Валюты"}
    ],
    "ТабличныеЧасти": [
      {
        "Имя": "Товары",
        "Реквизиты": [
          {"Имя": "Номенклатура", "Тип": "СправочникСсылка.Номенклатура"},
          {"Имя": "Количество", "Тип": "Число(15,3)"},
          {"Имя": "Цена", "Тип": "Число(15,2)"},
          {"Имя": "Сумма", "Тип": "Число(15,2)"}
        ]
      }
    ]
  }
}
```

### Step 5: Sample real data

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка, Дата, Номер, Контрагент ИЗ Документ.РеализацияТоваровУслуг УПОРЯДОЧИТЬ ПО Дата УБЫВ","limit":5}'
```

Response:
```json
{
  "success": true,
  "data": [
    {
      "Ссылка": {"_objectRef": true, "ТипОбъекта": "ДокументСсылка.РеализацияТоваровУслуг", "Представление": "Реализация №015 от 15.01.2024", "УникальныйИдентификатор": "..."},
      "Дата": "2024-01-15T10:30:00",
      "Номер": "015",
      "Контрагент": {"_objectRef": true, "ТипОбъекта": "СправочникСсылка.Контрагенты", "Представление": "ООО Рога и Копыта", "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678"}
    }
  ],
  "count": 1
}
```

**Result**: Agent now understands the database structure and can answer questions about available data.

---

## Workflow 2: Investigate Object Dependencies

**Trigger**: user asks "where is this customer used", "can I delete this item", "show all references to this object"

### Step 1: Find the object

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка ИЗ Справочник.Контрагенты ГДЕ Наименование ПОДОБНО &Маска","params":{"Маска":"%Рога%"},"limit":1}'
```

Response — extract `object_description` from the `Ссылка` field:
```json
{
  "success": true,
  "data": [
    {
      "Ссылка": {
        "_objectRef": true,
        "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
        "ТипОбъекта": "СправочникСсылка.Контрагенты",
        "Представление": "ООО Рога и Копыта"
      }
    }
  ]
}
```

### Step 2: Find all references

Use the `object_description` from Step 1 as `target_object_description`:

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/find_references_to_object?channel=$CHANNEL" $J \
  -d '{
    "target_object_description": {
      "_objectRef": true,
      "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
      "ТипОбъекта": "СправочникСсылка.Контрагенты"
    },
    "search_scope": ["documents", "catalogs", "information_registers", "accumulation_registers"],
    "limit_hits": 100
  }'
```

Response:
```json
{
  "success": true,
  "data": {
    "hits": [
      {
        "found_in_meta": "Документ.РеализацияТоваровУслуг",
        "found_in_object": {"_objectRef": true, "ТипОбъекта": "ДокументСсылка.РеализацияТоваровУслуг", "Представление": "Реализация №015 от 15.01.2024", "УникальныйИдентификатор": "..."},
        "path": "Контрагент",
        "match_kind": "attribute",
        "note": "Реализация №015 от 15.01.2024"
      },
      {
        "found_in_meta": "Документ.ЗаказПокупателя",
        "found_in_object": {"_objectRef": true, "ТипОбъекта": "ДокументСсылка.ЗаказПокупателя", "Представление": "Заказ №003 от 10.01.2024", "УникальныйИдентификатор": "..."},
        "path": "Контрагент",
        "match_kind": "attribute",
        "note": "Заказ №003 от 10.01.2024"
      }
    ],
    "total_hits": 2,
    "candidates_checked": 15,
    "timeout_exceeded": false,
    "skipped_names": []
  }
}
```

### Step 3: Check access rights

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
    "user": {
      "name": "Иванов",
      "roles": ["Менеджер"],
      "effective_rights": {"Чтение": true, "Изменение": true, "Добавление": true, "Удаление": false}
    }
  }
}
```

### Step 4: Check recent changes in event log

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{
    "object_description": {
      "_objectRef": true,
      "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
      "ТипОбъекта": "СправочникСсылка.Контрагенты"
    },
    "limit": 20
  }'
```

**Result**: Agent can now tell the user: "This customer is referenced by 2 documents (1 sales order, 1 shipment). User Иванов can read/modify but not delete it. Last modified on 2024-01-10."

---

## Workflow 3: Diagnose Event Log Errors

**Trigger**: user asks "what errors happened", "investigate recent problems", "show error log"

### Step 1: Fetch recent errors

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-01T00:00:00","end_date":"2024-01-31T23:59:59","levels":["Error"],"limit":100}'
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
      "comment": "Ошибка при проведении документа: Недостаточно товара на складе",
      "user": "Иванов",
      "metadata": "Документ.РеализацияТоваровУслуг",
      "data_presentation": "Реализация №015 от 15.01.2024",
      "session": 12345,
      "application": "ThinClient"
    },
    {
      "date": "2024-01-14T16:20:00",
      "level": "Error",
      "event": "_$Data$_.Update",
      "comment": "Ошибка блокировки данных",
      "user": "Петров",
      "metadata": "Документ.ПоступлениеТоваровУслуг",
      "data_presentation": "Поступление №008 от 14.01.2024"
    }
  ],
  "count": 2,
  "last_date": "2024-01-15T10:30:00",
  "next_same_second_offset": 1,
  "has_more": false
}
```

### Step 2: Paginate if needed

If `has_more=true`, fetch next page using cursor:

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"start_date":"2024-01-15T10:30:00","same_second_offset":1,"end_date":"2024-01-31T23:59:59","levels":["Error"],"limit":100}'
```

### Step 3: Examine objects from errors

Use the data_presentation link to get the problematic document:

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка, Дата, Номер, Контрагент, Склад ИЗ Документ.РеализацияТоваровУслуг ГДЕ Номер = &Номер","params":{"Номер":"015"},"limit":1}'
```

### Step 4: Investigate context

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Номенклатура, Количество ИЗ Документ.РеализацияТоваровУслуг.Товары ГДЕ Ссылка.Номер = &Номер","params":{"Номер":"015"},"limit":100}'
```

**Result**: Agent identifies error patterns, affected documents, and provides diagnostic summary to the user.

---

## Workflow 4: Generate Clickable Links for Users

**Trigger**: user asks "give me links to these documents", "show results with navigation links"

### Step 1: Query documents

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка, Дата, Номер, Контрагент ИЗ Документ.РеализацияТоваровУслуг ГДЕ Дата >= &Дата","params":{"Дата":"2024-01-01T00:00:00"},"limit":10}'
```

Response (contains `object_description` in each row's `Ссылка` field).

### Step 2: Generate navigation links for each result

For each row, pass the `Ссылка` object_description to get_link_of_object:

```sh
curl -sS --noproxy $BASE_HOST "$BASE_URL/api/get_link_of_object?channel=$CHANNEL" $J \
  -d '{"object_description":{"_objectRef":true,"УникальныйИдентификатор":"a1b2c3d4-5678-9012-3456-789012345678","ТипОбъекта":"ДокументСсылка.РеализацияТоваровУслуг"}}'
```

Response:
```json
{
  "success": true,
  "data": "e1cib/data/Документ.РеализацияТоваровУслуг?ref=a1b2c3d456789012345678901234"
}
```

### Step 3: Present to user

Combine query data with navigation links to present a formatted list:

```
1. Реализация №015 от 15.01.2024 — ООО Рога и Копыта
   Link: e1cib/data/Документ.РеализацияТоваровУслуг?ref=a1b2c3d456789012345678901234

2. Реализация №016 от 16.01.2024 — ООО Звезда
   Link: e1cib/data/Документ.РеализацияТоваровУслуг?ref=b2c3d4e567890123456789012345
```

**Result**: User gets a structured list with links they can paste into 1C navigation bar to open documents directly.
