"""
Response Formatter module for 1C MCP Toolkit Proxy.

Provides formatting of tool responses in JSON or TOON format.
TOON (Token-Oriented Object Notation) is a compact serialization format
optimized for LLM contexts, providing 30-60% token savings compared to JSON.

Validates: Requirements 2.1, 2.2, 2.3, 2.4, 3.1, 3.3
"""

import json
import logging
import re
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)
_UNQUOTED_KEY_RE = re.compile(r"^[A-Z_][\w.]*$", re.IGNORECASE)

# Try to import toon-format library (Requirement 2.4)
_toon_available = False
try:
    from toon_format import encode as toon_encode
    _toon_available = True
except ImportError:
    # Log warning at module load time (Requirement 3.3)
    logger.warning(
        "toon-format library not installed. "
        "TOON format will not be available, falling back to JSON."
    )


def _is_json_primitive(value: Any) -> bool:
    """Check whether value is a JSON primitive."""
    return value is None or isinstance(value, (str, int, float, bool))


def _encode_key_for_toon(key: str) -> str:
    """Encode key similarly to TOON rules (quote only when needed)."""
    if _UNQUOTED_KEY_RE.match(key):
        return key
    return json.dumps(key, ensure_ascii=False)


def _encode_primitive_for_toon(value: Any) -> str:
    """Encode primitive value in TOON-compatible form."""
    if _toon_available:
        try:
            return toon_encode(value).strip()
        except Exception:
            pass

    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, (int, float)):
        return str(value)
    return json.dumps(value, ensure_ascii=False, default=str)


def _encode_inline_nested_value(value: Any) -> str:
    """Encode nested value inline for custom tabular rendering."""
    if _is_json_primitive(value):
        return _encode_primitive_for_toon(value)

    if isinstance(value, dict):
        parts: List[str] = []
        for key, nested_value in value.items():
            encoded_key = _encode_key_for_toon(str(key))
            encoded_value = _encode_inline_nested_value(nested_value)
            parts.append(f"{encoded_key}: {encoded_value}")
        return "{" + ", ".join(parts) + "}"

    if isinstance(value, list):
        items = [_encode_inline_nested_value(item) for item in value]
        return "[" + ", ".join(items) + "]"

    # Fallback for non-JSON values
    return json.dumps(value, ensure_ascii=False, default=str)


def _detect_nested_tabular_fields(data: Any) -> Optional[List[str]]:
    """Detect array-of-objects with uniform keys and nested values."""
    if not isinstance(data, list) or not data:
        return None

    first_item = data[0]
    if not isinstance(first_item, dict):
        return None

    fields = list(first_item.keys())
    fields_set = set(fields)
    has_nested_values = False

    for row in data:
        if not isinstance(row, dict):
            return None
        if set(row.keys()) != fields_set:
            return None
        if any(not _is_json_primitive(row[field]) for field in fields):
            has_nested_values = True

    return fields if has_nested_values else None


def _encode_nested_tabular_toon(data: List[Dict[str, Any]], fields: List[str]) -> str:
    """Render nested tabular output with one header line."""
    header_fields = ",".join(_encode_key_for_toon(field) for field in fields)
    lines = [f"[{len(data)}]{{{header_fields}}}:"]

    for row in data:
        encoded_values = [_encode_inline_nested_value(row[field]) for field in fields]
        lines.append(f"  {','.join(encoded_values)}")

    return "\n".join(lines)


def is_toon_available() -> bool:
    """Check if TOON format is available.
    
    Returns:
        bool: True if toon-format library is installed and available.
    """
    return _toon_available


def format_response(data: Any, format_type: str) -> str:
    """
    Format response data to the specified format.
    
    Args:
        data: The data to format (dict, list, or primitive)
        format_type: "json" or "toon"
        
    Returns:
        Formatted string representation of the data
        
    Validates: Requirements 2.1, 2.2, 2.3, 2.4, 3.1, 3.3
    """
    if format_type == "toon" and _toon_available:
        try:
            nested_fields = _detect_nested_tabular_fields(data)
            if nested_fields is not None:
                return _encode_nested_tabular_toon(data, nested_fields)
            return toon_encode(data)
        except Exception as e:
            # Fallback to JSON on error (Requirement 3.1)
            # Log warning with fallback reason (Requirement 3.3)
            logger.error(
                f"TOON encoding failed: {e}. Falling back to JSON."
            )
    
    # Default: JSON format (Requirement 2.2)
    # Use ensure_ascii=False to support Cyrillic characters
    return json.dumps(data, ensure_ascii=False, default=str)


def format_tool_result(result: Dict[str, Any], format_type: str) -> Dict[str, Any]:
    """
    Format tool result data field based on configuration.
    
    Args:
        result: Tool result dictionary with 'success', 'data', 'error' fields
        format_type: "json" or "toon"
        
    Returns:
        Result dictionary with formatted 'data' field
        
    Validates: Requirements 2.1, 2.2, 2.3
    """
    # Don't format error responses - pass through unchanged
    if not result.get("success", False):
        return result
    
    # If no data field, nothing to format
    if "data" not in result:
        return result
    
    # Format the data field
    formatted_data = format_response(result["data"], format_type)
    
    # For TOON format, replace data with formatted string
    # For JSON format, keep original data structure (Requirement 2.2)
    return {
        **result,
        "data": formatted_data if format_type == "toon" else result["data"]
    }
