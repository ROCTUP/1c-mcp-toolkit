# -*- coding: utf-8 -*-
"""Сверяет общие утилиты, продублированные в модуле формы и в модуле объекта.

Серверное ядро обработки живет в Ext/ObjectModule.bsl: фоновое задание видит
только модуль объекта. Но 17 утилит остались и в модуле формы — их зовет
клиентский код, а клиент до модуля объекта не дотягивается. Копии обязаны
совпадать дословно; разошлись — два пути начинают вести себя по-разному без
единого симптома.

Запускать перед каждой сборкой .epf. Возвращает 1, если хоть одна пара
отличается или потеряна.
"""
import io
import re
import sys

# Утилиты, живущие в обоих модулях. Список закрытый: он равен замыканию тех
# процедур ядра, которые вызываются из кода, остающегося в форме.
SHARED = [
    "ПолучитьСтроковоеПредставлениеТипаДляСхемы", "СоединитьМассив", "ЭтоКоллекция",
    "РазделитьСтрокуПоРазделителю", "URLDecode", "HexВЧисло", "БайтыUTF8ВСтроку",
    "ПрочитатьJSON2", "НормализоватьМассивПараметра",
    "НастройкаАвтоматическоеПриведениеОбъектаКСтруктуре", "jsonПрочитатьИнициализация",
    "jsonПрочитатьПропуститьФорматирование", "jsonПрочитать", "jsonПрочитатьСтроку",
    "ЭтоСсылка", "СтроковоеПредставлениеТипа", "СтрокаСоответствуетISO8601ДатаВремя",
]

START = re.compile(r"^(Процедура|Функция)\s+([A-Za-zА-Яа-яЁё_0-9]+)\s*\(")
END = re.compile(r"^(КонецПроцедуры|КонецФункции)")


def bodies(path):
    """Имя процедуры -> ее текст, без директивы компиляции над ней."""
    lines = io.open(path, encoding="utf-8-sig").read().split("\n")
    found = {}
    i = 0
    while i < len(lines):
        m = START.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(2)
        j = i + 1
        while j < len(lines) and not END.match(lines[j]):
            j += 1
        if j >= len(lines):
            sys.exit("незакрытая процедура в %s, строка %d: %s" % (path, i + 1, name))
        if name in found:
            sys.exit("дубль имени %s в %s" % (name, path))
        found[name] = "\n".join(lines[i:j + 1]).rstrip()
        i = j + 1
    return found


def main():
    form_path, object_path = sys.argv[1], sys.argv[2]
    form, obj = bodies(form_path), bodies(object_path)

    problems = []
    for name in SHARED:
        if name not in form:
            problems.append("нет в модуле формы: " + name)
        elif name not in obj:
            problems.append("нет в модуле объекта: " + name)
        elif form[name] != obj[name]:
            problems.append("копии разошлись: " + name)

    if problems:
        for p in problems:
            print(p)
        return 1

    print("общие утилиты совпадают: %d процедур" % len(SHARED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
