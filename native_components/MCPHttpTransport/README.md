# MCPHttpTransport — 1C Native API Component

C++ HTTP-сервер для загрузки внутрь процесса 1С:Предприятие. Принимает MCP Streamable HTTP и REST запросы напрямую, без Python-прокси.

## Требования

- Windows (x86 или x64)
- CMake 3.16+
- Visual Studio 2022 (MSVC)

## Сборка

Собирать нужно под разрядность платформы 1С, на которой будет работать компонента.

```bash
cd native_components/MCPHttpTransport

# x64
cmake -B build -A x64
cmake --build build --config Release
# Результат: build/Release/MCPHttpTransport.dll

# x86
cmake -B build_x86 -A Win32
cmake --build build_x86 --config Release
# Результат: build_x86/Release/MCPHttpTransport.dll
```

## Установка в обработку

Скопируйте DLL нужной разрядности в макет обработки под именем `Template.bin`:

```bash
# x64
cp build/Release/MCPHttpTransport.dll \
   ../../1c/MCPToolkit/MCPToolkit/Templates/MCPHttpTransport/Ext/Template.bin

# x86
cp build_x86/Release/MCPHttpTransport.dll \
   ../../1c/MCPToolkit/MCPToolkit/Templates/MCPHttpTransport/Ext/Template.bin
```

После обновления Template.bin нужно пересохранить обработку в конфигураторе 1С.

## API

### Методы

| Метод | Параметры | Возврат | Описание |
|-------|-----------|---------|----------|
| `Start(port)` | Число | Boolean | Запустить HTTP-сервер на 127.0.0.1:port |
| `Stop()` | — | Boolean | Остановить сервер |
| `SendResponse(requestId, statusCode, headersJson, body)` | Строки/Число | Boolean | Отправить HTTP-ответ |
| `SendSSEEvent(requestId, eventData, headersJson)` | Строки | Boolean | Отправить SSE-событие |
| `CloseSSEStream(requestId)` | Строка | Boolean | Закрыть SSE-поток |
| `GetRequestBody(requestId)` | Строка | Строка | Получить тело запроса (для тел > 64KB) |

### Свойства

| Свойство | Тип | R/W | Default |
|----------|-----|-----|---------|
| `IsRunning` | Boolean | R | — |
| `Port` | Number | R | — |
| `RequestTimeout` | Number | R/W | 180 |
| `MaxConcurrentRequests` | Number | R/W | 10 |

### ВнешниеСобытия

| Тип | Описание |
|-----|----------|
| `REQUEST` | REST, health, DELETE /mcp |
| `MCP_POST` | POST /mcp |
| `SSE_CONNECT` | GET /mcp |
| `SSE_CLOSED` | SSE-поток закрылся |
