# QueryLineageAnalyzer - 1C Native API Component

C++ компонента для загрузки внутрь процесса 1С:Предприятие. Анализирует текст запроса 1С и обогащает `schema` результата `execute_query` полем `sources`.

## Требования

- Windows (x86 или x64): CMake 3.16+, Visual Studio 2022 (MSVC)
- Linux: Docker Desktop (сборка через контейнер)
- Доступ в интернет на этапе `cmake`, если `include/nlohmann/json.hpp` ещё не закеширован локально

## Сборка

### Windows

```bash
cd native_components/QueryLineageAnalyzer

# x64
cmake -B build -A x64
cmake --build build --config Release

# x86
cmake -B build_x86 -A Win32
cmake --build build_x86 --config Release

# optional native tests
cmake -B build_tests -A x64 -DQUERY_LINEAGE_BUILD_TESTS=ON
cmake --build build_tests --config Release
ctest --test-dir build_tests -C Release
```

### Linux (через Docker)

```bat
native_components\QueryLineageAnalyzer\build_linux.bat
```

Результат: `native_components/QueryLineageAnalyzer/build_linux/QueryLineageAnalyzer.so`

Скрипт собирает образ на базе Ubuntu 20.04 (glibc 2.31) — подходит для RedOS 8.x и совместимых дистрибутивов. Для RedOS 7.x замените базовый образ в `Dockerfile.linux` на `debian:buster`.

## Установка в обработку

Скопировать DLL/SO, соответствующую ОС и разрядности процесса 1С, в макет обработки под именем `Template.bin`:

```bash
# Windows x64
cp build/Release/QueryLineageAnalyzer.dll \
  ../../1c/MCPToolkit/MCPToolkit/Templates/QueryLineageAnalyzer/Ext/Template.bin

# Windows x86
cp build_x86/Release/QueryLineageAnalyzer.dll \
  ../../1c/MCPToolkit/MCPToolkit/Templates/QueryLineageAnalyzer/Ext/Template.bin

# Linux
cp build_linux/QueryLineageAnalyzer.so \
  ../../1c/MCPToolkit/MCPToolkit/Templates/QueryLineageAnalyzer/Ext/Template.bin
```

После замены `Template.bin` необходимо пересобрать EPF и перезапустить клиентский сеанс 1С.

## API

### Методы

| Method | Alias RU | Params | Return | Description |
|--------|----------|--------|--------|-------------|
| `AnalyzeSources` | `АнализироватьИсточники` | `queryText`, `schemaJson` | `string` | Обогащает `schema` полем `sources` |
| `Version` | `Версия` | - | `string` | Версия компоненты |
