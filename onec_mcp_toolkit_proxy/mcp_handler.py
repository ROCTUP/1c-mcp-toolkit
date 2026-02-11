"""
MCP HTTP Streamable Transport handler for 1C MCP Toolkit Proxy.

This module provides the MCP server implementation using the official MCP SDK.
It creates an MCP server with tools for interacting with 1C:Enterprise databases.

The MCP server exposes four tools:
- execute_query: Execute 1C query language queries
- execute_code: Execute arbitrary 1C code
- get_metadata: Get metadata information about 1C database objects
- get_event_log: Get event log entries from 1C database

Validates: Requirements 2.1, 3.1, 4.1, Event Log 1.1
"""

import asyncio
import logging
import unicodedata
from typing import Any, Dict, List, Optional, Union

from mcp.server.fastmcp import FastMCP, Context
from pydantic import ValidationError

from .command_queue import channel_command_queue
from .config import settings
from .response_formatter import format_tool_result, is_toon_available
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

logger = logging.getLogger(__name__)

# Check TOON availability at startup (Requirement 3.2)
# If TOON format is requested but library not installed, log error and use JSON
if settings.response_format == "toon" and not is_toon_available():
    logger.error(
        "TOON format requested but toon-format library not installed. "
        "Using JSON format instead."
    )


_ZERO_WIDTH_CHARS = {"\u200b", "\u200c", "\u200d", "\ufeff"}


def _normalize_for_scan(text: str) -> str:
    """
    Normalize input for keyword scanning.

    - NFKC normalization to reduce Unicode representation variance
    - remove zero-width characters often used for obfuscation
    """
    normalized = unicodedata.normalize("NFKC", text)
    return "".join(ch for ch in normalized if ch not in _ZERO_WIDTH_CHARS)


def _is_ident_start(ch: str) -> bool:
    """Return True if character can start a 1C identifier."""
    return ch == "_" or ch.isalpha()


def _is_ident_part(ch: str) -> bool:
    """Return True if character can be a part of a 1C identifier."""
    return ch == "_" or ch.isalnum()


def _tokenize_1c_code(code: str) -> List[tuple]:
    """
    Tokenize a minimal subset of 1C code for safe keyword detection.

    The tokenizer intentionally ignores comments and string literals to avoid
    false positives from plain text.
    """
    tokens: List[tuple] = []
    i = 0
    n = len(code)

    while i < n:
        ch = code[i]

        if ch.isspace():
            i += 1
            continue

        if ch == "/" and i + 1 < n:
            nxt = code[i + 1]
            # // line comment
            if nxt == "/":
                i += 2
                while i < n and code[i] not in ("\n", "\r"):
                    i += 1
                continue
            # /* block comment */
            if nxt == "*":
                i += 2
                while i + 1 < n and not (code[i] == "*" and code[i + 1] == "/"):
                    i += 1
                i = i + 2 if i + 1 < n else n
                continue

        # 1C string literal: "text", escaped quote is doubled ""
        if ch == '"':
            i += 1
            while i < n:
                if code[i] == '"':
                    if i + 1 < n and code[i + 1] == '"':
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            continue

        # Keep single-quoted text out of analysis too (can appear in embedded text)
        if ch == "'":
            i += 1
            while i < n:
                if code[i] == "'":
                    if i + 1 < n and code[i + 1] == "'":
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            continue

        if _is_ident_start(ch):
            start = i
            i += 1
            while i < n and _is_ident_part(code[i]):
                i += 1
            tokens.append(("IDENT", code[start:i]))
            continue

        if ch == ".":
            tokens.append(("DOT", ch))
            i += 1
            continue
        if ch == "(":
            tokens.append(("LPAREN", ch))
            i += 1
            continue
        if ch == ")":
            tokens.append(("RPAREN", ch))
            i += 1
            continue

        tokens.append(("SYMBOL", ch))
        i += 1

    return tokens


def _collect_called_identifiers(code: str) -> set:
    """
    Return canonical names of identifiers used as calls in code.

    We treat IDENT followed by LPAREN as a call context.
    """
    tokens = _tokenize_1c_code(code)
    called: set = set()
    for idx, (kind, value) in enumerate(tokens):
        if kind != "IDENT":
            continue
        if idx + 1 < len(tokens) and tokens[idx + 1][0] == "LPAREN":
            called.add(value.casefold())
    return called


def find_dangerous_keywords(code: str, dangerous_keywords: list) -> list:
    """
    Find dangerous keywords in the given code.
    
    Args:
        code: The code to check
        dangerous_keywords: List of dangerous keywords to search for
        
    Returns:
        List of found dangerous keywords (case-preserved from the keyword list)
    """
    normalized_code = _normalize_for_scan(code)
    called_identifiers = _collect_called_identifiers(normalized_code)

    found = []
    seen = set()
    for keyword in dangerous_keywords:
        canonical_keyword = _normalize_for_scan(keyword).casefold()
        if not canonical_keyword or canonical_keyword in seen:
            continue
        if canonical_keyword in called_identifiers:
            found.append(keyword)
            seen.add(canonical_keyword)
    return found

# Create the MCP server instance
mcp = FastMCP(
    name="1C MCP Toolkit Proxy",
    instructions="""
    This MCP server provides tools for interacting with 1C:Enterprise databases.

    Available tools:
    - execute_query: Execute queries in 1C query language
    - execute_code: Execute arbitrary 1C code (with safety restrictions)
    - get_metadata: Get information about database structure and metadata
    - get_event_log: Get event log entries with filtering by date, level, and event type
    - get_object_by_link: Get 1C object data by navigation link
    - get_link_of_object: Generate a 1C navigation link from an object description
    - find_references_to_object: Find references to a given object across metadata collections
    - get_access_rights: Get role permissions for a metadata object and optionally effective rights for a user

    The server acts as a proxy between AI agents and 1C:Enterprise,
    forwarding commands to a connected 1C processing client.
    """
)


async def _execute_1c_command(tool: str, params: Dict[str, Any], channel: str = "default") -> Dict[str, Any]:
    """
    Execute a command on the 1C client and wait for the result.
    
    Args:
        tool: Name of the tool (execute_query, execute_code, get_metadata)
        params: Parameters for the tool
        channel: Channel ID for routing the command
        
    Returns:
        Result from 1C processing
        
    Validates: Requirements 6.2, 6.3
    - 6.2: Прокси возвращает понятную ошибку если обработка 1С не подключена
    - 6.3: Таймаут запроса к 1С не блокирует прокси для других операций
    """
    # Check if 1C client is connected by checking if there was recent activity
    # If there are too many pending commands, 1C might not be connected
    channel_stats = await channel_command_queue.get_stats()
    pending_count = sum(channel_stats.values())
    logger.debug(f"Executing {tool} command on channel '{channel}', pending commands: {pending_count}")
    
    # Warning if many commands are pending (possible 1C disconnection)
    if pending_count > 10:
        logger.warning(
            f"High number of pending commands ({pending_count}). "
            "1C processing might be disconnected or slow."
        )
    
    # Add command to channel queue
    command_id = await channel_command_queue.add_command(channel, tool, params)
    logger.info(f"Command {command_id} added to channel '{channel}': tool={tool}")
    
    try:
        # Wait for result with timeout (non-blocking for other operations)
        # Validates: Requirement 6.3 - timeout doesn't block proxy for other operations
        result = await channel_command_queue.wait_for_result(
            command_id, 
            timeout=float(settings.timeout)
        )
        logger.info(f"Command {command_id} completed successfully on channel '{channel}'")
        
        # Format result based on configuration (Requirement 2.1, 2.2, 5.1, 5.2, 5.3)
        # Applies TOON or JSON formatting to the result data
        return format_tool_result(result, settings.response_format)
    except asyncio.TimeoutError:
        # Validates: Requirement 6.2 - clear error message when 1C not responding
        logger.error(f"Command {command_id} timed out after {settings.timeout}s on channel '{channel}'")
        return {
            "success": False,
            "error": f"Таймаут ожидания ответа от 1С на канале '{channel}' (>{settings.timeout}с). "
                     "Убедитесь, что клиент 1С подключён с тем же channel ID. / "
                     f"Timeout waiting for 1C response on channel '{channel}' (>{settings.timeout}s). "
                     "Make sure 1C client is connected with the same channel ID."
        }
    except KeyError as e:
        logger.error(f"Command {command_id} not found: {e}")
        return {
            "success": False,
            "error": f"Команда не найдена: {e} / Command not found: {e}"
        }
    except Exception as e:
        # Catch any unexpected errors
        logger.exception(f"Unexpected error executing command {command_id}: {e}")
        return {
            "success": False,
            "error": f"Внутренняя ошибка прокси: {str(e)} / Internal proxy error: {str(e)}"
        }


def _get_channel_from_context(ctx: Context) -> str:
    """
    Extract channel from MCP Context.
    
    Args:
        ctx: MCP Context object
        
    Returns:
        Channel ID or "default" if not found.
    """
    channel = "default"
    try:
        if ctx.request_context and ctx.request_context.request:
            channel = ctx.request_context.request.scope.get("channel", "default")
    except Exception:
        pass
    return channel


@mcp.tool()
async def execute_query(
    ctx: Context,
    query: str,
    params: Optional[Dict[str, Any]] = None,
    limit: Optional[int] = None,
    include_schema: Optional[bool] = None
) -> Dict[str, Any]:
    """
    Execute a query in 1C query language and return the results.
    
    This tool executes queries on the 1C:Enterprise database using the 1C query language.
    Results are returned as JSON or TOON format.
    
    Args:
        ctx: MCP Context (injected automatically)
        query: The query text in 1C query language (e.g., "ВЫБРАТЬ * ИЗ Справочник.Номенклатура")
        params: Optional dictionary of query parameters (e.g., {"Param1": "value1"}).
               Parameter values can be:
               - Simple values (string, number, boolean, date)
               - Object references in format: {"_objectRef": true, "УникальныйИдентификатор": "uuid-string", "ТипОбъекта": "СправочникСсылка.Контрагенты"}
                 Object references are automatically converted to actual 1C references before query execution.
        limit: Maximum number of rows to return (default: 100, max: 1000)
        include_schema: Include column type schema in response (default: False)
        
    Returns:
        Dictionary with:
        - success: Boolean indicating if the query executed successfully
        - data: Array of result objects in JSON or TOON format (if successful)
        - schema: Column type information (if include_schema=True and successful)
        - error: Error message (if failed)
        
    Examples:
        # Simple parameter
        execute_query(
            query="ВЫБРАТЬ Код, Наименование ИЗ Справочник.Номенклатура ГДЕ Код = &КодТовара",
            params={"КодТовара": "001"},
            limit=100,
            include_schema=True
        )
        
        # Object reference parameter (from previous execute_query result)
        execute_query(
            query="ВЫБРАТЬ * ИЗ Документ.РеализацияТоваровУслуг ГДЕ Контрагент = &МойКонтрагент",
            params={
                "МойКонтрагент": {
                    "_objectRef": true,
                    "УникальныйИдентификатор": "a1b2c3d4-e5f6-7890-1234-567890abcdef",
                    "ТипОбъекта": "СправочникСсылка.Контрагенты"
                }
            },
            limit=50
        )
    """
    channel = _get_channel_from_context(ctx)
    logger.info(f"execute_query on channel '{channel}': query length={len(query)}")
    
    # Validate parameters using Pydantic model (applies defaults automatically)
    # Build dict excluding None values so Pydantic applies Field defaults
    # Validates: Requirement 6.4 - JSON serialization/deserialization errors with clear messages
    try:
        params_dict = {
            "query": query,
            "params": params,
            "limit": limit,
            "include_schema": include_schema
        }
        # Remove None values to let Pydantic apply defaults
        params_dict = {k: v for k, v in params_dict.items() if v is not None}
        
        validated = ExecuteQueryParams.model_validate(params_dict)
    except ValidationError as e:
        # Format error with field name for clarity
        if e.errors():
            first_error = e.errors()[0]
            field_name = first_error.get('loc', ['unknown'])[0] if first_error.get('loc') else 'unknown'
            error_msg = first_error.get('msg', str(e))
            error_detail = f"Field '{field_name}': {error_msg}"
        else:
            error_detail = str(e)
        
        logger.warning(f"execute_query validation failed: {error_detail}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_detail} / Parameter validation failed: {error_detail}"
        }
    except ValueError as e:
        logger.warning(f"execute_query validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"execute_query type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }
    
    result = await _execute_1c_command("execute_query", {
        "query": validated.query,
        "params": validated.params or {},
        "limit": validated.limit,
        "include_schema": validated.include_schema
    }, channel=channel)
    
    return result


@mcp.tool()
async def execute_code(ctx: Context, code: str) -> Dict[str, Any]:
    """
    Execute arbitrary code in 1C language.
    
    This tool executes code on the 1C:Enterprise server using the Execute() operator.
    The result is returned from the 'Результат' (Result) variable as JSON or TOON format.
    
    Important limitations (code is executed as a statement block, not a full module):
    - Do NOT declare procedures/functions (`Процедура/Функция`) inside the snippet.
    - Do NOT use `Возврат` (Return). Instead assign a value to `Результат`.
    
    WARNING: Some dangerous operations are blocked for safety reasons.
    
    Args:
        ctx: MCP Context (injected automatically)
        code: The 1C code to execute. Use 'Результат = ...' to return a value.
        
    Returns:
        Dictionary with:
        - success: Boolean indicating if the code executed successfully
        - data: The value of 'Результат' variable in JSON or TOON format (if successful)
        - error: Error message with line number (if failed)
        
    Example:
        execute_code(code="Результат = ТекущаяДата();")
    """
    channel = _get_channel_from_context(ctx)
    logger.info(f"execute_code on channel '{channel}': code length={len(code)}")
    
    # Validate parameters using Pydantic model
    # Validates: Requirement 6.4 - JSON serialization/deserialization errors with clear messages
    try:
        validated = validate_execute_code_params(code=code)
    except ValidationError as e:
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"execute_code validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"execute_code validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"execute_code type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }
    
    # Check for dangerous keywords (blacklist validation)
    # Validates: Requirement 3.5 - mechanism for blocking dangerous operations
    found_dangerous = find_dangerous_keywords(validated.code, settings.dangerous_keywords)
    
    if found_dangerous:
        keywords_str = ", ".join(found_dangerous)
        if settings.allow_dangerous_with_approval:
            # Approval mode: send to 1C with requires_approval flag
            logger.info(f"Dangerous code requires approval: {found_dangerous}")
            result = await _execute_1c_command("execute_code", {
                "code": validated.code,
                "requires_approval": True,
                "dangerous_keywords": found_dangerous
            }, channel=channel)
        else:
            # Block mode (default): reject dangerous code
            logger.warning(f"Blocked dangerous operation: {found_dangerous}")
            return {
                "success": False,
                "error": f"Операция запрещена: код содержит опасные ключевые слова: {keywords_str} / "
                         f"Operation not allowed: code contains dangerous keywords: {keywords_str}",
                "dangerous_keywords": found_dangerous,
            }
    else:
        # Safe code: execute normally
        result = await _execute_1c_command("execute_code", {
            "code": validated.code
        }, channel=channel)
    
    return result


@mcp.tool()
async def get_metadata(
    ctx: Context,
    filter: Optional[str] = None,
    meta_type: Optional[Union[str, List[str]]] = None,
    name_mask: Optional[str] = None,
    limit: int = 100,
    sections: Optional[List[str]] = None,
    offset: int = 0,
    extension_name: Optional[str] = None
) -> Dict[str, Any]:
    """
    Get metadata information about 1C database objects.

    This tool returns information about the structure of the 1C database in JSON format.

    Usage modes:
    1. Summary (root types): No parameters (filter/meta_type/name_mask not provided)
       Returns:
         - data: list of root metadata types with counts (Тип, Количество)
         - configuration: configuration/platform info (when available) as a top-level field

    2. Filtered list: Use 'meta_type' and/or 'name_mask' parameters
       Example: meta_type="Документ", name_mask="реализ"
       Returns: List of objects matching the criteria.
       Each item includes: ПолноеИмя, Синоним.
       List results are sorted by ПолноеИмя and support pagination via offset.
       If list is cut by limit, response includes: truncated, limit, returned, offset, has_more, next_offset.

    3. Detailed structure: Use 'filter' parameter with exact full name
       Example: filter="Справочник.Номенклатура"
       Returns: Full structure with attributes, dimensions, resources, tabular sections

    4. Extensions support: Use 'extension_name' parameter
       - extension_name not provided: work with main configuration (default)
       - extension_name="": get list of all connected extensions
       - extension_name="ExtensionName": work with objects inside the specified extension

    Args:
        ctx: MCP Context (injected automatically)
        filter: Exact object name for detailed structure (e.g., "Справочник.Номенклатура")
        meta_type: Object type filter for list (string or list). Use "*" to list across all root types.
        name_mask: Search mask for name/synonym (case-insensitive substring search)
        limit: Maximum number of objects in list (default: 100, max: 1000)
        sections: Detail sections to include (works only with filter). Supported: properties, forms, commands, layouts, predefined
        offset: Offset for pagination in list mode (default: 0)
        extension_name: Extension name (None=main config, ""=list extensions, "Name"=extension objects).
                       Whitespace-only values are rejected.

    Returns:
        Dictionary with:
        - success: Boolean indicating if the request was successful
         - data: Metadata information in JSON format:
             - Summary: array data of {Тип, Количество} plus optional top-level field configuration
             - List: array of {ПолноеИмя, Синоним}
             - Details: object structure; sections like Реквизиты/ТабличныеЧасти/Измерения/Ресурсы may be omitted if not applicable for the metadata object type (if applicable, they are returned as arrays and may be empty)
             - Extensions list: array of extension properties (ConfigurationExtension), e.g. {Имя, Синоним, Активно, БезопасныйРежим, Версия, УникальныйИдентификатор, Назначение, ОбластьДействия, ...}
        - extension: Extension name (when working with extension objects)
        - error: Error message (if failed)

    Examples:
        # Get summary of root metadata types (counts)
        get_metadata()

        # Get only documents
        get_metadata(meta_type="Документ")

        # Search for objects with "реализ" in name (across all types)
        get_metadata(name_mask="реализ")

        # Get mixed list across all root types
        get_metadata(meta_type="*", limit=50)

        # Get details about a specific catalog
        get_metadata(filter="Справочник.Номенклатура")

        # Get list of all connected extensions
        get_metadata(extension_name="")

        # Get objects from a specific extension
        get_metadata(extension_name="MyExtension", meta_type="Справочник")

        # Get details about an object in extension
        get_metadata(extension_name="MyExtension", filter="Справочник.МойСправочник")
    """
    channel = _get_channel_from_context(ctx)
    logger.info(f"get_metadata on channel '{channel}': filter={filter}, meta_type={meta_type}, name_mask={name_mask}, limit={limit}, offset={offset}, extension_name={extension_name}")

    # Validate parameters using Pydantic model
    # Validates: Requirement 6.4 - JSON serialization/deserialization errors with clear messages
    try:
        validated = validate_get_metadata_params(
            filter=filter,
            meta_type=meta_type,
            name_mask=name_mask,
            limit=limit,
            sections=sections,
            offset=offset,
            extension_name=extension_name
        )
    except ValidationError as e:
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"get_metadata validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"get_metadata validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"get_metadata type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }
    
    # Keep params serialization consistent with REST handler:
    # optional fields with None are omitted instead of being forced to empty strings.
    params_dict = validated.model_dump(exclude_none=True)

    result = await _execute_1c_command("get_metadata", params_dict, channel=channel)
    
    return result


@mcp.tool()
async def get_event_log(
    ctx: Context,
    start_date: Optional[str] = None,
    end_date: Optional[str] = None,
    levels: Optional[List[str]] = None,
    events: Optional[List[str]] = None,
    limit: int = 100,
    object_description: Optional[Dict[str, Any]] = None,
    link: Optional[str] = None,
    data: Optional[str] = None,
    metadata_type: Optional[List[str]] = None,
    user: Optional[List[str]] = None,
    session: Optional[List[int]] = None,
    application: Optional[List[str]] = None,
    computer: Optional[str] = None,
    comment_contains: Optional[str] = None,
    transaction_status: Optional[str] = None,
    same_second_offset: int = 0
) -> Dict[str, Any]:
    """
    Get event log entries from 1C database with filtering options and cursor pagination.

    This tool retrieves entries from the 1C event log (Журнал регистрации)
    with optional filtering by date range, importance level, event type,
    and additional filters for data object, metadata type, user, session, etc.
    Results are returned as JSON or TOON format.

    Supports cursor pagination for iterating through large result sets:
    - Response includes last_date, next_same_second_offset, and has_more
    - For next page: use last_date as start_date and next_same_second_offset as same_second_offset

    Args:
        start_date: Start date in ISO 8601 format (YYYY-MM-DDTHH:MM:SS).
                   If not provided, no start date filter is applied.
                   For pagination: use last_date from previous response.
        end_date: End date in ISO 8601 format (YYYY-MM-DDTHH:MM:SS).
                 If not provided, no end date filter is applied.
        levels: List of importance levels to filter by.
               Valid values: "Information", "Warning", "Error", "Note".
               If not provided, all levels are returned.
        events: List of event types to filter by (e.g., "_$Data$_.New", "_$Session$_.Start").
               If not provided, all event types are returned.
        limit: Maximum number of records to return (default: 100, max: 1000).
        object_description: Object description from execute_query results.
                          Priority: object_description > link > data.
                          Example: {"_objectRef": true, "УникальныйИдентификатор": "...", "ТипОбъекта": "..."}
        link: Navigation link in format e1cib/data/Type.Name?ref=HexGUID.
             Priority: object_description > link > data.
             Example: "e1cib/data/Документ.РеализацияТоваровУслуг?ref=..."
        data: Reference to data object (navigation link) - for backward compatibility.
              Priority: object_description > link > data.
              Example: "e1cib/data/Документ.РеализацияТоваровУслуг?ref=..."
        metadata_type: List of metadata object types to filter by.
                       Example: ["Документ.РеализацияТоваровУслуг", "Справочник.Номенклатура"].
        user: List of user names to filter by.
        session: List of session numbers to filter by.
        application: List of application types to filter by.
                    Valid values: "ThinClient", "WebClient", "ThickClient",
                    "BackgroundJob", "Designer", "COMConnection",
                    "Server", "WebService", "HTTPService", "ODataInterface",
                    "MobileAppClient", "MobileAppServer", "MobileAppBackgroundJob",
                    "MobileClient", "MobileStandaloneServer",
                    "FileVariantBackgroundJob", "FileVariantServerSide",
                    "WebSocket", "FileVariantWebSocket",
                    "1CV8C", "1CV8".
        computer: Computer name to filter by.
        comment_contains: Substring to search in comment (case-insensitive).
        transaction_status: Transaction status to filter by.
                           Valid values: "Committed", "RolledBack", "NotApplicable", "Unfinished".
        same_second_offset: Skip N records with the same second as start_date (for pagination).
                           For pagination: use next_same_second_offset from previous response.
                           Requires start_date to be specified.

    Returns:
        Dictionary with:
        - success: Boolean indicating if the request was successful
        - data: Array of event log entries in JSON or TOON format (if successful), each containing:
            - date: Event timestamp
            - level: Importance level (Information, Warning, Error, Note)
            - event: Event type
            - comment: Event description/comment
            - user: User name
            - metadata: Metadata object name
            - data_presentation: Data presentation string
            - session: Session number
            - application: Application type
            - computer: Computer name
            - transaction_status: Transaction status
        - count: Number of returned records
        - last_date: ISO 8601 date of last record (cursor for pagination, only with date filter)
        - next_same_second_offset: Accumulated offset for next page (only with date filter)
        - has_more: Boolean indicating if there are more records (only with date filter)
        - error: Error message (if failed)

    Example:
        # Get all errors from the last day
        get_event_log(
            start_date="2024-01-15T00:00:00",
            end_date="2024-01-15T23:59:59",
            levels=["Error", "Warning"],
            limit=100
        )

        # Pagination example - page 1
        result = get_event_log(
            start_date="2024-01-01T00:00:00",
            end_date="2024-01-31T23:59:59",
            levels=["Error"],
            limit=100
        )
        # Response: last_date="2024-01-15T14:30:45", next_same_second_offset=3, has_more=true

        # Pagination example - page 2
        result = get_event_log(
            start_date="2024-01-15T14:30:45",  # last_date from page 1
            end_date="2024-01-31T23:59:59",
            levels=["Error"],
            limit=100,
            same_second_offset=3  # next_same_second_offset from page 1
        )

        # Get entries for specific object using object_description
        get_event_log(
            object_description={
                "_objectRef": True,
                "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
                "ТипОбъекта": "ДокументСсылка.РеализацияТоваровУслуг"
            }
        )

        # Get entries for specific metadata type and user
        get_event_log(
            metadata_type=["Документ.РеализацияТоваровУслуг"],
            user=["Иванов"],
            application=["ThinClient", "WebClient"],
            comment_contains="ошибка"
        )
    """
    channel = _get_channel_from_context(ctx)
    logger.info(
        f"get_event_log on channel '{channel}': start_date={start_date}, end_date={end_date}, "
        f"levels={levels}, events={events}, limit={limit}, object_description={object_description is not None}, "
        f"link={link}, data={data}, metadata_type={metadata_type}, user={user}, session={session}, "
        f"application={application}, computer={computer}, "
        f"comment_contains={comment_contains}, transaction_status={transaction_status}"
    )
    
    # Validate parameters using Pydantic model
    try:
        validated = validate_get_event_log_params(
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
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"get_event_log validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"get_event_log validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"get_event_log type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }
    
    # Create command for the queue with validated parameters
    # Use exclude_none=True to avoid sending null values to 1C
    # This prevents issues with 1C treating JSON null as Null (not Неопределено)
    params_dict = validated.model_dump(exclude_none=True)
    result = await _execute_1c_command("get_event_log", params_dict, channel=channel)
    
    return result


@mcp.tool()
async def get_object_by_link(ctx: Context, link: str) -> Dict[str, Any]:
    """
    Get 1C object data by navigation link.
    
    This tool retrieves complete object data from the 1C:Enterprise database
    using a navigation link. The link format is e1cib/data/Type.Name?ref=HexGUID.
    Results are returned as JSON or TOON format.
    
    Args:
        ctx: MCP Context (injected automatically)
        link: Navigation link in format e1cib/data/Type.Name?ref=HexGUID
              (e.g., "e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5")
              
    Returns:
        Dictionary with:
        - success: Boolean indicating if the request was successful
        - data: Object data in JSON or TOON format (if successful), containing:
            - _type: Full metadata type name
            - _presentation: String representation of the object
            - Standard attributes (Код, Наименование, Дата, Номер if applicable)
            - Custom attributes defined in metadata
            - Tabular sections with all rows
        - error: Error message (if failed)
        
    Example:
        get_object_by_link(
            link="e1cib/data/Справочник.Контрагенты?ref=80c6cc1a7e58902811ebcda8cb07c0f5"
        )
    """
    channel = _get_channel_from_context(ctx)
    logger.info(f"get_object_by_link on channel '{channel}': link={link[:50]}..." if len(link) > 50 else f"get_object_by_link on channel '{channel}': link={link}")
    
    # Validate parameters using Pydantic model
    # Validates: Requirement 1.2 - validate link against navigation link format
    try:
        validated = validate_get_object_by_link_params(link=link)
    except ValidationError as e:
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"get_object_by_link validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"get_object_by_link validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"get_object_by_link type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }
    
    # Execute command on 1C client
    # Validates: Requirement 1.4 - return object data in JSON format
    result = await _execute_1c_command("get_object_by_link", {
        "link": validated.link
    }, channel=channel)
    
    return result


@mcp.tool()
async def get_link_of_object(ctx: Context, object_description: Dict[str, Any]) -> Dict[str, Any]:
    """
    Generate a 1C navigation link from an object description.
    
    This tool converts an object description (returned by execute_query) into a clickable
    navigation link that can be used to open the object directly in 1C.
    
    Use this tool to provide users with clickable links to 1C objects after finding them
    with execute_query.
    
    Args:
        ctx: MCP Context (injected automatically)
        object_description: Object description from execute_query results with fields:
                          - _objectRef: must be true
                          - УникальныйИдентификатор: UUID string
                          - ТипОбъекта: object type (e.g., "СправочникСсылка.Контрагенты")
                          - Представление: string representation (optional)
                          
    Returns:
        Dictionary with:
        - success: Boolean indicating if the request was successful
        - data: Navigation link string (e.g., "e1cib/data/Справочник.Контрагенты?ref=...")
        - error: Error message (if failed)
        
    Example:
        # After finding an object with execute_query
        result = execute_query("SELECT Ref FROM Catalog.Customers WHERE Code = '001'")
        # result.data[0]["Ref"] contains object description
        
        # Generate link
        link_result = get_link_of_object(result.data[0]["Ref"])
        # link_result.link = "e1cib/data/Справочник.Контрагенты?ref=80c6cc1a..."
    """
    channel = _get_channel_from_context(ctx)
    logger.info(f"get_link_of_object on channel '{channel}'")
    
    # Validate parameters using Pydantic model
    try:
        validated = validate_get_link_of_object_params(object_description=object_description)
    except ValidationError as e:
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"get_link_of_object validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"get_link_of_object validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"get_link_of_object type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }
    
    # Execute command on 1C client
    result = await _execute_1c_command("get_link_of_object", {
        "object_description": validated.object_description
    }, channel=channel)

    return result


@mcp.tool()
async def find_references_to_object(
    ctx: Context,
    target_object_description: Dict[str, Any],
    search_scope: List[str],
    meta_filter: Optional[Dict[str, Any]] = None,
    limit_hits: int = 200,
    limit_per_meta: int = 20,
    timeout_budget_sec: int = 30
) -> Dict[str, Any]:
    """
    Find references to a given object across specified metadata collections.

    This tool searches for references to the target object in documents, catalogs,
    and registers. It checks all fields whose type can contain a reference to the
    target object type, then queries for actual matches.

    The search is performed in two stages:
    1. Candidate discovery: metadata traversal to find fields that can hold the target type
    2. Query execution: actual queries to find objects/records referencing the target

    Args:
        ctx: MCP Context (injected automatically)
        target_object_description: Object description from execute_query results with fields:
                                  - _objectRef: must be true
                                  - УникальныйИдентификатор: UUID string
                                  - ТипОбъекта: object type (e.g., "СправочникСсылка.Контрагенты")
                                  - Представление: string representation (optional)
        search_scope: List of search scopes to check. Valid values:
                     "documents", "catalogs", "information_registers",
                     "accumulation_registers", "accounting_registers", "calculation_registers".
        meta_filter: Optional metadata filter dict with fields:
                    - names: list of exact metadata object names (e.g., ["Документ.РеализацияТоваровУслуг"])
                    - name_mask: search mask for name/synonym (case-insensitive)
                    If names is provided, name_mask is ignored.
        limit_hits: Maximum total number of hits (default: 200, max: 10000).
        limit_per_meta: Maximum hits per metadata object (default: 20, max: 1000).
        timeout_budget_sec: Time budget in seconds for the search (default: 30, min: 5, max: 300).

    Returns:
        Dictionary with:
        - success: Boolean indicating if the request was successful
        - data: Object containing:
            - hits: Array of found references, each with:
                - found_in_meta: metadata object name (e.g., "Документ.РеализацияТоваровУслуг")
                - found_in_object: object_description of the owner (for documents/catalogs) or null (for registers)
                - record_key: dimension values (for registers only)
                - path: field path (e.g., "Контрагент" or "Товары.Номенклатура")
                - match_kind: field type (attribute, tabular_section, dimension, resource, requisite)
                - note: human-readable description
            - total_hits: number of hits found
            - candidates_checked: number of candidate fields checked
            - timeout_exceeded: true if time budget was exhausted
            - skipped_names: names from meta_filter.names that were skipped
        - error: Error message (if failed)

    Example:
        find_references_to_object(
            target_object_description={
                "_objectRef": True,
                "УникальныйИдентификатор": "ba7e5a3d-1234-5678-9abc-def012345678",
                "ТипОбъекта": "СправочникСсылка.Контрагенты",
                "Представление": "ООО Рога и Копыта"
            },
            search_scope=["documents"],
            limit_hits=10
        )
    """
    channel = _get_channel_from_context(ctx)
    logger.info(
        f"find_references_to_object on channel '{channel}': "
        f"search_scope={search_scope}, meta_filter={meta_filter}, "
        f"limit_hits={limit_hits}, limit_per_meta={limit_per_meta}, "
        f"timeout_budget_sec={timeout_budget_sec}"
    )

    # Validate parameters using Pydantic model
    try:
        validated = validate_find_references_to_object_params(
            target_object_description=target_object_description,
            search_scope=search_scope,
            meta_filter=meta_filter,
            limit_hits=limit_hits,
            limit_per_meta=limit_per_meta,
            timeout_budget_sec=timeout_budget_sec
        )
    except ValidationError as e:
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"find_references_to_object validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"find_references_to_object validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"find_references_to_object type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }

    # Create command for the queue with validated parameters
    # Use exclude_none=True to avoid sending null values to 1C
    params_dict = validated.model_dump(exclude_none=True)
    result = await _execute_1c_command("find_references_to_object", params_dict, channel=channel)

    return result


@mcp.tool()
async def get_access_rights(
    ctx: Context,
    metadata_object: str,
    user_name: Optional[str] = None,
    rights_filter: Optional[List[str]] = None,
    roles_filter: Optional[List[str]] = None
) -> Dict[str, Any]:
    """
    Get role permissions for a metadata object and optionally effective rights for a user.

    This tool retrieves information about what rights each role grants for the specified
    metadata object. If a user name is provided, it also calculates the effective rights
    for that user (sum of all their roles).

    IMPORTANT limitations:
    - effective_rights is "rights by sum of roles", NOT a guarantee of real access
    - RLS (Row Level Security) is NOT taken into account
    - Contextual restrictions (by organizations, departments) are NOT taken into account
    - This is a check of "what is allowed by roles", not "what the user will actually see"

    Runtime requirements:
    - Admin rights are required: AccessRight() with 3rd parameter (Role/User) requires admin rights
    - Privileged mode is forbidden: if enabled, AccessRight() always returns True, result is meaningless

    Args:
        ctx: MCP Context (injected automatically)
        metadata_object: Full metadata object name (e.g., "Справочник.Контрагенты", "Документ.РеализацияТоваровУслуг")
        user_name: Optional user name for effective rights calculation (case-insensitive search)
        rights_filter: Optional list of rights to show in result (default: all applicable rights for type)
        roles_filter: Optional list of roles to show (default: all roles with rights)

    Returns:
        Dictionary with:
        - success: Boolean indicating if the request was successful
        - data: Object containing:
            - metadata_object: full metadata object name
            - metadata_type: object type (e.g., "Справочник", "Документ", "РегистрСведений")
            - applicable_rights: array of applicable rights for this object type
            - roles: array of roles with their rights, sorted by name
            - total_roles: number of roles in result (after roles_filter)
            - roles_with_rights: number of roles that have at least one right (after rights_filter)
            - user: (if user_name provided) object with:
                - name: user name
                - full_name: user's full name (optional, may not be available)
                - roles: array of role names assigned to the user
                - effective_rights: object with right names as keys and boolean values
        - error: Error message (if failed)

    Example:
        # Get all role permissions for a catalog
        get_access_rights(metadata_object="Справочник.Контрагенты")

        # Get specific rights for a user
        get_access_rights(
            metadata_object="Справочник.Контрагенты",
            user_name="Иванов",
            rights_filter=["Чтение", "Изменение"]
        )
    """
    channel = _get_channel_from_context(ctx)
    logger.info(
        f"get_access_rights on channel '{channel}': "
        f"metadata_object={metadata_object}, user_name={user_name}, "
        f"rights_filter={rights_filter}, roles_filter={roles_filter}"
    )

    # Validate parameters using Pydantic model
    try:
        validated = validate_get_access_rights_params(
            metadata_object=metadata_object,
            user_name=user_name,
            rights_filter=rights_filter,
            roles_filter=roles_filter
        )
    except ValidationError as e:
        error_msg = e.errors()[0]['msg'] if e.errors() else str(e)
        logger.warning(f"get_access_rights validation failed: {error_msg}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {error_msg} / Parameter validation failed: {error_msg}"
        }
    except ValueError as e:
        logger.warning(f"get_access_rights validation failed: {e}")
        return {
            "success": False,
            "error": f"Ошибка валидации параметров: {str(e)} / Parameter validation failed: {str(e)}"
        }
    except TypeError as e:
        logger.warning(f"get_access_rights type error: {e}")
        return {
            "success": False,
            "error": f"Ошибка типа данных: {str(e)} / Type error: {str(e)}"
        }

    # Create command for the queue with validated parameters
    params_dict = validated.model_dump(exclude_none=True)
    result = await _execute_1c_command("get_access_rights", params_dict, channel=channel)

    return result


def get_mcp_server() -> FastMCP:
    """
    Get the MCP server instance.
    
    Returns:
        The FastMCP server instance configured for 1C MCP Toolkit Proxy.
    """
    return mcp
