"""Immutable technical ranges shared by the content-based anonymizers.

Explicit source policies intentionally bypass this module. Ranges are half-open
Python character offsets, never native UTF-16 offsets.
"""
import re
import unicodedata
from collections.abc import Callable, Iterator

from .token_mapper import TOKEN_RE

_HEX = r"[0-9a-fA-F]"
_UUID = rf"{_HEX}{{8}}-{_HEX}{{4}}-{_HEX}{{4}}-{_HEX}{{4}}-{_HEX}{{12}}"
_NAME = r"[^\W\d]\w*"
_TYPE = (r"Справочник|Документ|Перечисление|ПланСчетов|ПланВидовХарактеристик|"
         r"ПланВидовРасчета|ПланОбмена|БизнесПроцесс|Задача|"
         r"Catalog|Document|Enum|ChartOfAccounts|ChartOfCharacteristicTypes|"
         r"ChartOfCalculationTypes|ExchangePlan|BusinessProcess|Task")
_TECH = re.compile(rf"e1cib/data/(?:{_TYPE})\.{_NAME}\?ref={_HEX}{{32}}|{_UUID}|{_HEX}{{32}}")


def _identifier_char(ch: str) -> bool:
    code = ord(ch)
    if code < 128:
        return ch.isalnum() or ch in "_-"
    # Match the pure BSL recognizer: unknown non-ASCII characters are not
    # treated as word boundaries. Only known whitespace/punctuation are.
    return not (code in (160, 171, 187, 5760) or 8192 <= code <= 8303
                or 12288 <= code <= 12292 or 12296 <= code <= 12319)


def technical_spans(text: str) -> list[tuple[int, int]]:
    return [(m.start(), m.end()) for m in _TECH.finditer(text)
            if (m.start() == 0 or not _identifier_char(text[m.start() - 1]))
            and (m.end() == len(text) or not _identifier_char(text[m.end()]))]


def protected_spans(text: str) -> list[tuple[int, int]]:
    candidates = technical_spans(text) + [(m.start(), m.end()) for m in TOKEN_RE.finditer(text)]
    merged: list[tuple[int, int]] = []
    for start, end in sorted(candidates):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    return merged


def is_technical_value(text: str) -> bool:
    return bool(text) and technical_spans(text) == [(0, len(text))]


def unprotected_spans(text: str) -> Iterator[tuple[int, int]]:
    last = 0
    for start, end in protected_spans(text):
        if last < start:
            yield last, start
        last = end
    if last < len(text):
        yield last, len(text)


def map_unprotected(text: str, transform: Callable[[str], str]) -> str:
    parts: list[str] = []
    last = 0
    for start, end in unprotected_spans(text):
        parts.extend((text[last:start], transform(text[start:end])))
        last = end
    parts.append(text[last:])
    return "".join(parts)


def tokenize_content(text: str, tokenize: Callable[[str], str]) -> str:
    """Keep separators; do not reclassify short/ISO-looking content after splitting."""
    def line(match):
        value = match.group()
        positions = [i for i, ch in enumerate(value) if unicodedata.category(ch)[0] in "LNM"]
        if not positions:
            return value
        start, end = positions[0], positions[-1] + 1
        return value[:start] + tokenize(value[start:end]) + value[end:]

    return re.sub(r"[^\r\n]+", line, text)
