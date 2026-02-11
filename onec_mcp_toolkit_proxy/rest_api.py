"""
HTTP REST API handlers for 1C MCP Toolkit Proxy.

This module provides HTTP REST API endpoints as an alternative to the MCP protocol.
All endpoints are under the /api/ prefix and reuse the existing logic from mcp_handler.py.

Validates: Requirements 7.1, 7.2, 7.4, 8.1, 8.2, 8.5
"""

import json
import logging
from typing import Any, Dict, List, Optional, Tuple

from starlette.requests import Request
from starlette.responses import JSONResponse
from pydantic import ValidationError

from .mcp_handler import _execute_1c_command, find_dangerous_keywords
from .config import settings
from .tools import (
    ExecuteQueryParams,
    validate_execute_query_params,
    validate_execute_code_params,
    validate_get_metadata_params,
    validate_get_event_log_params,
    validate_get_object_by_link_params,
    validate_get_link_of_object_params,
    validate_find_references_to_object_params,
    validate_get_access_rights_params
)
from .channel_registry import ChannelRegistry, DEFAULT_CHANNEL

logger = logging.getLogger(__name__)


def _get_channel(request: Request) -> str:
    """
    Get channel from request.
    
    Source of truth: ChannelMiddleware puts normalized channel in scope.
    If middleware didn't run, validate manually via ChannelRegistry.validate_channel_id().
    
    Validates: Requirements 7.1, 7.2, 7.4
    - 7.1: Channel from query parameter ?channel=xxx is used for routing
    - 7.2: Default channel "default" is used when not specified
    - 7.4: Invalid channel is normalized to "default"
    
    Args:
        request: Starlette Request object
        
    Returns:
        Validated channel ID string
    """
    # Prefer scope["channel"] from ChannelMiddleware
    if "channel" in request.scope:
        return request.scope["channel"]
    
    # Fallback: validate manually
    raw_channel = request.query_params.get("channel", DEFAULT_CHANNEL)
    return ChannelRegistry.validate_channel_id(raw_channel)


def _check_content_type(request: Request) -> Optional[JSONResponse]:
    """
    Check Content-Type for POST requests.
    
    Case-insensitive check, allows parameters like charset.
    
    Validates: Requirement 8.5
    - 8.5: POST request with Content-Type not starting with application/json returns HTTP 415
    
    Args:
        request: Starlette Request object
        
    Returns:
        JSONResponse with 415 error if Content-Type is invalid, None otherwise
    """
    if request.method == "POST":
        content_type = request.headers.get("content-type", "").lower()
        if not content_type.startswith("application/json"):
            return JSONResponse(
                status_code=415,
                content={
                    "success": False,
                    "error": "Content-Type должен быть application/json / "
                             "Content-Type must be application/json"
                }
            )
    return None


async def _parse_json_body(request: Request) -> Tuple[Optional[Dict[str, Any]], Optional[JSONResponse]]:
    """
    Parse JSON body from request with error handling.
    
    Validates: Requirement 8.1
    - 8.1: Invalid JSON body returns HTTP 400 with parse error message
    
    Args:
        request: Starlette Request object
        
    Returns:
        Tuple of (parsed_body, error_response):
        - If successful: (dict, None)
        - If error: (None, JSONResponse with 400 status)
    """
    try:
        body = await request.json()
        if not isinstance(body, dict):
            return None, JSONResponse(
                status_code=400,
                content={
                    "success": False,
                    "error": "Тело запроса должно быть JSON объектом / "
                             "Request body must be a JSON object"
                }
            )
        return body, None
    except json.JSONDecodeError as e:
        return None, JSONResponse(
            status_code=400,
            content={
                "success": False,
                "error": f"Ошибка разбора JSON: {e.msg} в позиции {e.pos} / "
                         f"JSON parse error: {e.msg} at position {e.pos}"
            }
        )
    except Exception as e:
        # Handle other parsing errors (e.g., empty body)
        return None, JSONResponse(
            status_code=400,
            content={
                "success": False,
                "error": f"Ошибка разбора JSON: {str(e)} / "
                         f"JSON parse error: {str(e)}"
            }
        )


def _extract_charset_from_content_type(content_type: str) -> Optional[str]:
    """
    Extract charset parameter from Content-Type header.

    Examples:
      "application/json; charset=utf-8" -> "utf-8"
      "application/json; charset=windows-1251" -> "windows-1251"
      "application/json" -> None

    Args:
        content_type: Content-Type header value

    Returns:
        Charset value or None if not specified
    """
    import re
    # IMPORTANT: Don't check "charset=" beforehand, as there may be spaces (charset = ...)
    # Use regex directly to handle spaces
    match = re.search(r'charset\s*=\s*([^;\s]+)', content_type, re.IGNORECASE)
    if match:
        charset = match.group(1).strip().strip('"').strip("'")
        return charset
    return None


def _encoding_quality_score(obj) -> int:
    """
    Score how well decoded JSON text looks like valid Russian.

    Higher score = more likely correct encoding. Used to pick the best
    encoding from the fallback list when charset-normalizer is unavailable
    or gives low confidence (common for short JSON payloads).

    Scoring:
      +2  Standard Russian Cyrillic (А-Яа-яЁё)
      -15 Box drawing / block elements / geometric shapes (strong cp866-artifact)
      -5  Math symbols ∙ √ (cp866 upper-byte artifacts)
      -3  Non-Russian Cyrillic (Ukrainian Є,І,Ї,Ґ / Belarusian Ў / Serbian Ђ,Ј,Љ…)
      -3  Typographic quotes/daggers/€/‰/™ (cp866→cp1251 misread artifacts)
    """
    score = 0

    def _scan(value):
        nonlocal score
        if isinstance(value, str):
            for ch in value:
                cp = ord(ch)
                # Standard Russian Cyrillic: А-Яа-я (U+0410-U+044F), Ё (U+0401), ё (U+0451)
                if (0x0410 <= cp <= 0x044F) or cp == 0x0401 or cp == 0x0451:
                    score += 2
                # Box drawing + block elements + geometric shapes (U+2500-U+25FF)
                elif 0x2500 <= cp <= 0x25FF:
                    score -= 15
                # Math symbols from cp866 upper bytes: ∙ (U+2219), √ (U+221A)
                elif cp in (0x2219, 0x221A):
                    score -= 5
                # Non-Russian Cyrillic (U+0400-U+045F minus standard Russian)
                elif (0x0400 <= cp <= 0x045F) and cp not in (0x0401, 0x0451) \
                        and not (0x0410 <= cp <= 0x044F):
                    score -= 3
                # Typographic symbols common in cp866→cp1251 misreads
                elif cp in (0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2026,
                            0x2020, 0x2021, 0x20AC, 0x2030, 0x2122):
                    score -= 3
        elif isinstance(value, dict):
            for v in value.values():
                _scan(v)
        elif isinstance(value, list):
            for item in value:
                _scan(item)

    _scan(obj)
    return score


async def _parse_json_body_with_encoding_detection(
    request: Request
) -> Tuple[Optional[Dict[str, Any]], Optional[JSONResponse]]:
    """
    Parse JSON body with automatic encoding detection.

    Handles Windows clients sending CP1251/CP866 encoded JSON.

    Algorithm:
    1. Extract charset from Content-Type header if present
    2. Try UTF-8 (fast path for 95%+ requests)
    3. Use charset-normalizer auto-detection (if available)
    4. Fallback to known encodings (CP1251, CP866, UTF-16)
    5. Return clear error if all methods fail

    Args:
        request: Starlette Request object

    Returns:
        Tuple of (parsed_body, error_response)
    """
    # 0. Check if feature is enabled
    if not settings.enable_encoding_auto_detection:
        return await _parse_json_body(request)

    # 1. Extract charset from Content-Type header
    content_type = request.headers.get("content-type", "")
    explicit_charset = _extract_charset_from_content_type(content_type)

    if explicit_charset and explicit_charset.lower() not in ("utf-8", "utf8"):
        # Try explicit charset first
        try:
            raw_body = await request.body()
            decoded = raw_body.decode(explicit_charset)
            body = json.loads(decoded)
            # IMPORTANT: Check type in ALL successful branches (as in _parse_json_body)
            if not isinstance(body, dict):
                return None, JSONResponse(
                    status_code=400,
                    content={
                        "success": False,
                        "error": "Тело запроса должно быть JSON объектом / "
                                 "Request body must be a JSON object"
                    }
                )
            logger.info(f"Decoded using explicit charset: {explicit_charset}")
            return body, None
        except (UnicodeDecodeError, LookupError):
            logger.warning(f"Explicit charset {explicit_charset} failed, trying alternatives")
        except json.JSONDecodeError as e:
            return None, JSONResponse(
                status_code=400,
                content={
                    "success": False,
                    "error": f"Ошибка разбора JSON: {e.msg} в позиции {e.pos} / "
                             f"JSON parse error: {e.msg} at position {e.pos}"
                }
            )

    # 2. Try UTF-8 fast path
    try:
        body = await request.json()
        if not isinstance(body, dict):
            return None, JSONResponse(
                status_code=400,
                content={
                    "success": False,
                    "error": "Тело запроса должно быть JSON объектом / "
                             "Request body must be a JSON object"
                }
            )
        return body, None
    except UnicodeDecodeError:
        pass  # Continue to auto-detection
    except json.JSONDecodeError as e:
        return None, JSONResponse(
            status_code=400,
            content={
                "success": False,
                "error": f"Ошибка разбора JSON: {e.msg} в позиции {e.pos} / "
                         f"JSON parse error: {e.msg} at position {e.pos}"
            }
        )
    except Exception as e:
        # IMPORTANT: Catch all other errors (empty body, etc.) for backward compatibility
        return None, JSONResponse(
            status_code=400,
            content={
                "success": False,
                "error": f"Ошибка разбора JSON: {str(e)} / "
                         f"JSON parse error: {str(e)}"
            }
        )

    # 3. Auto-detect with charset-normalizer (OPTIONAL)
    HAS_CHARSET_NORMALIZER = False
    try:
        from charset_normalizer import from_bytes
        HAS_CHARSET_NORMALIZER = True
    except ImportError:
        logger.warning("charset-normalizer unavailable, using fallback encoding list")

    raw_body = await request.body()

    if HAS_CHARSET_NORMALIZER:
        result = from_bytes(raw_body).best()

        if result and result.coherence >= 0.7:
            encoding = result.encoding
            logger.info(f"Auto-detected {encoding} (coherence: {result.coherence:.0%})")
            decoded = str(result)
            try:
                body = json.loads(decoded)
                if not isinstance(body, dict):
                    return None, JSONResponse(
                        status_code=400,
                        content={
                            "success": False,
                            "error": "Тело запроса должно быть JSON объектом / "
                                     "Request body must be a JSON object"
                        }
                    )
                return body, None
            except json.JSONDecodeError as e:
                return None, JSONResponse(
                    status_code=400,
                    content={
                        "success": False,
                        "error": f"Ошибка разбора JSON: {e.msg} в позиции {e.pos} / "
                                 f"JSON parse error: {e.msg} at position {e.pos}"
                    }
                )

    # 4. Fallback to known encodings (ALWAYS works even without charset-normalizer)
    # CP1251 first (modern Windows default), then CP866 (legacy DOS).
    # Both are 8-bit and decode ANY bytes, so we try ALL and pick the best
    # using _encoding_quality_score (most standard-Russian-looking result wins).
    best_body = None
    best_score = None
    best_encoding = None
    last_json_error = None

    for encoding in ['cp1251', 'cp866', 'utf-16', 'utf-16-le']:
        try:
            decoded = raw_body.decode(encoding)
            body = json.loads(decoded)
            if not isinstance(body, dict):
                # Not a dict — could still be the right encoding but wrong payload.
                # Remember and keep looking; report error only if nothing better found.
                continue
            score = _encoding_quality_score(body)
            if best_score is None or score > best_score:
                best_body = body
                best_score = score
                best_encoding = encoding
        except UnicodeDecodeError:
            continue
        except json.JSONDecodeError as e:
            last_json_error = e
            continue
        except LookupError:
            continue

    if best_body is not None:
        logger.info(f"Fallback succeeded with {best_encoding} (score {best_score})")
        return best_body, None

    # If we had at least one successful decode with invalid JSON, return JSON error
    if last_json_error:
        return None, JSONResponse(
            status_code=400,
            content={
                "success": False,
                "error": f"Ошибка разбора JSON: {last_json_error.msg} в позиции {last_json_error.pos} / "
                         f"JSON parse error: {last_json_error.msg} at position {last_json_error.pos}"
            }
        )

    # 5. All methods failed
    logger.warning(f"All encoding detection methods failed (size: {len(raw_body)})")
    return None, JSONResponse(
        status_code=400,
        content={
            "success": False,
            "error": "Не удалось определить кодировку текста. "
                     "Используйте UTF-8 или укажите charset в Content-Type / "
                     "Failed to detect text encoding. "
                     "Use UTF-8 or specify charset in Content-Type header",
            "hint": "Try: Content-Type: application/json; charset=utf-8"
        }
    )


def _validation_error_response(e: ValidationError) -> JSONResponse:
    """
    Create bilingual validation error response (HTTP 422) with field name.
    
    Validates: Requirement 8.2
    - 8.2: Validation errors return HTTP 422 with detailed error description
    
    Args:
        e: Pydantic ValidationError
        
    Returns:
        JSONResponse with 422 status and error details including field name
    """
    # Format error with field name for clarity
    if e.errors():
        first_error = e.errors()[0]
        field_name = first_error.get('loc', ['unknown'])[0] if first_error.get('loc') else 'unknown'
        error_msg = first_error.get('msg', str(e))
        error_detail = f"Field '{field_name}': {error_msg}"
    else:
        error_detail = str(e)
    
    return JSONResponse(
        status_code=422,
        content={
            "success": False,
            "error": f"Ошибка валидации параметров: {error_detail} / Parameter validation failed: {error_detail}"
        }
    )


def _parse_csv_or_repeated_query_param(request: Request, name: str) -> Optional[List[str]]:
    """
    Parse a query parameter supporting both CSV and repeated-key forms.

    Examples:
    - ?meta_type=Document,Catalog
    - ?meta_type=Document&meta_type=Catalog
    - mixed: ?meta_type=Document,Catalog&meta_type=Register
    """
    raw_values = request.query_params.getlist(name)
    if not raw_values:
        return None

    values: List[str] = []
    for raw in raw_values:
        for part in str(raw).split(","):
            item = part.strip()
            if item:
                values.append(item)

    return values or None


# Placeholder handlers - will be implemented in subsequent tasks

async def execute_query_handler(request: Request) -> JSONResponse:
    """
    POST /api/execute_query - Execute 1C query language query.
    
    Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5, 1.6
    - 1.1: POST request with JSON body {"query": "...", "limit": N} executes query
    - 1.2: Empty or missing query returns HTTP 422
    - 1.3: Default limit is 100 if not specified
    - 1.4: params parameter is passed to 1C query
    - 1.5: Successful query returns HTTP 200 with {"success": true, "data": [...]}
    - 1.6: Failed query returns HTTP 200 with {"success": false, "error": "..."}
    
    Order of checks: Content-Type (415) → JSON parsing (400) → validation (422)
    
    Request format:
    {
        "query": "ВЫБРАТЬ * ИЗ Справочник.Номенклатура",
        "params": {"КодТовара": "001"},  // optional
        "limit": 100  // optional, default 100
    }
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Validate parameters via Pydantic (applies defaults automatically)
    try:
        validated_params = ExecuteQueryParams.model_validate(body)
    except ValidationError as e:
        return _validation_error_response(e)
    
    # Step 4: Get channel from request
    channel = _get_channel(request)
    
    # Step 5: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = validated_params.model_dump(exclude_none=True)
    
    result = await _execute_1c_command("execute_query", params_dict, channel)
    
    # Step 6: Return result as JSONResponse
    # Result already has success/data/error structure from _execute_1c_command
    return JSONResponse(content=result)


async def execute_code_handler(request: Request) -> JSONResponse:
    """
    POST /api/execute_code - Execute arbitrary 1C code.
    
    Validates: Requirements 2.1, 2.2, 2.3, 2.4
    - 2.1: POST request with JSON body {"code": "..."} executes code
    - 2.2: Empty or missing code returns HTTP 422
    - 2.3: Dangerous keywords trigger blocking/approval logic (same as MCP)
    - 2.4: Successful execution returns value of 'Результат' variable in data field
    
    IMPORTANT: Reuses dangerous keyword checking logic from mcp_handler.py
    (find_dangerous_keywords, settings.dangerous_keywords, settings.allow_dangerous_with_approval).
    
    Order of checks: Content-Type (415) → JSON parsing (400) → validation (422)
    
    Request format:
    {
        "code": "Результат = ТекущаяДата();"
    }
    
    Response for dangerous code (blocked):
    {
        "success": false,
        "error": "Код содержит опасные ключевые слова: УдалитьОбъект / Code contains dangerous keywords: УдалитьОбъект"
    }
    
    Response for dangerous code when approval mode is enabled:
    The command is sent to 1C with requires_approval flag and this endpoint waits
    for the final result (approved execution result OR rejected error).
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Extract code parameter from body
    code = body.get("code", "")
    
    # Step 4: Validate parameters via Pydantic
    try:
        validated_params = validate_execute_code_params(code=code)
    except ValidationError as e:
        return _validation_error_response(e)
    
    # Step 5: Get channel from request
    channel = _get_channel(request)
    
    # Step 6: Check for dangerous keywords (same logic as MCP handler)
    found_dangerous = find_dangerous_keywords(validated_params.code, settings.dangerous_keywords)
    
    if found_dangerous:
        keywords_str = ", ".join(found_dangerous)
        
        if settings.allow_dangerous_with_approval:
            # Approval mode: send to 1C with requires_approval flag (same as MCP handler)
            logger.info(f"REST API: Dangerous code requires approval: {found_dangerous}")
            params_dict = {
                "code": validated_params.code,
                "requires_approval": True,
                "dangerous_keywords": found_dangerous
            }
            result = await _execute_1c_command("execute_code", params_dict, channel)
            return JSONResponse(content=result)
        else:
            # Block mode (default): reject dangerous code
            logger.warning(f"REST API: Blocked dangerous operation: {found_dangerous}")
            return JSONResponse(content={
                "success": False,
                "error": f"Операция запрещена: код содержит опасные ключевые слова: {keywords_str} / "
                         f"Operation not allowed: code contains dangerous keywords: {keywords_str}",
                "dangerous_keywords": found_dangerous
            })
    
    # Step 7: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = {"code": validated_params.code}
    
    result = await _execute_1c_command("execute_code", params_dict, channel)
    
    # Step 8: Return result as JSONResponse
    return JSONResponse(content=result)


async def get_metadata_handler(request: Request) -> JSONResponse:
    """
    GET/POST /api/get_metadata - Get metadata information.
    
    Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8
    - 3.1: GET request without params returns root summary (params from query string)
    - 3.2: GET with filter=... returns detailed structure of specified object
    - 3.3: POST request accepts params from JSON body (query string ignored except channel)
    - 3.4: meta_type parameter filters objects by metadata type
    - 3.5: name_mask parameter filters objects by name mask
    - 3.6: Channel always from query string ?channel=xxx regardless of method
    - 3.7: GET request with body ignores the body, uses only query string
    - 3.8: Invalid query params in GET (e.g., limit=abc) return HTTP 422
    
    GET: parameters from query string, body is IGNORED (even if present)
    POST: parameters from JSON body, query string is ignored (except channel)
    
    Request format (POST):
    {
        "filter": "Справочник.Номенклатура",  // optional
        "meta_type": "Справочник",  // optional (string or array of strings)
        "name_mask": "номенклат",  // optional
        "limit": 100,  // optional, default 100
        "offset": 0,  // optional, default 0 (list mode only)
        "sections": ["properties", "forms"]  // optional, details-only (works with filter)
    }
    
    GET examples:
    - GET /api/get_metadata
    - GET /api/get_metadata?filter=Справочник.Номенклатура
    - GET /api/get_metadata?meta_type=Документ&name_mask=реализ&limit=50
    - GET /api/get_metadata?channel=dev
    """
    # Step 1: Get channel from query string (always, regardless of method)
    channel = _get_channel(request)
    
    if request.method == "GET":
        # GET: parameters from query string, body is IGNORED
        # Step 2: Extract parameters from query string
        filter_param = request.query_params.get("filter")
        meta_type_values = _parse_csv_or_repeated_query_param(request, "meta_type")
        meta_type = (
            meta_type_values[0]
            if meta_type_values and len(meta_type_values) == 1
            else meta_type_values
        )
        name_mask = request.query_params.get("name_mask")
        limit_str = request.query_params.get("limit")
        offset_str = request.query_params.get("offset")
        sections = _parse_csv_or_repeated_query_param(request, "sections")
        # extension_name: None if not in query string, "" if present but empty
        extension_name = request.query_params.get("extension_name")
        
        # Step 3: Parse limit parameter (validate it's a valid integer)
        limit = 100  # default
        if limit_str is not None:
            try:
                limit = int(limit_str)
            except ValueError:
                return JSONResponse(
                    status_code=422,
                    content={
                        "success": False,
                        "error": f"Ошибка валидации: limit должен быть целым числом, получено '{limit_str}' / "
                                 f"Validation error: limit must be an integer, got '{limit_str}'",
                        "details": [
                            {
                                "loc": ["limit"],
                                "msg": f"value is not a valid integer",
                                "type": "int_parsing",
                                "input": limit_str
                            }
                        ]
                    }
                )

        # Step 3.1: Parse offset parameter
        offset = 0  # default
        if offset_str is not None:
            try:
                offset = int(offset_str)
            except ValueError:
                return JSONResponse(
                    status_code=422,
                    content={
                        "success": False,
                        "error": f"Ошибка валидации: offset должен быть целым числом, получено '{offset_str}' / "
                                 f"Validation error: offset must be an integer, got '{offset_str}'",
                        "details": [
                            {
                                "loc": ["offset"],
                                "msg": f"value is not a valid integer",
                                "type": "int_parsing",
                                "input": offset_str
                            }
                        ]
                    }
                )
        
        # Step 4: Validate parameters via Pydantic
        try:
            validated_params = validate_get_metadata_params(
                filter=filter_param,
                meta_type=meta_type,
                name_mask=name_mask,
                limit=limit,
                sections=sections,
                offset=offset,
                extension_name=extension_name
            )
        except ValidationError as e:
            return _validation_error_response(e)

    else:
        # POST: parameters from JSON body
        # Step 2: Check Content-Type
        content_type_error = _check_content_type(request)
        if content_type_error:
            return content_type_error

        # Step 3: Parse JSON body
        body, parse_error = await _parse_json_body_with_encoding_detection(request)
        if parse_error:
            return parse_error

        # Step 4: Extract parameters from body
        filter_param = body.get("filter")
        meta_type = body.get("meta_type")
        name_mask = body.get("name_mask")
        limit = body.get("limit", 100)
        offset = body.get("offset", 0)
        sections = body.get("sections")
        extension_name = body.get("extension_name")

        # Step 5: Validate parameters via Pydantic
        try:
            validated_params = validate_get_metadata_params(
                filter=filter_param,
                meta_type=meta_type,
                name_mask=name_mask,
                limit=limit,
                sections=sections,
                offset=offset,
                extension_name=extension_name
            )
        except ValidationError as e:
            return _validation_error_response(e)
    
    # Step 6: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = validated_params.model_dump(exclude_none=True)
    
    result = await _execute_1c_command("get_metadata", params_dict, channel)
    
    # Step 7: Return result as JSONResponse
    return JSONResponse(content=result)


async def get_event_log_handler(request: Request) -> JSONResponse:
    """
    POST /api/get_event_log - Get event log entries.
    
    Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5
    - 4.1: POST request with JSON body returns event log entries
    - 4.2: start_date and end_date parameters filter entries by period
    - 4.3: levels parameter filters entries by importance level
    - 4.4: Default limit is 100 if not specified
    - 4.5: All filters from MCP tool get_event_log are supported
    
    Order of checks: Content-Type (415) → JSON parsing (400) → validation (422)
    
    Request format:
    {
        "start_date": "2024-01-01T00:00:00",  // optional
        "end_date": "2024-01-31T23:59:59",  // optional
        "levels": ["Error", "Warning"],  // optional
        "events": ["_$Data$_.New", "_$Data$_.Update"],  // optional
        "limit": 100,  // optional, default 100
        "object_description": {  // optional, priority: object_description > link > data
            "_objectRef": true,
            "УникальныйИдентификатор": "...",
            "ТипОбъекта": "..."
        },
        "link": "e1cib/data/...",  // optional, priority: object_description > link > data
        "data": "e1cib/data/...",  // optional, priority: object_description > link > data (navigation link only)
        "metadata_type": "Справочник",  // optional (can be string or list)
        "user": "Администратор",  // optional (can be string or list)
        "session": 12345,  // optional (can be int or list)
        "application": "1CV8",  // optional (can be string or list)
        "computer": "SERVER01",  // optional
        "comment_contains": "ошибка",  // optional
        "transaction_status": "Committed"  // optional
    }
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Extract parameters from body
    start_date = body.get("start_date")
    end_date = body.get("end_date")
    levels = body.get("levels")
    events = body.get("events")
    limit = body.get("limit", 100)  # Default limit is 100 per requirement 4.4
    object_description = body.get("object_description")
    link = body.get("link")
    data = body.get("data")
    metadata_type = body.get("metadata_type")
    user = body.get("user")
    session = body.get("session")
    application = body.get("application")
    computer = body.get("computer")
    comment_contains = body.get("comment_contains")
    transaction_status = body.get("transaction_status")
    same_second_offset = body.get("same_second_offset", 0)  # Cursor pagination offset

    # Be tolerant to accidental double-serialization: metadata_type can be sent as a JSON string
    # like "[\"Документ.ВходящийДокумент\"]" instead of an actual array.
    if isinstance(metadata_type, str):
        raw = metadata_type.strip()
        if raw.startswith("[") and raw.endswith("]"):
            try:
                parsed = json.loads(raw)
                if isinstance(parsed, list):
                    metadata_type = parsed
            except json.JSONDecodeError:
                pass
    elif isinstance(metadata_type, list) and len(metadata_type) == 1 and isinstance(metadata_type[0], str):
        raw = metadata_type[0].strip()
        if raw.startswith("[") and raw.endswith("]"):
            try:
                parsed = json.loads(raw)
                if isinstance(parsed, list):
                    metadata_type = parsed
            except json.JSONDecodeError:
                pass
    
    # Step 4: Normalize parameters that can be single values or lists
    # metadata_type, user, session, application can be passed as single values
    # but the validator expects lists
    if metadata_type is not None and not isinstance(metadata_type, list):
        metadata_type = [metadata_type]
    if user is not None and not isinstance(user, list):
        user = [user]
    if session is not None and not isinstance(session, list):
        session = [session]
    if application is not None and not isinstance(application, list):
        application = [application]
    if events is not None and not isinstance(events, list):
        events = [events]
    if levels is not None and not isinstance(levels, list):
        levels = [levels]
    
    # Step 5: Validate parameters via Pydantic
    try:
        validated_params = validate_get_event_log_params(
            start_date=start_date,
            end_date=end_date,
            levels=levels,
            events=events,
            limit=limit,
            object_description=object_description,
            link=link,
            data=data,
            metadata_type=metadata_type,
            user=user,
            session=session,
            application=application,
            computer=computer,
            comment_contains=comment_contains,
            transaction_status=transaction_status,
            same_second_offset=same_second_offset
        )
    except ValidationError as e:
        return _validation_error_response(e)
    
    # Step 6: Get channel from request
    channel = _get_channel(request)
    
    # Step 7: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = validated_params.model_dump(exclude_none=True)
    
    result = await _execute_1c_command("get_event_log", params_dict, channel)
    
    # Step 8: Return result as JSONResponse
    return JSONResponse(content=result)


async def get_object_by_link_handler(request: Request) -> JSONResponse:
    """
    POST /api/get_object_by_link - Get object data by navigation link.
    
    Validates: Requirements 5.1, 5.2, 5.3
    - 5.1: POST request with JSON body {"link": "e1cib/data/..."} returns object data
    - 5.2: Invalid link format returns HTTP 422 with format error description
    - 5.3: Object not found returns HTTP 200 with {"success": false, "error": "..."}
    
    Order of checks: Content-Type (415) → JSON parsing (400) → validation (422)
    
    Request format:
    {
        "link": "e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5"
    }
    
    Response format (success):
    {
        "success": true,
        "data": {
            "Код": "001",
            "Наименование": "Контрагент 1",
            ...
        }
    }
    
    Response format (not found):
    {
        "success": false,
        "error": "Объект не найден / Object not found"
    }
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Extract link parameter from body
    link = body.get("link", "")
    
    # Step 4: Validate parameters via Pydantic
    try:
        validated_params = validate_get_object_by_link_params(link=link)
    except ValidationError as e:
        return _validation_error_response(e)
    
    # Step 5: Get channel from request
    channel = _get_channel(request)
    
    # Step 6: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = {"link": validated_params.link}
    
    result = await _execute_1c_command("get_object_by_link", params_dict, channel)
    
    # Step 7: Return result as JSONResponse
    # Result already has success/data/error structure from _execute_1c_command
    # Object not found will return {"success": false, "error": "..."} with HTTP 200
    return JSONResponse(content=result)


async def get_link_of_object_handler(request: Request) -> JSONResponse:
    """
    POST /api/get_link_of_object - Generate navigation link from object description.
    
    Validates: Requirements 6.1, 6.2, 6.3
    - 6.1: POST request with object description returns navigation link
    - 6.2: Incomplete object description returns HTTP 422 with error description
    - 6.3: Successful generation returns {"success": true, "data": "e1cib/data/..."} (data is a string)
    
    Order of checks: Content-Type (415) → JSON parsing (400) → validation (422)
    
    Request format:
    {
        "object_description": {
            "_objectRef": true,
            "УникальныйИдентификатор": "80c6cc1a-7e58-9028-11eb-cda8cb07c0f5",
            "ТипОбъекта": "СправочникСсылка.Контрагенты"
        }
    }
    
    Response format (success):
    {
        "success": true,
        "data": "e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5"
    }
    
    Note: data is a STRING (navigation link), not an object.
    This matches the current 1C client implementation.
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Extract object_description parameter from body
    object_description = body.get("object_description", {})
    
    # Step 4: Validate parameters via Pydantic
    try:
        validated_params = validate_get_link_of_object_params(
            object_description=object_description
        )
    except ValidationError as e:
        return _validation_error_response(e)
    
    # Step 5: Get channel from request
    channel = _get_channel(request)
    
    # Step 6: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = {"object_description": validated_params.object_description}
    
    result = await _execute_1c_command("get_link_of_object", params_dict, channel)
    
    # Step 7: Return result as JSONResponse
    # Result already has success/data/error structure from _execute_1c_command
    # On success, data is a STRING (navigation link), not an object
    return JSONResponse(content=result)


async def find_references_to_object_handler(request: Request) -> JSONResponse:
    """
    POST /api/find_references_to_object - Find references to an object.

    Finds references to the target object across specified metadata collections
    (documents, catalogs, registers).

    Order of checks: Content-Type (415) -> JSON parsing (400) -> validation (422)

    Request format:
    {
        "target_object_description": {
            "_objectRef": true,
            "УникальныйИдентификатор": "...",
            "ТипОбъекта": "СправочникСсылка.Контрагенты",
            "Представление": "ООО Рога и Копыта"
        },
        "search_scope": ["documents", "catalogs"],
        "meta_filter": {  // optional
            "names": ["Документ.РеализацияТоваровУслуг"],
            "name_mask": "реализ"
        },
        "limit_hits": 200,  // optional, default 200
        "limit_per_meta": 20,  // optional, default 20
        "timeout_budget_sec": 30  // optional, default 30
    }
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Extract parameters from body
    target_object_description = body.get("target_object_description", {})
    search_scope = body.get("search_scope", [])
    meta_filter = body.get("meta_filter")
    limit_hits = body.get("limit_hits", 200)
    limit_per_meta = body.get("limit_per_meta", 20)
    timeout_budget_sec = body.get("timeout_budget_sec", 30)

    # Step 4: Validate parameters via Pydantic
    try:
        validated_params = validate_find_references_to_object_params(
            target_object_description=target_object_description,
            search_scope=search_scope,
            meta_filter=meta_filter,
            limit_hits=limit_hits,
            limit_per_meta=limit_per_meta,
            timeout_budget_sec=timeout_budget_sec
        )
    except ValidationError as e:
        return _validation_error_response(e)

    # Step 5: Get channel from request
    channel = _get_channel(request)

    # Step 6: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = validated_params.model_dump(exclude_none=True)

    result = await _execute_1c_command("find_references_to_object", params_dict, channel)

    # Step 7: Return result as JSONResponse
    return JSONResponse(content=result)


async def get_access_rights_handler(request: Request) -> JSONResponse:
    """
    POST /api/get_access_rights - Get role permissions for a metadata object.

    Gets role permissions for a metadata object and optionally effective rights for a user.

    Order of checks: Content-Type (415) -> JSON parsing (400) -> validation (422)

    Request format:
    {
        "metadata_object": "Справочник.Контрагенты",
        "user_name": "Иванов",  // optional
        "rights_filter": ["Чтение", "Изменение"],  // optional
        "roles_filter": ["ПолныеПрава", "Менеджер"]  // optional
    }
    """
    # Step 1: Check Content-Type
    content_type_error = _check_content_type(request)
    if content_type_error:
        return content_type_error

    # Step 2: Parse JSON body
    body, parse_error = await _parse_json_body_with_encoding_detection(request)
    if parse_error:
        return parse_error

    # Step 3: Extract parameters from body
    metadata_object = body.get("metadata_object", "")
    user_name = body.get("user_name")
    rights_filter = body.get("rights_filter")
    roles_filter = body.get("roles_filter")

    # Step 4: Validate parameters via Pydantic
    try:
        validated_params = validate_get_access_rights_params(
            metadata_object=metadata_object,
            user_name=user_name,
            rights_filter=rights_filter,
            roles_filter=roles_filter
        )
    except ValidationError as e:
        return _validation_error_response(e)

    # Step 5: Get channel from request
    channel = _get_channel(request)

    # Step 6: Execute command via _execute_1c_command
    # Convert validated params to dict for _execute_1c_command
    params_dict = validated_params.model_dump(exclude_none=True)

    result = await _execute_1c_command("get_access_rights", params_dict, channel)

    # Step 7: Return result as JSONResponse
    return JSONResponse(content=result)
