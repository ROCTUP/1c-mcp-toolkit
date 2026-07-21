"""
Configuration module for 1C MCP Toolkit Proxy.

Loads settings from environment variables with sensible defaults.
Validates: Requirements 5.1, 5.2
"""

import json
import logging
import os
from typing import Dict, List, Literal, Optional, Tuple

from .anonymizer.anonymization_defaults import (
    DEFAULT_ANONYMIZATION_TOOLS,
)

logger = logging.getLogger(__name__)

# Type alias for response format (Requirement 1.1)
ResponseFormat = Literal["json", "toon"]


class Settings:
    """Configuration settings loaded from environment variables."""

    def __init__(self):
        # HTTP port (Requirement 5.2)
        self.port: int = int(os.getenv("PORT", "6003"))
        # Timeout for waiting 1C response (Requirement 5.5)
        self.timeout: float = float(os.getenv("TIMEOUT", "180"))
        # Default long-poll timeout for /1c/poll (do not change to keep 1C UI responsive)
        self.poll_timeout: float = float(os.getenv("POLL_TIMEOUT", "0"))
        # Logging level
        self.log_level: str = os.getenv("LOG_LEVEL", "INFO")
        # Debug mode for development (enables auto-reload)
        self.debug: bool = os.getenv("DEBUG", "false").lower() in ("true", "1", "yes")
        # Dangerous keywords for execute_code blacklist
        self.dangerous_keywords: List[str] = self._parse_dangerous_keywords()
        # Allow dangerous operations with user approval (default: false - block dangerous operations)
        self.allow_dangerous_with_approval: bool = os.getenv(
            "ALLOW_DANGEROUS_WITH_APPROVAL", "false"
        ).lower() in ("true", "1", "yes")
        # Response format setting (Requirement 1.1, 1.2, 1.4)
        self.response_format: ResponseFormat = self._parse_response_format()
        # Enable automatic encoding detection for non-UTF-8 request bodies
        # Helps Windows clients sending CP1251/CP866 encoded JSON (default: true)
        self.enable_encoding_auto_detection: bool = os.getenv(
            "ENABLE_ENCODING_AUTO_DETECTION", "true"
        ).lower() in ("true", "1", "yes")

        # --- Anonymization ---
        self.anonymization_enabled: bool = os.getenv(
            "ANONYMIZATION_ENABLED", "false"
        ).lower() in ("true", "1", "yes")

        # Whitelist of tools to anonymize (comma-separated)
        self.anonymization_tools: List[str] = self._parse_csv(
            "ANONYMIZATION_TOOLS", ",".join(DEFAULT_ANONYMIZATION_TOOLS)
        )
        # Tool names are treated as case-insensitive in config.
        self.anonymization_tools = [t.lower() for t in self.anonymization_tools]

        # Radical mode: tokenize all strings (not only detected sensitive)
        self.anonymization_radical_mode: bool = os.getenv(
            "ANONYMIZATION_RADICAL_MODE", "false"
        ).lower() in ("true", "1", "yes")

        # Anonymize error messages too (1C errors often contain parameter values)
        self.anonymization_include_errors: bool = os.getenv(
            "ANONYMIZATION_INCLUDE_ERRORS", "true"
        ).lower() in ("true", "1", "yes")

        # Global NER toggle — set to true to enable SpaCy NER everywhere.
        self.anonymization_ner_enabled: bool = os.getenv(
            "ANONYMIZATION_NER_ENABLED", "false"
        ).lower() in ("true", "1", "yes")

        # Allow SpaCy NER inside execute_code result payloads (data subtree).
        self.anonymization_ner_for_execute_code_result: bool = os.getenv(
            "ANONYMIZATION_NER_FOR_EXECUTE_CODE_RESULT",
            "false",
        ).lower() in ("true", "1", "yes")

        # Allow SpaCy NER for top-level error messages of any tool.
        self.anonymization_ner_for_error: bool = os.getenv(
            "ANONYMIZATION_NER_FOR_ERROR",
            "false",
        ).lower() in ("true", "1", "yes")

        # SpaCy NER model name (used if SpaCy is installed in the runtime image).
        # Default: md model (sm is not bundled in the image).
        self.anonymization_spacy_model: str = (
            os.getenv("ANONYMIZATION_SPACY_MODEL", "ru_core_news_md").strip().lower()
            or "ru_core_news_md"
        )

        # Overridable key lists — if set, fully replace defaults
        self.anonymization_skip_value_key_prefixes: Optional[Tuple[str, ...]] = \
            self._parse_csv_tuple("ANONYMIZATION_SKIP_VALUE_KEY_PREFIXES")
        self.anonymization_sensitive_keys: Optional[Dict[str, str]] = \
            self._parse_json_dict("ANONYMIZATION_SENSITIVE_KEY_SUBSTRINGS")
        # Additional sensitive key substrings (JSON dict) — merges into defaults/override.
        self.anonymization_sensitive_keys_add: Optional[Dict[str, str]] = \
            self._parse_json_dict("ANONYMIZATION_SENSITIVE_KEY_SUBSTRINGS_ADD")

        # Exact-match sensitive keys (JSON dict). If set, replaces defaults; *_ADD extends.
        self.anonymization_sensitive_key_exact: Optional[Dict[str, str]] = \
            self._parse_json_dict("ANONYMIZATION_SENSITIVE_KEY_EXACT")
        self.anonymization_sensitive_key_exact_add: Optional[Dict[str, str]] = \
            self._parse_json_dict("ANONYMIZATION_SENSITIVE_KEY_EXACT_ADD")

        self.anonymization_non_sensitive_keys: Optional[frozenset] = \
            self._parse_csv_frozenset("ANONYMIZATION_NON_SENSITIVE_KEY_SUBSTRINGS")
        self.anonymization_non_sensitive_keys_add: Optional[frozenset] = \
            self._parse_csv_frozenset("ANONYMIZATION_NON_SENSITIVE_KEY_SUBSTRINGS_ADD")

        # --- Dictionary-based anonymization ---
        self.anonymization_dictionary_enabled: bool = os.getenv(
            "ANONYMIZATION_DICTIONARY_ENABLED", "true"
        ).lower() in ("true", "1", "yes")

        self.anonymization_dictionary_sources_override: Optional[List[Dict]] = \
            self._parse_json_list("ANONYMIZATION_DICTIONARY_SOURCES_OVERRIDE")
        self.anonymization_dictionary_sources_add: Optional[List[Dict]] = \
            self._parse_json_list("ANONYMIZATION_DICTIONARY_SOURCES_ADD")

        # Overridable apply-key-substrings for dictionary matcher
        self.anonymization_dictionary_apply_key_substrings_override: Optional[Tuple[str, ...]] = \
            self._parse_csv_tuple("ANONYMIZATION_DICTIONARY_APPLY_KEY_SUBSTRINGS_OVERRIDE")
        self.anonymization_dictionary_apply_key_substrings_add: Optional[Tuple[str, ...]] = \
            self._parse_csv_tuple("ANONYMIZATION_DICTIONARY_APPLY_KEY_SUBSTRINGS_ADD")

        # Soft cap on the token map size (per channel). Eviction happens at the
        # request boundary (see TokenMapper.prune). 0 = unlimited (opt-out).
        self.anonymization_tokenmap_max: int = self._parse_int(
            "ANONYMIZATION_TOKENMAP_MAX", 10000
        )

    @staticmethod
    def _parse_int(env_name: str, default: int) -> int:
        """Parse a non-negative int from env, falling back to default on error."""
        raw = os.getenv(env_name)
        if raw is None or not raw.strip():
            return default
        try:
            value = int(raw.strip())
        except ValueError:
            logger.warning(
                "%s must be an integer, got %r. Using default %d.",
                env_name, raw, default,
            )
            return default
        if value < 0:
            logger.warning(
                "%s must be non-negative, got %d. Using default %d.",
                env_name, value, default,
            )
            return default
        return value

    def _parse_dangerous_keywords(self) -> List[str]:
        """Parse DANGEROUS_KEYWORDS from environment variable."""
        # Default dangerous keywords for execute_code blacklist
        default_keywords = [
            "Удалить", "Delete",
            "Записать", "Write",
            "УстановитьПривилегированныйРежим", "SetPrivilegedMode",
            "ПодключитьВнешнююКомпоненту", "AttachAddIn",
            "УстановитьВнешнююКомпоненту", "InstallAddIn",
            "COMОбъект", "COMObject",
            "УстановитьМонопольныйРежим", "SetExclusiveMode",
            # "НачатьТранзакцию", "BeginTransaction",
            # "ЗафиксироватьТранзакцию", "CommitTransaction",
            "УдалитьФайлы", "DeleteFiles",
            "КопироватьФайл", "CopyFile",
            "ПереместитьФайл", "MoveFile",
            "СоздатьКаталог", "CreateDirectory",
        ]

        env_value = os.getenv("DANGEROUS_KEYWORDS")
        if env_value is None:
            return default_keywords

        parsed_keywords = [kw.strip() for kw in env_value.split(",") if kw.strip()]
        if parsed_keywords:
            return parsed_keywords

        logger.warning(
            "DANGEROUS_KEYWORDS was provided but no valid keywords were parsed. "
            "Using default dangerous keywords."
        )
        return default_keywords

    def _parse_response_format(self) -> ResponseFormat:
        """Parse RESPONSE_FORMAT from environment variable.

        Returns:
            ResponseFormat: "json" or "toon" based on environment variable.
            Defaults to "toon" if not set or invalid (Requirements 1.2, 1.3).
        """
        env_value = os.getenv("RESPONSE_FORMAT", "toon").lower().strip()

        if env_value in ("json", "toon"):
            return env_value  # type: ignore[return-value]

        # Log warning and fallback to json (Requirement 1.3)
        logger.warning(
            f"Invalid RESPONSE_FORMAT value: '{env_value}'. "
            "Valid values are 'json' or 'toon'. Using 'json' as fallback."
        )
        return "json"

    @staticmethod
    def _parse_csv(env_name: str, default: str) -> List[str]:
        """Parse comma-separated list from env var."""
        return [
            t.strip()
            for t in os.getenv(env_name, default).split(",")
            if t.strip()
        ]

    @staticmethod
    def _parse_csv_tuple(env_name: str) -> Optional[Tuple[str, ...]]:
        """Parse CSV into tuple of lowercase strings, or None for defaults."""
        val = os.getenv(env_name)
        if val is None:
            return None
        return tuple(t.strip().lower() for t in val.split(",") if t.strip())

    @staticmethod
    def _parse_csv_frozenset(env_name: str) -> Optional[frozenset]:
        """Parse CSV into frozenset of lowercase strings, or None for defaults."""
        val = os.getenv(env_name)
        if val is None:
            return None
        return frozenset(t.strip().lower() for t in val.split(",") if t.strip())

    @staticmethod
    def _parse_json_list(env_name: str) -> Optional[List[Dict]]:
        """Parse JSON array from env var. Returns None on error (use defaults)."""
        val = os.getenv(env_name)
        if val is None:
            return None
        try:
            parsed = json.loads(val)
            if not isinstance(parsed, list):
                logger.warning(f"{env_name} must be a JSON array, ignoring")
                return None
            result = [item for item in parsed if isinstance(item, dict)]
            if len(result) != len(parsed):
                logger.warning(f"{env_name}: some elements are not dicts, skipped")
            return result or None
        except json.JSONDecodeError as e:
            logger.warning(f"Invalid JSON in {env_name}: {e}")
            return None

    @staticmethod
    def _parse_json_dict(env_name: str) -> Optional[Dict[str, str]]:
        """Parse JSON dict from env var with graceful fallback.

        Keys are normalized to lowercase (for case-insensitive matching).
        """
        val = os.getenv(env_name)
        if val is None:
            return None
        try:
            parsed = json.loads(val)
            # Keys are normalized to lowercase; values (categories) to uppercase.
            # This keeps token categories stable (e.g. "inn" -> "INN").
            return {str(k).lower(): str(v).upper() for k, v in parsed.items()}
        except (json.JSONDecodeError, AttributeError) as e:
            logger.warning(
                f"Invalid JSON in {env_name}, using defaults: {e}"
            )
            return None


# Global settings instance
settings = Settings()
