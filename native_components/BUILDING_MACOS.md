# Сборка нативных компонент под macOS

Встроенный HTTP-сервер (`MCPHttpTransport`) и остальные нативные компоненты
работают на macOS нативно — Python-прокси не нужен. Требуется только собрать
библиотеки под macOS как **universal (x86_64 + arm64)**.

## TL;DR

```bash
# по одной компоненте
bash native_components/MCPHttpTransport/build_macos.sh
# → native_components/MCPHttpTransport/build_macos/MCPHttpTransport.dylib (universal, ad-hoc signed)
```

Готовые `.dylib` для всех компонент также собирает CI
(`.github/workflows/build-macos.yml`, раннер `macos-14`) и публикует артефактами —
**физический Mac для этого не нужен**.

Требования для локальной сборки: CMake 3.16+, Xcode Command Line Tools
(`xcode-select --install`). Docker/Python не нужны.

## Два критичных момента (иначе компонента/обработка не заведётся)

### 1. Только universal или x86_64 — НЕ arm64-only

Клиент 1С:Предприятие под macOS поставляется как **x86_64** и на Apple Silicon
работает через Rosetta 2. arm64-only библиотека не загрузится в x86_64-процесс:

```
Тип не определен (AddIn.MCPHttp.MCPHttpTransport)
```

Поэтому `build_macos.sh` собирает с `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"`.
Проверить клиент: `lipo -archs /opt/1cv8/<версия>/1cv8` → должно быть `x86_64`.

### 2. .epf пересохранять Конфигуратором НЕ новее рантайма

Если собрать `.epf` Конфигуратором версии новее, чем платформа, на которой его
открывают, при открытии будет:

```
ошибка формата потока
```

Собирайте `.epf` самой старой из используемых версий платформы.

## Встраивание в обработку

Как и для Windows/Linux, библиотека кладётся сырым бинарником в макет
(одна разрядность на `.epf`):

```bash
cp native_components/MCPHttpTransport/build_macos/MCPHttpTransport.dylib \
   1c/MCPToolkit/MCPToolkit/Templates/MCPHttpTransport/Ext/Template.bin
```

Аналогично для `QueryLineageAnalyzer`, `RegexHelper`, `SyntaxHelpReader`,
`ToonConverter`. После замены `Template.bin` пересохраните `.epf` в Конфигураторе
(на любой ОС — `.epf` кроссплатформенный, маковый только встроенный бинарник):

```bash
1cv8 DESIGNER /F<scratch_ib> \
  /LoadExternalDataProcessorOrReportFromFiles \
     1c/MCPToolkit/MCPToolkit.xml build/MCP_Toolkit_mac.epf
```

## ScreenCapture

`ScreenCapture` не переносится на macOS (использует Windows GDI, нет
не-Windows ветки), поэтому в CI и `build_macos.sh` не входит. На macOS его макет
остаётся Windows-DLL — обработка при старте выдаёт одно предупреждение, а
`get_screenshot` недоступен. Остальной функционал (`execute_query`,
`execute_code`, навигация, TOON, regex, справка по синтаксису) работает.
