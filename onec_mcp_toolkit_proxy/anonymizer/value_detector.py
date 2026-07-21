"""
Multi-level sensitive data detection for anonymization.

Detection priority:
  Level 0: Key-aware (SENSITIVE_KEY_SUBSTRINGS + NON_SENSITIVE_KEY_SUBSTRINGS dual filter)
  Level 1: Regex patterns (full match for values, search for inline)
  Level 2: SpaCy NER (optional, graceful degradation)
  Level 3: Radical mode fallback (tokenize everything)
"""
import re
import json
import logging
from typing import Dict, Optional, Sequence

from ..config import settings
from .token_mapper import TOKEN_RE as _TOKEN_RE
from .anonymization_defaults import (
    KNOWN_SAFE_VALUES,
    SKIP_KEYS,
    _1C_TECH_ENTITY_SUBSTRINGS,
    _DEFAULT_NON_SENSITIVE_KEY_SUBSTRINGS,
    _DEFAULT_SENSITIVE_KEY_EXACT,
    _DEFAULT_SENSITIVE_KEY_SUBSTRINGS,
    _DEFAULT_SKIP_PREFIXES,
    _NER_ERROR_STOPWORD_SUBSTRINGS,
    is_error_inline_anonymization_exclusion,
)

logger = logging.getLogger(__name__)

_CARD_CANDIDATE_RE = re.compile(r"(?<!\d)(?:\d[ -]?){12,18}\d(?!\d)")

_IDENT_CHAR_RE = re.compile(r"[0-9A-Za-zА-Яа-яЁё_]")
_IDENTIFIER_TOKEN_RE = re.compile(r"^[A-Za-zА-Яа-яЁё_][0-9A-Za-zА-Яа-яЁё_]*$")

_1C_METADATA_PREFIX_RE = re.compile(
    r"^(?:"
    r"Справочник|Документ|Регистр|Перечисление|Отчет|Обработка|ВнешняяОбработка"
    r"|Catalog|Document|Register|Enum|Report|DataProcessor"
    r")\.",
    re.IGNORECASE,
)


def _luhn_is_valid(digits: str) -> bool:
    """Return True if `digits` pass Luhn checksum (bank card PAN)."""
    if not digits or not digits.isdigit():
        return False
    if digits[0] == "0":
        return False
    if len(set(digits)) == 1:
        return False

    total = 0
    for index, ch in enumerate(reversed(digits)):
        d = ord(ch) - 48
        if index % 2 == 1:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return total % 10 == 0


def _sub_outside_tokens(text: str, pattern: re.Pattern, repl) -> str:
    """Apply regex substitution only outside already tokenized substrings.

    Prevents nested tokenization like turning '[PASSPORT-0001]' into a SWIFT
    match on 'PASSPORT'.
    """
    if not text:
        return text
    parts: list[str] = []
    last = 0
    for m in _TOKEN_RE.finditer(text):
        if m.start() > last:
            parts.append(pattern.sub(repl, text[last:m.start()]))
        parts.append(m.group(0))
        last = m.end()
    if last < len(text):
        parts.append(pattern.sub(repl, text[last:]))
    return "".join(parts)

def _spans_intersect(a_start: int, a_end: int, b_start: int, b_end: int) -> bool:
    return not (a_end <= b_start or a_start >= b_end)


def _overlaps_any_token(text: str, start: int, end: int) -> bool:
    """Return True if [start:end) overlaps an existing token span."""
    for m in _TOKEN_RE.finditer(text):
        if _spans_intersect(start, end, m.start(), m.end()):
            return True
    return False


def _looks_like_1c_identifier_fragment(text: str) -> bool:
    """Heuristic: True for 1C-like identifiers (field/module names), not real PII."""
    if not text or " " in text:
        return False
    if not _IDENTIFIER_TOKEN_RE.fullmatch(text):
        return False
    lower = text.lower()
    if any(s in lower for s in _1C_TECH_ENTITY_SUBSTRINGS):
        return True
    return False


def _is_dotted_identifier_context(text: str, start: int, end: int) -> bool:
    """True if entity looks like part of a dotted identifier chain (Xxx.Yyy)."""
    if start < 0 or end < 0 or start > end or end > len(text):
        return False

    # prev is "...Xxx.<ENT>" where Xxx is identifier-like
    if start >= 2 and text[start - 1] == ".":
        prev_ch = text[start - 2]
        if _IDENT_CHAR_RE.match(prev_ch):
            return True

    # next is "<ENT>.Yyy..." where Yyy is identifier-like
    if end + 1 < len(text) and text[end] == ".":
        next_ch = text[end + 1]
        if _IDENT_CHAR_RE.match(next_ch):
            return True

    return False


# ---------------------------------------------------------------------------
# Key-aware detection (Level 0)
# ---------------------------------------------------------------------------

_NON_SENSITIVE_BASE = (
    settings.anonymization_non_sensitive_keys
    or _DEFAULT_NON_SENSITIVE_KEY_SUBSTRINGS
)
_NON_SENSITIVE_ADD = getattr(settings, "anonymization_non_sensitive_keys_add", None)
NON_SENSITIVE_KEY_SUBSTRINGS = (
    frozenset(set(_NON_SENSITIVE_BASE).union(_NON_SENSITIVE_ADD))
    if _NON_SENSITIVE_ADD
    else _NON_SENSITIVE_BASE
)

def _merge_key_substrings_dict(
    default_map: Dict[str, str],
    override_map: Optional[Dict[str, str]],
    add_map: Optional[Dict[str, str]],
) -> Dict[str, str]:
    """Build effective key-substring map from defaults/override/add.

    Precedence:
      1) override_map replaces defaults (if provided)
      2) add_map is then merged (updates existing keys / adds new ones)
    """
    effective = dict(override_map) if override_map is not None else dict(default_map)
    if add_map:
        effective.update(add_map)
    return effective


SENSITIVE_KEY_SUBSTRINGS = _merge_key_substrings_dict(
    _DEFAULT_SENSITIVE_KEY_SUBSTRINGS,
    settings.anonymization_sensitive_keys,
    getattr(settings, "anonymization_sensitive_keys_add", None),
)

SENSITIVE_KEY_EXACT = _merge_key_substrings_dict(
    _DEFAULT_SENSITIVE_KEY_EXACT,
    getattr(settings, "anonymization_sensitive_key_exact", None),
    getattr(settings, "anonymization_sensitive_key_exact_add", None),
)


def _detect_by_key(key: str) -> Optional[str]:
    """Detect category by key name (exact match + substring match).

    SENSITIVE wins on conflict: НомерТелефона → "телефон" in SENSITIVE → PHONE,
    even though NON might also match.
    """
    k = key.lower()
    # 0. SENSITIVE exact-match first
    exact_cat = SENSITIVE_KEY_EXACT.get(k)
    if exact_cat:
        return exact_cat
    # 1. SENSITIVE — check first
    sensitive_cat = None
    for pattern, category in SENSITIVE_KEY_SUBSTRINGS.items():
        if pattern in k:
            sensitive_cat = category
            break
    # 2. If SENSITIVE matched — it wins
    if sensitive_cat:
        return sensitive_cat
    # 3. NON — only when SENSITIVE did not match
    for non_pattern in NON_SENSITIVE_KEY_SUBSTRINGS:
        if non_pattern in k:
            return None
    return None


def _detect_by_key_candidates(keys: Sequence[str]) -> Optional[str]:
    """Return the first non-None _detect_by_key result across key candidates."""
    for key in keys:
        if not isinstance(key, str):
            continue
        normalized = key.strip()
        if not normalized:
            continue
        category = _detect_by_key(normalized)
        if category is not None:
            return category
    return None


# ---------------------------------------------------------------------------
# Regex patterns (Level 1)
# ---------------------------------------------------------------------------

# Full-match patterns (for standalone values like table cells)
PATTERNS = [
    # Phone: international format (+X... with 7-15 digits)
    ("PHONE",    re.compile(r"^\+\d[\d\s\-()]{6,18}\d$")),
    # Phone (RU): +7 / 8 with separators
    ("PHONE",    re.compile(
        r"^(?:\+7|8)[\s\-]?\(?\d{3}\)?[\s\-]?\d{3}[\s\-]?\d{2}[\s\-]?\d{2}$"
    )),
    # Phone (RU): +7XXXXXXXXXX / 8XXXXXXXXXX
    ("PHONE",    re.compile(r"^(?:\+7|8)\d{10}$")),
    # Email
    ("EMAIL",    re.compile(r"^[\w.+\-]+@[\w.\-]+\.\w{2,}$")),
    # SNILS: 123-456-789 01 or 123 456 789 01 (separators required)
    ("SNILS",    re.compile(r"^\d{3}[-\s]\d{3}[-\s]\d{3}\s?\d{2}$")),
    # BIK (Russia): 9 digits, usually starts with 04
    ("BIK",      re.compile(r"^04\d{7}$")),
    # Bank account: 20 digits starting with 3 or 4 (MVP heuristic).
    ("ACC",      re.compile(r"^[34]\d{19}$")),
    # SWIFT/BIC: 8 or 11 chars
    ("SWIFT",    re.compile(r"^[A-Z]{4}[A-Z]{2}[A-Z0-9]{2}([A-Z0-9]{3})?$", re.I)),
    # IBAN: 15–34 chars total
    ("IBAN",     re.compile(r"^[A-Z]{2}\d{2}[A-Z0-9]{11,30}$", re.I)),
    # FIO: three capitalized words
    ("PER",      re.compile(r"^[А-ЯЁ][а-яё]+\s[А-ЯЁ][а-яё]+\s[А-ЯЁ][а-яё]+$")),
    # FIO with initials: Иванов И.И. or Иванов И. И.
    ("PER",      re.compile(r"^[А-ЯЁ][а-яё]+\s[А-ЯЁ]\.\s?[А-ЯЁ]\.$")),
    # Organization with legal form prefix
    ("ORG",      re.compile(
        r'^(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)\s+', re.I)),
    # Organization with legal form prefix but no space before quotes: ЗАО"Клауст"
    ("ORG",      re.compile(
        r'^(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)\s*["\u00ab«].+',
        re.I)),
    # Organization with full legal form (Russian) prefix: Общество с ограниченной ответственностью "..."
    ("ORG",      re.compile(
        r'^(Общество\s+с\s+ограниченной\s+ответственностью'
        r'|Открытое\s+акционерное\s+общество'
        r'|Закрытое\s+акционерное\s+общество'
        r'|Публичное\s+акционерное\s+общество'
        r'|Непубличное\s+акционерное\s+общество'
        r'|Акционерное\s+общество'
        r'|Индивидуальный\s+предприниматель'
        r'|Федеральное\s+государственное\s+унитарное\s+предприятие'
        r'|Государственное\s+унитарное\s+предприятие'
        r'|Муниципальное\s+унитарное\s+предприятие'
        r'|Некоммерческая\s+организация)\s*["\u00ab«]?.+',
        re.I)),
    # Organization with legal form suffix: Клауст ЗАО
    ("ORG",      re.compile(
        r'^.+\s+(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)\.?\s*$',
        re.I)),
    # Organization with quoted name and suffix without space: "Клауст"ЗАО
    ("ORG",      re.compile(
        r'^["\u00ab«].+["\u00bb»]\s*(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)\.?\s*$',
        re.I)),
    # Organization with short legal form (English) prefix: LLC Klaust
    ("ORG",      re.compile(
        r'^(LLC|LTD|INC|CORP|CO|GMBH|PLC|LLP|LP)\.?\s+.+',
        re.I)),
    # Organization with short legal form (English) suffix: Klaust LLC
    ("ORG",      re.compile(
        r'^.+\s+(LLC|LTD|INC|CORP|CO|GMBH|PLC|LLP|LP)\.?\s*$',
        re.I)),
    # Organization with full legal form (English) prefix: Limited Liability Company Klaust
    ("ORG",      re.compile(
        r'^(Limited\s+Liability\s+Company'
        r'|Private\s+Limited\s+Company'
        r'|Public\s+Limited\s+Company'
        r'|Incorporated'
        r'|Corporation'
        r'|Company'
        r'|Limited)\s+.+',
        re.I)),
    # Organization with full legal form (English) suffix: Klaust Limited Liability Company
    ("ORG",      re.compile(
        r'^.+\s+(Limited\s+Liability\s+Company'
        r'|Private\s+Limited\s+Company'
        r'|Public\s+Limited\s+Company'
        r'|Incorporated'
        r'|Corporation'
        r'|Company'
        r'|Limited)\s*$',
        re.I)),
    # Passport: DD MM NNNNNN
    ("PASSPORT", re.compile(r"^\d{2}\s\d{2}\s\d{6}$")),
]

# Search patterns without anchors — find PII fragments inside longer strings
# Used via re.sub to replace fragments, not the whole value
SEARCH_PATTERNS = [
    ("PHONE",    re.compile(r"\+\d[\d\s\-()]{6,18}\d")),
    # Phone (RU): +7 / 8 with separators
    ("PHONE",    re.compile(
        r"(?<!\d)(?:\+7|8)[\s\-]?\(?\d{3}\)?[\s\-]?\d{3}[\s\-]?\d{2}[\s\-]?\d{2}(?!\d)"
    )),
    # Phone (RU): +7XXXXXXXXXX / 8XXXXXXXXXX
    ("PHONE",    re.compile(r"(?<!\d)(?:\+7|8)\d{10}(?!\d)")),
    ("EMAIL",    re.compile(r"[\w.+\-]+@[\w.\-]+\.\w{2,}")),
    # SNILS: dashed/spaced form
    ("SNILS",    re.compile(r"(?<!\d)\d{3}[-\s]\d{3}[-\s]\d{3}\s?\d{2}(?!\d)")),
    # SNILS: digits-only (11 digits)
    ("SNILS",    re.compile(r"(?<!\d)\d{11}(?!\d)")),
    # Passport: common compact forms in text
    # - 45 10 №123456
    # - 4510 123456
    ("PASSPORT", re.compile(r"(?<!\d)\d{2}\s?\d{2}\s*№\s*\d{6}(?!\d)")),
    ("PASSPORT", re.compile(r"(?<!\d)\d{4}\s+\d{6}(?!\d)")),
    # Passport: strict spaced form (legacy)
    ("PASSPORT", re.compile(r"\d{2}\s\d{2}\s\d{6}")),
    # BIK (Russia): keep it strict to avoid collisions with KPP (also 9 digits)
    ("BIK",      re.compile(r"\b04\d{7}\b")),
    # Bank account: 20 digits starting with 3 or 4 (MVP heuristic).
    ("ACC",      re.compile(r"\b[34]\d{19}\b")),
    # SWIFT/BIC: 8 or 11 chars
    ("SWIFT",    re.compile(r"\b[A-Z]{4}[A-Z]{2}[A-Z0-9]{2}([A-Z0-9]{3})?\b", re.I)),
    # IBAN: 15–34 chars total
    ("IBAN",     re.compile(r"\b[A-Z]{2}\d{2}[A-Z0-9]{11,30}\b", re.I)),
    # FIO: three capitalized words
    ("PER",      re.compile(r"[А-ЯЁ][а-яё]+\s[А-ЯЁ][а-яё]+\s[А-ЯЁ][а-яё]+")),
    # FIO with initials
    ("PER",      re.compile(r"[А-ЯЁ][а-яё]+\s[А-ЯЁ]\.\s?[А-ЯЁ]\.")),
    # Organization with legal form
    # Suffix form first to avoid partial matches like 'ЗАО (777...)' inside 'Клауст ЗАО (ИНН/КПП)'.
    ("ORG",      re.compile(
        r'\b[А-ЯЁA-Za-z0-9][^,\n;]{1,80}?\s+(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)\.?\b'
        r'(?=\s*(?:\(|$|,|;|\n))',
        re.I)),
    ("ORG",      re.compile(
        r'(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)\s+'
        r'(?=["\u00ab«A-Za-zА-ЯЁ0-9])'
        r'["\u00ab«]?[^"»\u00bb,;]{2,}["\u00bb»]?', re.I)),
    # Organization with legal form but no space before quotes: ЗАО"Клауст"
    ("ORG",      re.compile(
        r'(ООО|ОАО|ЗАО|ПАО|АО|ИП|НАО|ГУП|МУП|НКО|ФГУП)["\u00ab«]'
        r'[^"»\u00bb,;]{2,}["\u00bb»]?', re.I)),
    # Organization with short legal form (English), suffix form: Klaust LLC
    ("ORG",      re.compile(
        r'\b[A-Za-z0-9][A-Za-z0-9&.,\-\s]{1,80}\s+(LLC|LTD|INC|CORP|CO|GMBH|PLC|LLP|LP)\.?\b',
        re.I)),
    # Organization with full legal form (English), suffix form: Klaust Limited Liability Company
    ("ORG",      re.compile(
        r'\b[A-Za-z0-9][A-Za-z0-9&.,\-\s]{1,80}\s+'
        r'(Limited\s+Liability\s+Company|Private\s+Limited\s+Company|Public\s+Limited\s+Company|Incorporated|Corporation|Company|Limited)\b',
        re.I)),
    # INN: 10 or 12 digits (word boundaries)
    # INN: 10 or 12 digits (word boundaries). INN can't start with "00" (region code).
    ("INN",      re.compile(r"\b(?!00)\d{10}\b|\b(?!00)\d{12}\b")),
    # KPP: 9 digits
    ("KPP",      re.compile(r"\b\d{9}\b")),
    # OGRN: 13 or 15 digits
    ("OGRN",     re.compile(r"\b\d{13}\b|\b\d{15}\b")),
]


# ---------------------------------------------------------------------------
# SpaCy NER (Level 2, optional)
# ---------------------------------------------------------------------------

class NERDetector:
    """Optional SpaCy-based NER. Degrades gracefully if spacy not installed."""
    _NER_MAP = {"PER": "PER", "ORG": "ORG", "LOC": "LOC"}

    def __init__(self):
        self._nlp = None
        model_name = getattr(settings, "anonymization_spacy_model", "ru_core_news_md")
        try:
            import spacy
            self._nlp = spacy.load(model_name)
            logger.info(f"SpaCy NER loaded ({model_name})")
        except (ImportError, OSError):
            logger.info("SpaCy not available, NER detection disabled")

    def detect(self, value: str) -> Optional[str]:
        """Detect NER category for whole-string match (>50% coverage)."""
        if not self._nlp or len(value) < 4:
            return None
        doc = self._nlp(value)
        for ent in doc.ents:
            if len(ent.text) / len(value) > 0.5:
                return self._NER_MAP.get(ent.label_)
        return None


# ---------------------------------------------------------------------------
# Whitelist — what to skip
# ---------------------------------------------------------------------------

SKIP_VALUE_KEY_PREFIXES = (
    settings.anonymization_skip_value_key_prefixes or _DEFAULT_SKIP_PREFIXES
)


# ---------------------------------------------------------------------------
# Main detector class
# ---------------------------------------------------------------------------

class ValueDetector:
    """Multi-level sensitive data detector."""

    def __init__(self, radical_mode: bool = False):
        self._ner = NERDetector()
        self._radical_mode = radical_mode

    def should_skip(self, key: str, value: str,
                    key_is_sensitive: bool = False) -> bool:
        """Check if value should be skipped from whole-string detection."""
        # 1. Service key
        if key in SKIP_KEYS:
            return True
        # 2. Key starts with status/type/mode prefix
        k = key.lower()
        if any(k.startswith(prefix) for prefix in SKIP_VALUE_KEY_PREFIXES):
            return True
        # 3. Known safe enum values
        if value in KNOWN_SAFE_VALUES:
            return True
        # 4. Empty string
        if not value.strip():
            return True
        # 5. Very short string (codes, abbreviations ≤3 chars)
        #    BUT: if key is sensitive (ФИО="Лев") — don't skip
        if len(value.strip()) <= 3 and not key_is_sensitive:
            return True
        # 6. ISO date/time
        if re.match(r"^\d{4}-\d{2}-\d{2}", value) and not key_is_sensitive:
            return True
        # 7. UUID
        if re.match(r"^[0-9a-f]{8}-[0-9a-f]{4}-", value, re.I):
            return True
        # 8. Hex GUID (32 chars)
        if re.match(r"^[0-9a-fA-F]{32}$", value):
            return True
        # 9. 1C navigation link
        if value.startswith("e1cib/"):
            return True
        # 10. 1C metadata type name
        if re.match(
            r"^(Справочник|Документ|Регистр|Перечисление|Отчет|Обработка"
            r"|Catalog|Document|Register|Enum|Report|DataProcessor)\.", value
        ):
            return True
        return False

    def detect(
        self,
        key: str,
        value: str,
        allow_ner: bool = True,
        extra_keys: Optional[Sequence[str]] = None,
    ) -> Optional[str]:
        """Return anonymization category or None."""
        if not isinstance(value, str):
            return None

        # Exact-key rules for common document series/number fields.
        # Category is intentionally neutral ("STR") to avoid misleading the agent.
        #
        # IMPORTANT: these checks run before should_skip() so that technical
        # prefixes like "код*" don't suppress anonymization for known-sensitive
        # document identifiers (e.g. "КодПодразделения").
        k = key.strip().lower()
        stripped = value.strip()
        if k == "сериядокумента" and re.fullmatch(r"\d{4}", stripped):
            return "STR"
        if k == "номердокумента" and re.fullmatch(r"\d{6}", stripped):
            return "STR"
        if k == "серия" and re.fullmatch(r"\d{4}", stripped):
            return "STR"
        if k == "номер" and re.fullmatch(r"\d{6}", stripped):
            return "STR"
        if k == "кодподразделения" and re.fullmatch(r"\d{3}-?\d{3}", stripped):
            return "STR"
        if k == "документсерия" and re.fullmatch(r"\d{4}", stripped):
            return "STR"
        if k == "документномер" and re.fullmatch(r"\d{6}", stripped):
            return "STR"
        if k == "документкодподразделения" and re.fullmatch(r"\d{3}-?\d{3}", stripped):
            return "STR"

        # Level 0: key-aware FIRST (before should_skip!)
        # This ensures ФИО="Лев" (3 chars) is not skipped by length check.
        key_candidates = [key]
        if extra_keys:
            key_candidates.extend(extra_keys)
        cat = _detect_by_key_candidates(key_candidates)

        # should_skip: if key is sensitive — disable length-skip
        if self.should_skip(key, value, key_is_sensitive=cat is not None):
            return None
        if cat:
            return cat

        # Level 1: regex patterns (full match)
        for category, pattern in PATTERNS:
            if pattern.match(value.strip()):
                return category
        # Level 2: NER
        if allow_ner:
            ner_result = self._ner.detect(value)
            if ner_result:
                return ner_result
        # Level 2.5: Bank card PAN (Luhn) — whole-string only
        stripped = value.strip()
        if stripped and re.fullmatch(r"[\d\s\-]+", stripped):
            digits = re.sub(r"\D", "", stripped)
            if 13 <= len(digits) <= 19 and _luhn_is_valid(digits):
                return "CARD"
        # Level 3: radical mode fallback
        if self._radical_mode:
            return "STR"
        return None

    def replace_inline_pii(
        self,
        value: str,
        tokenize_fn,
        parent_key: str = "",
        allow_ner: bool = True,
    ) -> str:
        """Replace PII fragments inside a string via SEARCH_PATTERNS + NER."""
        # For SKIP_KEYS we must avoid *any* inline modifications, otherwise
        # technical identifiers can get partially tokenized (hybrid corrupted values).
        # "error" is the only SKIP_KEY where inline anonymization is desired.
        if parent_key in SKIP_KEYS and parent_key != "error":
            return value

        # Общий предикат исключений для поля "error": те же технические слова
        # защищаются во всех inline-путях (regex/NER/card) через один guarded-обёртчик.
        # Проверяется фактически найденный фрагмент; вне "error" обёртка не создаётся.
        if parent_key == "error":
            def _guarded_tokenize(matched_text, category, _fn=tokenize_fn):
                if is_error_inline_anonymization_exclusion(matched_text, parent_key):
                    return matched_text  # вернуть исходный найденный текст
                return _fn(matched_text, category)
            inline_tokenize = _guarded_tokenize
        else:
            inline_tokenize = tokenize_fn

        # JSON-aware replacement for fields that store structured JSON as a string
        # (e.g. контактная информация). This prevents tokenizing JSON keys like
        # "areaCode"/"countryCode" as SWIFT due to case-insensitive SWIFT regex.
        parent_key_lower = (parent_key or "").lower()
        if "значени" in parent_key_lower:
            stripped = (value or "").strip()
            if stripped.startswith(("{", "[")) and stripped.endswith(("}", "]")):
                try:
                    parsed = json.loads(stripped)
                except json.JSONDecodeError:
                    parsed = None

                if isinstance(parsed, (dict, list)):
                    def _walk_json_values(obj):
                        if isinstance(obj, dict):
                            return {k: _walk_json_values(v) for k, v in obj.items()}
                        if isinstance(obj, list):
                            return [_walk_json_values(v) for v in obj]
                        if isinstance(obj, str):
                            # Process only values; keys are never modified.
                            return self.replace_inline_pii(
                                obj,
                                tokenize_fn,
                                parent_key="",
                                allow_ner=allow_ner,
                            )
                        return obj

                    updated = _walk_json_values(parsed)
                    dumped = json.dumps(updated, ensure_ascii=False, indent=4)
                    if "\r\n" in value:
                        dumped = dumped.replace("\n", "\r\n")
                    return dumped

        # 1. Regex search-patterns
        for category, pattern in SEARCH_PATTERNS:
            value = _sub_outside_tokens(
                value,
                pattern,
                lambda m, cat=category: inline_tokenize(m.group(0), cat),
            )
        # 2. NER — catches PER/ORG/LOC in free text (if SpaCy available)
        if allow_ner and self._ner._nlp:
            key = (parent_key or "").lower()
            doc = self._ner._nlp(value)
            for ent in reversed(doc.ents):  # reversed to preserve indices
                cat = self._ner._NER_MAP.get(ent.label_)
                if not cat:
                    continue
                if not ent.text or ent.text.startswith("["):
                    continue

                if _overlaps_any_token(value, ent.start_char, ent.end_char):
                    continue

                if key == "error":
                    # Do not allow NER to "anonymize" 1C query/metadata identifiers
                    # in error messages (e.g. ЮридическийАдрес, Справочник.Организации,
                    # ВнешняяОбработка.MCPToolkit.Форма...).
                    ent_lower = ent.text.lower()
                    if any(s in ent_lower for s in _NER_ERROR_STOPWORD_SUBSTRINGS):
                        continue
                    if _1C_METADATA_PREFIX_RE.match(ent.text):
                        continue
                    if "." in ent.text and " " not in ent.text:
                        continue
                    if _is_dotted_identifier_context(value, ent.start_char, ent.end_char):
                        continue
                    if _looks_like_1c_identifier_fragment(ent.text):
                        continue

                token = inline_tokenize(ent.text, cat)
                value = value[:ent.start_char] + token + value[ent.end_char:]

        # 3. Bank cards (PAN): 13–19 digits with optional spaces/dashes + Luhn check
        def _replace_card(m: re.Match) -> str:
            raw = m.group(0)
            digits = re.sub(r"\D", "", raw)
            if 13 <= len(digits) <= 19 and _luhn_is_valid(digits):
                return inline_tokenize(raw, "CARD")
            return raw

        value = _sub_outside_tokens(value, _CARD_CANDIDATE_RE, _replace_card)
        return value
