"""
Core anonymizer: recursive response anonymization and request de-tokenization.
"""
from typing import Any, Dict, Optional, Tuple

from .token_mapper import TokenMapper
from .value_detector import ValueDetector
from .dictionary_matcher import DictionaryMatcher, DICT_APPLY_KEY_SUBSTRINGS
from ..config import settings
from .anonymization_defaults import (
    COUNTERPARTY_TYPE_MARKERS,
    FORCE_NAME_KEYS,
    PERSON_TYPE_MARKERS,
    ROW_GROUP_ORG_ANCHOR_SUBSTRINGS,
    ROW_GROUP_ORG_EVIDENCE_KEY_SUBSTRINGS,
    ROW_GROUP_ORG_GENERIC_NAME_KEYS,
    TOP_LEVEL_SKIP_KEYS,
    is_error_inline_anonymization_exclusion,
)


class Anonymizer:
    """Anonymizes 1C responses and de-tokenizes agent requests."""

    def __init__(self, radical_mode: bool = False,
                 include_errors: bool = True,
                 tokenmap_max: int = 0):
        self._mapper = TokenMapper(max_size=tokenmap_max)
        self._detector = ValueDetector(radical_mode=radical_mode)
        self._include_errors = include_errors
        self._dict_matcher: Optional[DictionaryMatcher] = None
        # Effective apply-key-substrings: override replaces defaults, add extends
        _override = settings.anonymization_dictionary_apply_key_substrings_override
        _base: Tuple[str, ...] = _override if _override is not None else DICT_APPLY_KEY_SUBSTRINGS
        _add = settings.anonymization_dictionary_apply_key_substrings_add
        self._dict_apply_substrings: Tuple[str, ...] = _base + _add if _add else _base
        self._force_name_keys = FORCE_NAME_KEYS
        # execute_query can return flat rows after JOINs where entity context is
        # lost in column aliases (e.g. "ПолноеНаименование"). Row-grouping tries
        # to re-associate columns and apply safe fallbacks only when unambiguous.
        self._row_group_org_anchor_substrings = ROW_GROUP_ORG_ANCHOR_SUBSTRINGS
        self._row_group_org_evidence_key_substrings = ROW_GROUP_ORG_EVIDENCE_KEY_SUBSTRINGS
        # Only these exact keys (no anchors) are eligible for single-group fallback,
        # to avoid tokenizing unrelated fields like "НаименованиеДокумента".
        self._row_group_org_generic_name_keys = ROW_GROUP_ORG_GENERIC_NAME_KEYS
        self._counterparty_type_markers = COUNTERPARTY_TYPE_MARKERS
        self._person_type_markers = PERSON_TYPE_MARKERS

    def set_dict_matcher(self, matcher: DictionaryMatcher) -> None:
        """Set dictionary matcher (called after preload completes)."""
        self._dict_matcher = matcher

    def _build_key_candidates(self, key: str, extra_keys: Tuple[str, ...] = ()) -> Tuple[str, ...]:
        """Build stable key candidates tuple from primary key and source-based hints."""
        candidates = []
        seen = set()
        for candidate in (key, *extra_keys):
            if not isinstance(candidate, str):
                continue
            normalized = candidate.strip()
            if not normalized or normalized in seen:
                continue
            candidates.append(normalized)
            seen.add(normalized)
        return tuple(candidates)

    def _should_apply_dict(self, key: str, extra_keys: Tuple[str, ...] = ()) -> bool:
        """True if any key candidate contains a dictionary apply-key substring."""
        for candidate in self._build_key_candidates(key, extra_keys):
            lowered = candidate.lower()
            if any(sub in lowered for sub in self._dict_apply_substrings):
                return True
        return False

    def _has_force_name_key(self, key: str, extra_keys: Tuple[str, ...] = ()) -> bool:
        """True if any key candidate is a name-like key eligible for forced-name logic."""
        for candidate in self._build_key_candidates(key, extra_keys):
            if candidate.lower() in self._force_name_keys:
                return True
        return False

    def anonymize_response(
        self,
        result: Dict[str, Any],
        tool_name: str | None = None,
    ) -> Dict[str, Any]:
        """Anonymize the full 1C response, not just result['data'].""" 
        if not isinstance(result, dict):
            return result
        source_last_hints_by_key: Dict[str, Tuple[str, ...]] = {}
        column_policy_by_key: Dict[str, Dict[str, Any]] = {}
        if tool_name == "execute_query":
            schema = result.get("schema")
            if isinstance(schema, dict):
                source_last_hints_by_key = self._build_execute_query_source_last_hints(schema)
                column_policy_by_key = self._build_execute_query_column_policy(schema)
        out = {}
        for k, v in result.items():
            if k in TOP_LEVEL_SKIP_KEYS:
                out[k] = v  # technical fields — unchanged
            elif k == "error" and not self._include_errors:
                out[k] = v  # errors: leave as-is if include_errors=False
            elif (
                tool_name == "execute_query"
                and k == "data"
                and isinstance(v, list)
            ):
                out[k] = self._walk_execute_query_rows(
                    v,
                    source_last_hints_by_key=source_last_hints_by_key,
                    column_policy_by_key=column_policy_by_key,
                )
            elif tool_name == "get_access_rights" and k == "data":
                if isinstance(v, dict) and isinstance(v.get("user"), dict):
                    user = v["user"]
                    saved = {f: user[f] for f in ("name", "full_name") if f in user and isinstance(user[f], str) and user[f]}
                    v = {**v, "user": {fk: fv for fk, fv in user.items() if fk not in saved}}
                    walked = self._walk(v, parent_key=k, allow_ner=self._allow_ner_for_top_level_key(k, tool_name))
                    if saved and isinstance(walked, dict) and isinstance(walked.get("user"), dict):
                        for field, raw in saved.items():
                            walked["user"][field] = self._mapper.tokenize(raw, "PER")
                    out[k] = walked
                else:
                    out[k] = self._walk(v, parent_key=k, allow_ner=self._allow_ner_for_top_level_key(k, tool_name))
            else:
                out[k] = self._walk(
                    v,
                    parent_key=k,
                    allow_ner=self._allow_ner_for_top_level_key(k, tool_name),
                    force_dict_apply=self._force_dict_for_top_level_value(k, v, tool_name),
                    source_last_hints_by_key=(
                        source_last_hints_by_key
                        if tool_name == "execute_query" and k == "data"
                        else None
                    ),
                )
        return out

    @staticmethod
    def _is_enum_column(types: Any) -> bool:
        """True if ALL column types are enum reference types (pure enum column).

        Composite types like ["ПеречислениеСсылка.X", "Строка"] return False so that
        string values in the column are still subject to normal anonymization.
        """
        if not isinstance(types, (list, tuple)) or not types:
            return False
        return all(
            isinstance(t, str) and "ПеречислениеСсылка." in t
            for t in types
        )

    @staticmethod
    def _is_chart_of_accounts_column(types: Any) -> bool:
        """True if ALL column types are chart-of-accounts reference types (pure CoA column).

        Chart-of-accounts objects contain normative/reference data (account codes and names)
        with no personal or sensitive information, so they are kept by default.
        Composite types like ["ПланСчетовСсылка.X", "Строка"] return False.
        """
        if not isinstance(types, (list, tuple)) or not types:
            return False
        return all(
            isinstance(t, str) and "ПланСчетовСсылка." in t
            for t in types
        )

    def _build_execute_query_column_policy(
        self,
        schema: Dict[str, Any],
    ) -> Dict[str, Dict[str, Any]]:
        """Build column_name -> internal precise policy mapping from execute_query schema."""
        columns = schema.get("columns")
        if not isinstance(columns, list):
            return {}

        result: Dict[str, Dict[str, Any]] = {}
        for column in columns:
            if not isinstance(column, dict):
                continue
            name = column.get("name")
            if not isinstance(name, str) or not name:
                continue
            types = column.get("types", [])
            if self._is_enum_column(types):
                result[name] = {
                    "mode": "force_keep",
                    "reference_presentation_mode": "force_keep",
                    "types": types,
                }
                continue
            if self._is_chart_of_accounts_column(types):
                explicit_mode = column.get("anonymization_mode", "default")
                explicit_ref_mode = column.get("reference_presentation_mode", "default")
                if explicit_mode != "force_anonymize" and explicit_ref_mode != "force_anonymize":
                    result[name] = {
                        "mode": "force_keep",
                        "reference_presentation_mode": "force_keep",
                        "types": types,
                    }
                    continue
            result[name] = {
                "mode": column.get("anonymization_mode", "default"),
                "reference_presentation_mode": column.get(
                    "reference_presentation_mode",
                    "default",
                ),
                "types": types,
            }
        return result

    def _walk_execute_query_rows(
        self,
        rows: list[Any],
        *,
        source_last_hints_by_key: Dict[str, Tuple[str, ...]],
        column_policy_by_key: Dict[str, Dict[str, Any]],
    ) -> list[Any]:
        """Apply precise per-column policy to execute_query flat rows."""
        out: list[Any] = []
        for row in rows:
            if not isinstance(row, dict):
                out.append(row)
                continue
            out_row: Dict[str, Any] = {}
            for key, value in row.items():
                policy = column_policy_by_key.get(key) or {}
                mode = policy.get("mode", "default")
                ref_mode = policy.get("reference_presentation_mode", "default")
                extra_keys = source_last_hints_by_key.get(key, ())
                out_row[key] = self._apply_execute_query_value_policy(
                    key,
                    value,
                    mode=mode,
                    reference_presentation_mode=ref_mode,
                    extra_keys=extra_keys,
                )
            out.append(out_row)
        return out

    def _apply_execute_query_value_policy(
        self,
        key: str,
        value: Any,
        *,
        mode: str,
        reference_presentation_mode: str,
        extra_keys: Tuple[str, ...],
    ) -> Any:
        """Apply exact/forced execute_query policy to a single column value."""
        if mode == "force_keep":
            return value

        if isinstance(value, dict) and value.get("_objectRef") is True:
            # Fallback: enum values must never be anonymized regardless of policy
            if str(value.get("ТипОбъекта", "")).startswith("ПеречислениеСсылка."):
                return value

            # Fallback: chart-of-accounts values are kept by default (normative data, no PII)
            if str(value.get("ТипОбъекта", "")).startswith("ПланСчетовСсылка."):
                if mode != "force_anonymize" and reference_presentation_mode != "force_anonymize":
                    return value

            if reference_presentation_mode == "force_keep" and mode != "force_anonymize":
                return value

            updated = dict(value)
            presentation = str(value.get("Представление", ""))

            if mode == "force_anonymize" or reference_presentation_mode == "force_anonymize":
                updated["Представление"] = self._force_anonymize_string(
                    key,
                    presentation,
                    extra_keys=extra_keys,
                )
                return updated

            updated["Представление"] = self._walk(
                presentation,
                parent_key=key,
                source_last_hints_by_key=None,
                extra_keys=extra_keys,
            )
            return updated

        if isinstance(value, str):
            if mode == "force_anonymize":
                return self._force_anonymize_string(key, value, extra_keys=extra_keys)
            return self._walk(
                value,
                parent_key=key,
                source_last_hints_by_key=None,
                extra_keys=extra_keys,
            )

        return value

    def _force_anonymize_string(
        self,
        key: str,
        value: str,
        *,
        extra_keys: Tuple[str, ...] = (),
    ) -> str:
        """Force anonymization by tokenizing the whole string as a single value."""
        if not value:
            return value
        detected = self._detector.detect(
            key,
            value,
            allow_ner=settings.anonymization_ner_enabled,
            extra_keys=extra_keys,
        )
        return self._mapper.tokenize(value, detected or "STR")

    def detokenize_params(self, params: Dict[str, Any]) -> Dict[str, Any]:
        """Replace tokens in agent request params with real values."""
        return self._walk_detokenize(params)

    # Поля с кодом/запросом и соответствующий режим детокенизации
    _CODE_FIELDS: dict = {
        "execute_code":  ("code",  "bsl"),
        "execute_query": ("query", "query"),
    }

    def detokenize_params_for_tool(self, params: Dict[str, Any], tool: str) -> Dict[str, Any]:
        """De-tokenize params with tool-specific quote escaping for code/query fields."""
        entry = self._CODE_FIELDS.get(tool)
        if entry is None:
            return self._walk_detokenize(params)

        code_field, mode = entry
        result = {}
        for k, v in params.items():
            if k == code_field and isinstance(v, str):
                if mode == "bsl":
                    result[k] = self._mapper.detokenize_escape_double(v)
                else:
                    result[k] = self._mapper.detokenize_for_query(v)
            else:
                result[k] = self._walk_detokenize(v)
        return result

    def _extract_source_last(self, source: str) -> str:
        """Return the anonymization hint segment from a source string.

        Usually this is the last path segment. For terminal ``...Ссылка`` the
        useful hint is the previous segment, because ``Ссылка`` itself carries
        almost no anonymization meaning.
        """
        parts = [part.strip() for part in source.split(".") if part.strip()]
        if not parts:
            return ""
        if len(parts) >= 2 and parts[-1].lower() == "ссылка":
            return parts[-2]
        return parts[-1]

    def _build_execute_query_source_last_hints(
        self,
        schema: Dict[str, Any],
    ) -> Dict[str, Tuple[str, ...]]:
        """Build column_name -> tuple(last(source), ...) mapping from execute_query schema."""
        columns = schema.get("columns")
        if not isinstance(columns, list):
            return {}

        result: Dict[str, Tuple[str, ...]] = {}
        for column in columns:
            if not isinstance(column, dict):
                continue
            name = column.get("name")
            if not isinstance(name, str) or not name:
                continue
            sources = column.get("sources")
            if not isinstance(sources, list):
                continue

            leafs: list[str] = []
            seen: set[str] = set()
            for source in sources:
                if not isinstance(source, str) or not source.strip():
                    continue
                leaf = self._extract_source_last(source)
                if not leaf or leaf in seen:
                    continue
                leafs.append(leaf)
                seen.add(leaf)

            if leafs:
                result[name] = tuple(leafs)

        return result

    def _walk(
        self,
        obj: Any,
        parent_key: str = "",
        forced_name_category: str | None = None,
        forced_str_category: str | None = None,
        allow_ner: bool = True,
        force_dict_apply: bool = False,
        source_last_hints_by_key: Optional[Dict[str, Tuple[str, ...]]] = None,
        extra_keys: Tuple[str, ...] = (),
    ) -> Any:
        """Recursive walk: anonymize sensitive string values."""
        if isinstance(obj, dict):
            local_forced = forced_name_category or self._forced_name_category(obj)
            row_forced = (
                self._row_grouping_forced_str_categories(obj)
                if parent_key == "data"
                else {}
            )
            out: Dict[str, Any] = {}
            for k, v in obj.items():
                per_key_forced_str = row_forced.get(k) if isinstance(v, str) else None
                current_extra_keys = (
                    source_last_hints_by_key.get(k, ())
                    if parent_key == "data" and source_last_hints_by_key and isinstance(v, str)
                    else ()
                )
                out[k] = self._walk(
                    v,
                    parent_key=k,
                    forced_name_category=local_forced,
                    forced_str_category=per_key_forced_str,
                    allow_ner=allow_ner,
                    force_dict_apply=force_dict_apply,
                    source_last_hints_by_key=source_last_hints_by_key,
                    extra_keys=current_extra_keys,
                )
            return out
        if isinstance(obj, list):
            return [
                self._walk(
                    item,
                    parent_key=parent_key,
                    forced_name_category=forced_name_category,
                    forced_str_category=None,
                    allow_ner=allow_ner,
                    force_dict_apply=force_dict_apply,
                    source_last_hints_by_key=source_last_hints_by_key,
                    extra_keys=(),
                )
                for item in obj
            ]
        if isinstance(obj, str):
            # 1. Dictionary replacement (before all other detection)
            # replace() works outside existing [TOKEN-NNNN] spans
            # passes canonical_term(category) to tokenize_fn for stable tokens
            dict_changed = False
            if self._dict_matcher and (force_dict_apply or self._should_apply_dict(parent_key, extra_keys)):
                # Same shared exclusion predicate as inline paths: only for the
                # "error" field, and only over the actually-matched fragment.
                exclude_fragment = (
                    (lambda frag: is_error_inline_anonymization_exclusion(frag, "error"))
                    if parent_key == "error" else None
                )
                new_obj = self._dict_matcher.replace(
                    obj, self._mapper.tokenize, exclude_fragment=exclude_fragment
                )
                if new_obj != obj:
                    dict_changed = True
                    obj = new_obj

            # 2. Whole-string detection
            cat = self._detector.detect(
                parent_key,
                obj,
                allow_ner=allow_ner,
                extra_keys=extra_keys,
            )
            if cat:
                if dict_changed:
                    # Part already tokenized by dict — only inline on remainder
                    return self._detector.replace_inline_pii(
                        obj,
                        self._mapper.tokenize,
                        parent_key=parent_key,
                        allow_ner=allow_ner,
                    )
                return self._mapper.tokenize(obj, cat)

            # 3. Forced name category
            if (
                forced_name_category
                and self._has_force_name_key(parent_key, extra_keys)
            ):
                if dict_changed:
                    return self._detector.replace_inline_pii(
                        obj,
                        self._mapper.tokenize,
                        parent_key=parent_key,
                        allow_ner=allow_ner,
                    )
                return self._mapper.tokenize(obj, forced_name_category)

            # 4. Forced str category (row grouping)
            if forced_str_category:
                if dict_changed:
                    return self._detector.replace_inline_pii(
                        obj,
                        self._mapper.tokenize,
                        parent_key=parent_key,
                        allow_ner=allow_ner,
                    )
                return self._mapper.tokenize(obj, forced_str_category)

            # 5. Inline PII (regex + NER + card)
            return self._detector.replace_inline_pii(
                obj,
                self._mapper.tokenize,
                parent_key=parent_key,
                allow_ner=allow_ner,
            )
        return obj  # numbers, booleans, null — unchanged

    def _allow_ner_for_top_level_key(self, key: str, tool_name: str | None) -> bool:
        """Return whether NER is allowed for this top-level response subtree."""
        if not settings.anonymization_ner_enabled:
            return False
        if key == "error":
            return settings.anonymization_ner_for_error
        if tool_name == "execute_code" and key == "data":
            return settings.anonymization_ner_for_execute_code_result
        return True

    def _force_dict_for_top_level_value(
        self,
        key: str,
        value: Any,
        tool_name: str | None,
    ) -> bool:
        """Force dictionary matching for the whole execute_code data subtree."""
        return tool_name == "execute_code" and key == "data"

    def _row_grouping_forced_str_categories(self, row: Dict[str, Any]) -> Dict[str, str]:
        """Return per-key forced category mapping for flat execute_query rows.

        Applies only for top-level dicts inside result['data'] list (parent_key == "data").
        """
        keys = list(row.keys())
        if not keys:
            return {}

        def _find_org_anchor(k_lower: str) -> str | None:
            for anchor in self._row_group_org_anchor_substrings:
                if anchor in k_lower:
                    return anchor
            return None

        def _is_org_evidence_key(k_lower: str) -> bool:
            return any(s in k_lower for s in self._row_group_org_evidence_key_substrings)

        def _is_name_like_key(k_lower: str) -> bool:
            return ("наименован" in k_lower) or ("представлен" in k_lower)

        # Group keys by ORG anchor substring.
        keys_by_anchor: Dict[str, list[str]] = {}
        anchor_by_key: Dict[str, str | None] = {}
        generic_evidence_present = False
        for k in keys:
            kl = k.lower()
            anchor = _find_org_anchor(kl)
            anchor_by_key[k] = anchor
            if anchor:
                keys_by_anchor.setdefault(anchor, []).append(k)
            else:
                if _is_org_evidence_key(kl):
                    generic_evidence_present = True

        if not keys_by_anchor:
            return {}

        # Evidence: group contains INN/KPP/OGRN column(s) (anchored).
        evidence_by_anchor: Dict[str, bool] = {}
        for anchor, group_keys in keys_by_anchor.items():
            evidence_by_anchor[anchor] = any(_is_org_evidence_key(gk.lower()) for gk in group_keys)

        # If we have exactly one ORG group, allow generic INN/KPP/OGRN columns
        # to serve as evidence for that group (common agent alias pattern).
        anchors = list(keys_by_anchor.keys())
        if len(anchors) == 1 and generic_evidence_present and not evidence_by_anchor.get(anchors[0], False):
            evidence_by_anchor[anchors[0]] = True

        proven_anchors = [a for a, ok in evidence_by_anchor.items() if ok]
        if not proven_anchors:
            return {}

        forced: Dict[str, str] = {}
        # Force ORG for name-like keys inside proven ORG groups.
        for anchor in proven_anchors:
            for k in keys_by_anchor.get(anchor, []):
                if _is_name_like_key(k.lower()):
                    forced[k] = "ORG"

        # Single-group fallback: force ORG for generic name keys without anchors.
        if len(proven_anchors) == 1:
            for k in keys:
                if anchor_by_key.get(k):
                    continue
                kl = k.lower()
                if kl in self._row_group_org_generic_name_keys:
                    forced[k] = "ORG"

        return forced

    def _forced_name_category(self, obj: Dict[str, Any]) -> str | None:
        """Detect context category for name-like keys based on object structure."""
        # Reference object: { ..., "ТипОбъекта": "...", "Представление": "..." }
        t = obj.get("ТипОбъекта")
        if isinstance(t, str):
            tl = t.lower()
            if any(marker in tl for marker in self._counterparty_type_markers):
                return "ORG"
            if any(marker in tl for marker in self._person_type_markers):
                return "PER"

        # Row object: { ..., "Ссылка": { "ТипОбъекта": "...", ... }, ... }
        ref = obj.get("Ссылка")
        if isinstance(ref, dict):
            return self._forced_name_category(ref)

        return None


    def _walk_detokenize(self, obj: Any) -> Any:
        """Recursive walk: replace tokens with real values."""
        if isinstance(obj, dict):
            return {k: self._walk_detokenize(v) for k, v in obj.items()}
        if isinstance(obj, list):
            return [self._walk_detokenize(item) for item in obj]
        if isinstance(obj, str) and self._mapper.has_tokens(obj):
            return self._mapper.detokenize(obj)
        return obj

    def prune(self) -> None:
        """Evict token map down to its soft cap. Call once per request boundary,
        after incoming params have been detokenized (so used tokens are touched
        and survive). See TokenMapper.prune for the overflow contract.
        """
        self._mapper.prune()

    @property
    def mapper(self) -> TokenMapper:
        return self._mapper
