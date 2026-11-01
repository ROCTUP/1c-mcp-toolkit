# MCPHttpTransport — 1C Native API Component

C++ HTTP-сервер для загрузки внутрь процесса 1С:Предприятие. Принимает MCP Streamable HTTP и REST запросы напрямую, без Python-прокси.

## Требования

- Windows (x86 или x64), либо Linux (x86_64)
- CMake 3.16+
- Visual Studio 2022 (MSVC) — для Windows
- Docker Desktop — для Linux-сборки

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

## Сборка под Linux

Требуется Docker Desktop. Запустите из корня репозитория:

```bat
native_components\MCPHttpTransport\build_linux.bat
```

Результат: `build_linux/MCPHttpTransport.so`

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

```bash
# Linux
cp build_linux/MCPHttpTransport.so \
   ../../1c/MCPToolkit/MCPToolkit/Templates/MCPHttpTransport/Ext/Template.bin
```

После обновления Template.bin нужно пересохранить обработку в конфигураторе 1С.

## API

### Методы

| Метод | Параметры | Возврат | Описание |
|-------|-----------|---------|----------|
| `Start(port, bindAddress="0.0.0.0")` | Число, строка | Boolean | Запустить HTTP-сервер с доставкой через `ExternalEvent` |
| `Stop()` | — | Boolean | Остановить сервер |
| `SendResponse(requestId, statusCode, headersJson, body)` | Строки/Число | Boolean | Отправить HTTP-ответ |
| `SendSSEEvent(requestId, eventData, headersJson)` | Строки | Boolean | Отправить SSE-событие |
| `CloseSSEStream(requestId)` | Строка | Boolean | Закрыть SSE-поток |
| `GetRequestBody(requestId)` | Строка | Строка | Получить тело запроса (для тел > 64KB) |
| `GetRequestStates(requestIds)` | Строка: до 16 внутренних UUID через запятую | JSON | Состояние и оставшийся срок уже известных запросов; пустая строка проверяет поддержку протокола |
| `StartQueued(port, bindAddress="0.0.0.0")` | Число, строка | Boolean | Запустить HTTP-сервер с очередью событий для серверного BSL |
| `WaitNextEvent(timeoutMs)` | Целое число 0–5000 | JSON | Получить событие, дождаться события или остановки |
| `WaitMilliseconds(timeoutMs)` | Целое число 0–5000 | Boolean | Ограниченная пауза серверного подключения к прокси; работает без слушающего порта |
| `SendResponseAndWait(requestId, statusCode, headersJson, body, timeoutMs)` | Как `SendResponse` + целое 1–10000 мс | Строка | Отправить финальный непустой REST-ответ и дождаться записи HTTP-транспортом |
| `SendSSEEventAndWait(requestId, eventData, headersJson, eventType, timeoutMs)` | Как `SendSSEEvent` + целое 1–10000 мс | Строка | Отправить последнее событие, закрыть выбранный SSE-поток и дождаться записи |

`GetRequestStates` возвращает `{version: 1, terminal_response_wait_version: 1, items: [{id, active, remaining_ms}]}`.
Отсутствующий или завершённый запрос имеет `active=false`. Для открытого SSE-потока
`remaining_ms=-1`: срок отдельной MCP-команды учитывается обработкой, а завершение
короткого legacy POST не завершает команду. Наблюдение не создаёт запросы и работает
при заполненном лимите входящих HTTP-запросов.

Методы `AndWait` имеют индексы 11 и 12. Возвращают `sent`, `failed`, `timeout`
или `rejected`. `sent` означает запись полного ответа транспортом ОС, а не
подтверждение прочтения агентом. `timeout` завершает ожидание, но не повторяет
ответ; позднее завершение освобождает ресурсы. `Stop`/`Done` отменяют активное
ожидание. Обычные методы отправки остаются неблокирующими.

Основная форма проверяет `terminal_response_wait_version` независимо от
параллельного выполнения. С прежней компонентой встроенные close/restart
отклоняются до подготовки запуска; остальные инструменты продолжают работать.

При конфигурации CMake создаёт в каталоге сборки исправленную копию cpp-httplib:
ошибка записи HTTP-заголовков прерывает отправку тела. Все targets используют
эту копию; скачанный `include/httplib.h` остаётся неизменным. При обновлении
cpp-httplib необходимо сохранить эту проверку: неизвестный или неоднозначный
контекст останавливает конфигурацию. Тест с отказом записи заголовков проверяет,
что тело не отправляется и результат не становится `sent`.

### Очередь для серверного BSL

`Start` использует только `ExternalEvent`, `StartQueued` — только очередь. Повторный
запуск работающего экземпляра возвращает `false` и не переключает способ доставки.
Методы очереди добавлены после прежних восьми методов, их индексы — 8 и 9.
`WaitMilliseconds` имеет индекс 10. Он не запускает HTTP-сервер и не выдаёт
события; используется для паузы между неудачными подключениями к прокси.
Вызов завершается по указанному интервалу; `Done` дожидается его завершения.
Поддержку можно проверить через Native API `FindMethod("StartQueued")`;
наличие `GetRequestStates` само по себе не означает наличие очереди.

`WaitNextEvent` возвращает JSON одного из трёх видов:

```json
{"version":1,"outcome":"event","event":"MCP_POST","data":{"id":"...","method":"POST","path":"/mcp","query":{},"headers":{},"request_state_version":1,"remaining_ms":179900,"body":"{}","bodyTruncated":false}}
{"version":1,"outcome":"timeout"}
{"version":1,"outcome":"stopped"}
```

`data` имеет ту же структуру, что данные соответствующего `ExternalEvent`.
Поддерживаются `REQUEST`, `MCP_POST`, `SSE_CONNECT`, `SSE_LEGACY_CONNECT`,
`SSE_LEGACY_MESSAGE`, `SSE_CLOSED`. У закрытия потока `data` содержит только `id`.
ID запроса сохраняется; для большого тела используется прежний `GetRequestBody`.

Нулевой интервал означает неблокирующее чтение. Появление события и `Stop` прерывают
ожидание сразу. Отрицательные, дробные, нечисловые и превышающие 5000 значения
Native API отклоняет. До запуска очереди, после остановки и в режиме `Start`
ожидание возвращает `stopped`. Ожидание от прежнего запуска не получает события
следующего запуска.

Очередь вмещает до 64 обычных событий. Бюджет данных, служебного запаса событий
и резервов закрытия — 16 МиБ. При приёме SSE-подключения отдельно резервируются
256 байт и одно из 64 мест для будущего закрытия. Место сохраняется до выдачи
`SSE_CLOSED` либо отказа в подключении. Закрытия имеют приоритет перед обычными
событиями; повторные уведомления одного потока объединяются. Таким образом,
заполнение обычной очереди не теряет закрытие, а новые подключения при исчерпании
резерва получают отказ. Это внутренние пределы, отдельные от `MaxConcurrentRequests`.

При отказе доставки HTTP получает 503, pending-запрос удаляется, счётчик активных
операций освобождается. Legacy POST получает 202 только после успешной постановки
сообщения; предел тела 1 МиБ сохраняется, включая запас на JSON-экранирование.
Отказ `ExternalEvent` также завершает HTTP-запрос ошибкой вместо ожидания таймаута.

Срок фиксируется при приёме и не продлевается очередью: при выдаче уменьшается
`data.remaining_ms`. Завершённые и просроченные запросы не выдаются исполнителю.
В режиме очереди срок действует и на ожидание решения по SSE-подключению; после
открытия потока его время жизни не ограничено `RequestTimeout`. Перед выполнением
инструмента BSL дополнительно проверяет собственную отмену, сеанс и исходный срок,
для известных HTTP-запросов — `GetRequestStates`. Native-очередь не исполняет
`notifications/cancelled` и не заменяет диспетчер MCP.

`Stop` прекращает доставку, очищает очередь, будит ожидания, завершает pending/SSE
и дожидается HTTP-потоков. Повторная остановка допустима. `Done` сохраняет allocator
до завершения уже начатого вызова `WaitNextEvent`; HTTP join выполняется без mutex
вызова `ExternalEvent`.

### Native-проверки

При конфигурации с `-DMCPHTTPTRANSPORT_SMOKE_TEST=ON` собираются `mcp_smoke_test`
и `mcp_queue_test`, зарегистрированные в CTest. Запускать `ctest -C Release
--output-on-failure` из соответствующего каталога сборки. Linux-скрипт сборки
выполняет оба теста автоматически.

Проверки используют локальные HTTP-соединения: обычный `ExternalEvent` и очередь,
MCP/REST/legacy/SSE, переполнение, размеры, сроки, остановку и повторный запуск,
закрытие SSE при заполненной очереди, завершение компоненты с ожидающим вызовом.
Это native-тесты транспорта, а не проверка готового серверного режима обработки в 1С.

События входящих запросов содержат `request_state_version=1` и `remaining_ms`.
Срок фиксируется при приёме HTTP-запроса; изменение `RequestTimeout` не продлевает
уже принятые запросы. Отмена MCP дополнительно обрабатывается формой по
`notifications/cancelled` с проверкой сессии и типа JSON-RPC ID.

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
| `SSE_LEGACY_CONNECT` | GET /mcp без заголовков Streamable HTTP |
| `SSE_LEGACY_MESSAGE` | POST /mcp/message |
| `SSE_CLOSED` | SSE-поток закрылся |
