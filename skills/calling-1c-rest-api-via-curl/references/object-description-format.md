# Object Description Format

## Overview

The `object_description` is a JSON structure that represents a reference to a 1C object. It flows between tools as a universal object identifier — it appears in query results and serves as input for several other tools.

## Structure

```json
{
  "_objectRef": true,
  "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
  "ТипОбъекта": "СправочникСсылка.Контрагенты",
  "Представление": "ООО Рога и Копыта"
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `_objectRef` | boolean | Yes | Must be `true`. Marks this object as a reference. |
| `УникальныйИдентификатор` | string | Yes | UUID in format `8-4-4-4-12` (e.g., `ba7e5a3d-1234-5678-9abc-def012345678`). |
| `ТипОбъекта` | string | Yes | Full type name (e.g., `СправочникСсылка.Контрагенты`, `ДокументСсылка.РеализацияТоваровУслуг`). |
| `Представление` | string | No | Human-readable string representation (e.g., object name or document number). |

### UUID validation

The `УникальныйИдентификатор` must match the regex:
```
^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$
```

## Where it appears as output

### 1. execute_query results

When a query selects a reference column, each row contains an `object_description` for that column:

```sh
curl -sS "$BASE_URL/api/execute_query?channel=$CHANNEL" $J \
  -d '{"query":"ВЫБРАТЬ Ссылка, Контрагент ИЗ Документ.РеализацияТоваровУслуг","limit":2}'
```

Response:
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
  ]
}
```

### 2. find_references_to_object results

The `found_in_object` field in hits contains an `object_description` (for documents/catalogs):

```json
{
  "found_in_object": {
    "_objectRef": true,
    "УникальныйИдентификатор": "...",
    "ТипОбъекта": "ДокументСсылка.РеализацияТоваровУслуг",
    "Представление": "Реализация №001 от 01.01.2024"
  }
}
```

## Where it is required as input

### 1. get_link_of_object

Pass `object_description` to generate a navigation link:

```sh
curl -sS "$BASE_URL/api/get_link_of_object?channel=$CHANNEL" $J \
  -d '{"object_description":{"_objectRef":true,"УникальныйИдентификатор":"ba7e5a3d-1234-5678-9abc-def012345678","ТипОбъекта":"СправочникСсылка.Контрагенты"}}'
```

### 2. find_references_to_object

Pass as `target_object_description`:

```sh
curl -sS "$BASE_URL/api/find_references_to_object?channel=$CHANNEL" $J \
  -d '{"target_object_description":{"_objectRef":true,"УникальныйИдентификатор":"ba7e5a3d-1234-5678-9abc-def012345678","ТипОбъекта":"СправочникСсылка.Контрагенты"},"search_scope":["documents"]}'
```

### 3. get_event_log (object filter)

Pass as `object_description` to filter event log by object:

```sh
curl -sS "$BASE_URL/api/get_event_log?channel=$CHANNEL" $J \
  -d '{"object_description":{"_objectRef":true,"УникальныйИдентификатор":"ba7e5a3d-1234-5678-9abc-def012345678","ТипОбъекта":"СправочникСсылка.Контрагенты"},"limit":50}'
```

## Typical flow: query → extract → use

```
Step 1: execute_query  →  get object_description from result rows
Step 2: use that object_description as input to:
        - get_link_of_object (generate clickable link)
        - find_references_to_object (find where it is used)
        - get_event_log (view history for this object)
```
