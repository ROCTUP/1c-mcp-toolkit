#!/usr/bin/env bash
# Сверка общих утилит между модулем формы и модулем объекта обработки MCPToolkit.
# Запускать перед каждой сборкой .epf.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

PYTHONUTF8=1 python "$HERE/check_shared.py" \
	"$REPO/1c/MCPToolkit/MCPToolkit/Forms/Форма/Ext/Form/Module.bsl" \
	"$REPO/1c/MCPToolkit/MCPToolkit/Ext/ObjectModule.bsl"
