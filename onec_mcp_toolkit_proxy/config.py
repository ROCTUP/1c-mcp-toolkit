"""
Configuration module for 1C MCP Toolkit Proxy.

Loads settings from environment variables with sensible defaults.
Validates: Requirements 5.1, 5.2
"""

import logging
import os
from typing import List, Literal

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


# Global settings instance
settings = Settings()
