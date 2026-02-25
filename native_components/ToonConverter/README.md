# ToonConverter — 1C Native API Component

C++ компонента для загрузки внутрь процесса 1С:Предприятие. Конвертирует JSON в формат TOON (Text Object Notation) — компактное текстовое представление данных.

## Требования

- Windows (x86 или x64)
- CMake 3.16+
- Visual Studio 2022 (MSVC)

## Сборка

Собирать нужно под разрядность платформы 1С, на которой будет работать компонента.

```bash
cd native_components/ToonConverter

# x64
cmake -B build -A x64
cmake --build build --config Release
# Результат: build/Release/ToonConverter.dll

# x86
cmake -B build_x86 -A Win32
cmake --build build_x86 --config Release
# Результат: build_x86/Release/ToonConverter.dll
```

## Установка в обработку

Скопируйте DLL нужной разрядности в макет обработки под именем `Template.bin`:

```bash
# x64
cp build/Release/ToonConverter.dll \
   ../../1c/MCPToolkit/MCPToolkit/Templates/ToonConverter/Ext/Template.bin

# x86
cp build_x86/Release/ToonConverter.dll \
   ../../1c/MCPToolkit/MCPToolkit/Templates/ToonConverter/Ext/Template.bin
```

После обновления Template.bin нужно пересохранить обработку в конфигураторе 1С.

## API

### Методы

| Метод | Псевдоним (RU) | Параметры | Возврат | Описание |
|-------|----------------|-----------|---------|----------|
| `JsonToToon(json)` | `JsonВТун` | Строка | Строка | Преобразовать JSON-строку в TOON-формат |

### Свойства

Компонента не имеет свойств.

### ВнешниеСобытия

Компонента не генерирует внешних событий.

## Формат TOON

TOON — компактный текстовый формат. Компонента автоматически выбирает представление:

- **Табличный формат** — если входной JSON является массивом объектов с одинаковыми ключами, где хотя бы одно поле содержит вложенные данные (объект или массив):

  ```
  [3]{id,name,meta}:
    1,Alice,{role: admin}
    2,Bob,{role: user}
    3,Carol,{role: user}
  ```

- **Стандартный ctoon** — во всех остальных случаях, через библиотеку [ctoon](third_party/ctoon/).

При ошибке разбора JSON возвращается пустая строка.

## Зависимости

| Библиотека | Расположение | Назначение |
|------------|-------------|------------|
| ctoon | `third_party/ctoon/` | Парсинг JSON и кодирование TOON |
| yyjson | `third_party/ctoon/src/sources/` | Быстрый JSON-парсер (используется внутри ctoon) |
