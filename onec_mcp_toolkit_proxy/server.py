"""
Starlette HTTP server for 1C MCP Toolkit Proxy.

Provides endpoints for:
- /mcp - MCP HTTP Streamable Transport for AI agents
- /1c/poll - Long polling for 1C processing (get commands)
- /1c/result - Receive results from 1C processing
- /health - Health check for Docker

Validates: Requirements 1.4, 2.1, 3.1, 4.1, 5.3, 5.4, 6.2, 6.3, 6.4
"""

import asyncio
import json
import logging
import math
import os
import time
from contextlib import asynccontextmanager, suppress
from typing import Any, Optional

import anyio
from starlette.applications import Starlette
from starlette.responses import JSONResponse, Response
from starlette.routing import Route
from starlette.requests import Request
from starlette.middleware import Middleware
from starlette.middleware.base import BaseHTTPMiddleware
from pydantic import BaseModel, ValidationError

from mcp.server.streamable_http import (
    CONTENT_TYPE_SSE,
    LAST_EVENT_ID_HEADER,
    MCP_PROTOCOL_VERSION_HEADER,
    MCP_SESSION_ID_HEADER,
)
from . import __version__
from .command_queue import command_queue, channel_command_queue
from .config import settings
from .mcp_handler import get_mcp_server
from .channel_registry import channel_registry, ChannelRegistry, DEFAULT_CHANNEL, CHANNEL_ID_PATTERN
from .channel_middleware import ChannelMiddleware
from .mcp_request_ids import McpRequestIdMiddleware
from .query_encoding_middleware import QueryEncodingMiddleware
from .channel_sse_transport import ChannelAwareSseTransport
from .rest_api import (
    execute_query_handler,
    execute_code_handler,
    get_metadata_handler,
    get_event_log_handler,
    get_object_by_link_handler,
    get_link_of_object_handler,
    find_references_to_object_handler,
    get_access_rights_handler,
    get_bsl_syntax_help_handler,
    submit_for_deanonymization_handler,
    deanonymize_handler,
    get_screenshot_handler,
    restart_1c_session_handler,
    close_1c_session_handler,
)
from .anonymizer.registry import AnonymizerRegistry
from .consumer_registry import consumer_registry, ConsumerChanged
from .host_api import register_host, poll_host, host_states, release_host, host_of

# Configure logging
logging.basicConfig(
    level=getattr(logging, settings.log_level.upper()),
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Get the MCP server instance
mcp_server = get_mcp_server()

# Create transports
streamable_app = McpRequestIdMiddleware(mcp_server.streamable_http_app())
legacy_sse_transport = ChannelAwareSseTransport("/mcp/message")


def _extract_headers(scope: dict) -> dict:
    headers = {}
    for key, value in scope.get("headers", []):
        headers[key.decode("latin1").lower()] = value.decode("latin1")
    return headers


def _wants_sse(headers: dict) -> bool:
    return CONTENT_TYPE_SSE in headers.get("accept", "")


def _is_streamable_get(headers: dict) -> bool:
    # Streamable HTTP GET carries MCP session/protocol headers (and/or Last-Event-ID)
    return any(
        header in headers
        for header in (
            MCP_SESSION_ID_HEADER,
            MCP_PROTOCOL_VERSION_HEADER,
            LAST_EVENT_ID_HEADER,
        )
    )


async def legacy_sse_asgi(scope, receive, send) -> None:
    async with legacy_sse_transport.connect_sse(scope, receive, send) as streams:
        await mcp_server._mcp_server.run(
            streams[0],
            streams[1],
            mcp_server._mcp_server.create_initialization_options(),
        )


class LegacySseMessageApp:
    """ASGI app for legacy SSE POST messages."""

    async def __call__(self, scope, receive, send) -> None:
        await legacy_sse_transport.handle_post_message(scope, receive, send)


class McpUnifiedApp:
    """ASGI app that multiplexes Streamable HTTP and legacy SSE on /mcp."""

    async def __call__(self, scope, receive, send) -> None:
        method = scope.get("method", "").upper()
        headers = _extract_headers(scope)

        # Safe raw-body logging for MCP requests in DEBUG mode.
        # We do not pre-read or replay the body. Instead, we wrap `receive`
        # and log payload chunks as they are consumed by downstream handlers.
        if settings.log_level.upper() == "DEBUG" and method in ("POST", "DELETE"):
            body_parts: list[bytes] = []
            body_logged = False
            original_receive = receive

            async def logging_receive():
                nonlocal body_logged
                message = await original_receive()

                if message.get("type") == "http.request":
                    body_chunk = message.get("body", b"")
                    if body_chunk:
                        body_parts.append(body_chunk)

                    if not message.get("more_body", False) and not body_logged:
                        body_logged = True
                        raw_bytes = b"".join(body_parts)
                        try:
                            raw_text = raw_bytes.decode("utf-8")
                            logger.info(
                                "MCP Raw Request Body (%d bytes): %s",
                                len(raw_bytes),
                                raw_text,
                            )
                        except UnicodeDecodeError:
                            logger.info(
                                "MCP Raw Request Body (%d bytes, non-UTF-8): %r",
                                len(raw_bytes),
                                raw_bytes,
                            )

                return message

            receive = logging_receive

        if method == "GET" and _wants_sse(headers):
            if _is_streamable_get(headers):
                logger.info("Routing to Streamable HTTP GET (new)")
                await streamable_app(scope, receive, send)
            else:
                logger.info("Routing to Legacy SSE GET (old)")
                await legacy_sse_asgi(scope, receive, send)
            return

        logger.info(f"Routing to Streamable HTTP (new): {method}")
        await streamable_app(scope, receive, send)


class MCPLoggingMiddleware(BaseHTTPMiddleware):
    """Middleware to log all MCP endpoint requests."""
    
    async def dispatch(self, request: Request, call_next):
        if request.url.path.startswith("/mcp"):
            logger.info(f"MCP Request: {request.method} {request.url.path}")
            logger.info(f"Headers: {dict(request.headers)}")
        
        response = await call_next(request)
        
        if request.url.path.startswith("/mcp"):
            logger.info(f"MCP Response: {response.status_code}")
        
        return response


COMMAND_CLEANUP_INTERVAL = 60.0


async def cleanup_abandoned_commands() -> None:
    """Safety net for commands left without an active result waiter."""
    max_age = max(float(settings.timeout) * 2, 300.0)
    while True:
        await asyncio.sleep(COMMAND_CLEANUP_INTERVAL)
        try:
            removed = await channel_command_queue.cleanup_expired(max_age)
            if removed:
                logger.warning("Removed %d abandoned commands", removed)
        except Exception:
            logger.exception("Failed to clean up abandoned commands")


@asynccontextmanager
async def lifespan(app: Starlette):
    """Lifespan context manager for startup and shutdown events."""
    # Startup
    logger.info(f"1C MCP Toolkit Proxy v{__version__} starting on port {settings.port}")
    logger.info(f"Log level: {settings.log_level}")
    logger.info(f"Command timeout: {settings.timeout}s")
    logger.info("MCP server available at /mcp endpoint")
    
    # Start the MCP session manager
    async with mcp_server.session_manager.run():
        cleanup_task = asyncio.create_task(
            cleanup_abandoned_commands(), name="command-queue-cleanup"
        )
        try:
            yield
        finally:
            cleanup_task.cancel()
            with anyio.CancelScope(shield=True):
                with suppress(asyncio.CancelledError):
                    await cleanup_task
    
    # Shutdown
    logger.info("1C MCP Toolkit Proxy shutting down")


# Pydantic models for request/response validation
class CommandResult(BaseModel):
    """Result from 1C processing."""
    id: str
    success: bool
    base: Optional[str] = None
    data: Optional[Any] = None
    schema: Optional[Any] = None
    error: Optional[str] = None
    count: Optional[int] = None
    # Optional metadata fields for list/pagination responses (e.g., get_metadata list mode)
    truncated: Optional[bool] = None
    limit: Optional[int] = None
    returned: Optional[int] = None
    offset: Optional[int] = None
    has_more: Optional[bool] = None
    next_offset: Optional[int] = None
    configuration: Optional[Any] = None
    extension: Optional[str] = None
    # Cursor pagination fields for get_event_log
    last_date: Optional[str] = None                  # Cursor - last record date
    next_same_second_offset: Optional[int] = None    # Accumulated offset for next page
    # submit_for_deanonymization acknowledgement field
    received: Optional[bool] = None
    # Screenshot result fields for get_screenshot
    image_base64: Optional[str] = None
    mime_type: Optional[str] = None
    window_rect: Optional[Any] = None
    grid_coords: Optional[Any] = None
    overlays: Optional[Any] = None
    overlay_failures: Optional[Any] = None
    capture_complete: Optional[Any] = None
    foreground_window: Optional[Any] = None


class HealthResponse(BaseModel):
    """Health check response."""
    status: str
    pending_commands: int
    mcp_endpoint: str


def record_legacy_activity(request: Request, consumer) -> None:
    """Legacy routing may normalize invalid channels; contact must not do so."""
    channels = request.query_params.getlist('channel')
    if len(channels) > 1:
        return
    channel = (channels[0] if channels else '') or DEFAULT_CHANNEL
    if consumer is not None and CHANNEL_ID_PATTERN.fullmatch(channel) and channel == consumer.channel:
        consumer_registry.record_activity(consumer)


async def poll_command(request: Request) -> Response:
    """
    Long polling endpoint for 1C processing to get commands.
    
    The 1C processing calls this endpoint to receive commands from MCP clients.
    If no command is available, waits up to `timeout` seconds (default from settings).
    
    Supports channel isolation - only returns commands for the specified channel.
    
    Validates: Requirement 1.4, 2.3, 3.3
    """
    # Extract and validate channel
    raw_channel = request.query_params.get("channel", DEFAULT_CHANNEL)
    channel = ChannelRegistry.validate_channel_id(raw_channel)
    timeout_param = request.query_params.get("timeout")
    try:
        poll_timeout = float(timeout_param) if timeout_param else float(settings.poll_timeout)
        if not math.isfinite(poll_timeout) or poll_timeout < 0:
            raise ValueError()
    except ValueError:
        return JSONResponse({'error': 'Invalid poll timeout'}, status_code=400)
    try:
        consumer = consumer_registry.bind(channel, 'legacy')
    except ConsumerChanged as exc:
        return JSONResponse({'error': str(exc)}, status_code=409)
    
    record_legacy_activity(request, consumer)
    
    logger.debug(f"1C poll request on channel '{channel}', timeout={poll_timeout}s")
    
    try:
        command = await channel_command_queue.get_next_command(channel, timeout=poll_timeout, consumer=consumer)
    except ConsumerChanged as exc:
        return JSONResponse({'error': str(exc)}, status_code=409)
    
    if command is None:
        logger.debug(f"No command available on channel '{channel}', returning empty response")
        return Response(content=b"", status_code=204)
    
    logger.info(f"Returning command to 1C on channel '{channel}': id={command.id}, tool={command.tool}")
    payload = command.to_dict()
    if request.query_params.get("request_state_version") == "1":
        state = channel_command_queue.command_states(channel, [command.id])[0]
        payload.update(request_state_version=1, remaining_ms=state["remaining_ms"])
    return JSONResponse(content=payload)


async def command_states(request: Request) -> JSONResponse:
    """Observe only IDs in the specified channel, without polling another command."""
    channel = ChannelRegistry.validate_channel_id(request.query_params.get("channel", DEFAULT_CHANNEL))
    raw_ids = request.query_params.get("ids", "")
    ids = raw_ids.split(",") if raw_ids else []
    if len(ids) > 16 or any(not item or len(item) > 36 for item in ids):
        return JSONResponse({"error": "At most 16 command IDs are allowed"}, status_code=400)
    consumer = consumer_registry.consumers.get(channel)
    if consumer is not None and consumer.mode == 'legacy':
        record_legacy_activity(request, consumer)
    return JSONResponse({"version": 1, "items": channel_command_queue.command_states(channel, ids)})


async def channel_status(request: Request) -> JSONResponse:
    """Observe exactly the query channel; monitoring never records 1C activity."""
    channels = request.query_params.getlist('channel')
    headers = {'Cache-Control': 'no-store'}
    if len(channels) > 1:
        return JSONResponse({'error': 'Specify channel once'}, status_code=400, headers=headers)
    channel = (channels[0] if channels else '') or DEFAULT_CHANNEL
    if not CHANNEL_ID_PATTERN.fullmatch(channel):
        return JSONResponse({'error': 'Invalid channel'}, status_code=400, headers=headers)
    now = time.monotonic()
    return JSONResponse({
        'version': 1, 'channel': channel,
        **consumer_registry.activity_status(channel, now),
        **channel_command_queue.channel_status(channel, now),
    }, headers=headers)


async def receive_result(request: Request) -> JSONResponse:
    """
    Endpoint for 1C processing to send command execution results.
    
    After executing a command, 1C processing sends the result here.
    The result is matched with the waiting MCP client request.
    
    Validates: Requirements 2.4, 3.4, 5.3, 5.4
    - 2.4: При ошибке выполнения запроса возвращается понятное сообщение об ошибке
    - 3.4: При ошибке выполнения кода возвращается сообщение об ошибке с указанием строки
    """
    # Extract channel for logging
    raw_channel = request.query_params.get("channel", DEFAULT_CHANNEL)
    channel = ChannelRegistry.validate_channel_id(raw_channel)
    
    try:
        body = await request.json()
        result = CommandResult(**body)
    except json.JSONDecodeError as e:
        logger.warning(f"JSON decode error: {e.msg} at position {e.pos}")
        return JSONResponse(
            status_code=400,
            content={
                "success": False,
                "error": f"Ошибка разбора JSON: {e.msg} в позиции {e.pos} / "
                         f"JSON parse error: {e.msg} at position {e.pos}"
            }
        )
    except ValidationError as e:
        errors = e.errors()
        error_messages = []
        for error in errors:
            loc = " -> ".join(str(l) for l in error.get("loc", []))
            msg = error.get("msg", "Unknown error")
            error_messages.append(f"{loc}: {msg}")
        
        error_detail = "; ".join(error_messages)
        logger.warning(f"JSON validation error: {error_detail}")
        
        return JSONResponse(
            status_code=422,
            content={
                "success": False,
                "error": f"Ошибка валидации JSON: {error_detail} / JSON validation error: {error_detail}",
                "details": errors
            }
        )
    
    logger.info(f"Received result from 1C on channel '{channel}': id={result.id}, success={result.success}")
    command = channel_command_queue.get_command(result.id)
    if command is not None and command.consumer is not None and command.consumer.mode == 'server_host':
        try:
            if host_of(request) is not command.consumer:
                raise ConsumerChanged('Result owner mismatch')
        except (ConsumerChanged, ValueError):
            return JSONResponse({'error': 'Result owner mismatch'}, status_code=409)
    elif request.headers.get('X-MCP-Host-Lease'):
        return JSONResponse({'error': 'Command no longer belongs to this host'}, status_code=409)
    
    if result.error:
        logger.warning(f"Command {result.id} failed: {result.error}")
    
    # Convert Pydantic model to dict for storage
    result_data = {
        "success": result.success,
        "data": result.data,
        "error": result.error
    }
    # Absence means opt-out/old consumer; explicit null means unknown source.
    if "base" in result.model_fields_set:
        result_data["base"] = result.base
    if result.success and result.schema is not None:
        result_data["schema"] = result.schema

    # Pass through whitelisted optional metadata fields (do not forward arbitrary extras)
    optional_meta_fields = (
        "truncated",
        "limit",
        "returned",
        "count",
        "offset",
        "has_more",
        "next_offset",
        "configuration",
        "extension",
        "last_date",                 # Cursor for get_event_log pagination
        "next_same_second_offset",   # Accumulated offset for compound cursor
        "received",                  # submit_for_deanonymization acknowledgement
        "image_base64",              # get_screenshot: base64-encoded PNG
        "mime_type",                 # get_screenshot: MIME type of the image
        "window_rect",               # get_screenshot: captured window rectangle
        "grid_coords",               # get_screenshot: grid line coordinates
        "overlays",
        "overlay_failures",
        "capture_complete",
        "foreground_window",
    )
    for key in optional_meta_fields:
        value = getattr(result, key, None)
        if value is not None:
            result_data[key] = value

    # Use channel_command_queue - it finds channel via index
    success = await channel_command_queue.set_result(result.id, result_data)
    
    if not success:
        logger.warning(f"Unknown command id: {result.id}")
        return JSONResponse(
            status_code=404,
            content={
                "success": False,
                "error": f"Команда {result.id} не найдена или уже выполнена / "
                         f"Command {result.id} not found or already completed"
            }
        )

    # A legacy result is routed by ID. Only a matching query channel may mark
    # contact, and only for the current owner; do not change result routing.
    if command is not None and channel_command_queue._command_index.get(result.id) == channel:
        consumer = command.consumer
        if consumer is None:
            consumer = consumer_registry.consumers.get(channel)
            if consumer is not None and consumer.mode != 'legacy':
                consumer = None
        if consumer is not None and consumer.mode == 'server_host':
            consumer_registry.record_activity(consumer)
        else:
            record_legacy_activity(request, consumer)

    logger.debug(f"Result stored for command {result.id}")
    return JSONResponse(content={"status": "ok"})


async def health_check(request: Request) -> JSONResponse:
    """
    Health check endpoint for Docker and monitoring.
    
    Returns:
        Health status, total pending commands, and MCP endpoint info.
        Channel IDs are not exposed by default to avoid leaking routing identifiers.
        
    Validates: Requirements 5.1, 5.2
    - 5.1: Shows number of active channels
    - 5.2: Shows pending commands by channel
    """
    # Get channel statistics
    channel_stats = await channel_command_queue.get_stats()
    active_sessions = channel_registry.get_active_channels()
    
    # Total pending commands (for backward compatibility)
    total_pending = sum(channel_stats.values())
    pending_channels_count = sum(1 for count in channel_stats.values() if count > 0)
    active_sessions_count = sum(active_sessions.values())
    
    include_channel_details = os.getenv("HEALTH_INCLUDE_CHANNEL_DETAILS", "false").lower() in (
        "true",
        "1",
        "yes",
    )
    
    logger.debug(f"Health check: pending_commands={total_pending}, channels={len(channel_stats)}")
    
    response_data = {
        "status": "healthy",
        # Backward compatibility - keep as number
        "pending_commands": total_pending,
        # Aggregated channel stats (do not expose channel IDs)
        "pending_channels_count": pending_channels_count,
        "active_channels_count": channel_command_queue.get_active_channels_count(),
        "active_sessions_count": active_sessions_count,
        "mcp_endpoint": "/mcp"
    }
    
    # Optional: include details for debugging/monitoring in trusted environments
    if include_channel_details:
        response_data["pending_commands_by_channel"] = channel_stats
        response_data["active_sessions_by_channel"] = active_sessions
    
    return JSONResponse(content=response_data)


async def anonymization_mappings(request: Request) -> JSONResponse:
    """
    Return anonymization token mappings for the specified channel.
    Used by 1C processing to display the mapping table.
    """
    raw_channel = request.query_params.get("channel", DEFAULT_CHANNEL)
    channel = ChannelRegistry.validate_channel_id(raw_channel)

    owner = consumer_registry.consumers.get(channel)
    host_meta = ({'generation': owner.generation, 'finalization_owner': 'proxy'}
                 if owner and not owner.retired and owner.mode == 'server_host' else {})
    if not settings.anonymization_enabled:
        return JSONResponse(content={
            "enabled": False, "mappings": [], "stats": {}, "count": 0, **host_meta
        })
    anon = AnonymizerRegistry.get_if_exists(channel)
    if anon is None:
        return JSONResponse(content={
            "enabled": True, "mappings": [], "stats": {}, "count": 0, **host_meta
        })

    mappings = anon.mapper.get_mappings()
    stats = anon.mapper.get_stats()

    return JSONResponse(content={
        "enabled": True, "mappings": mappings, "stats": stats, "count": len(mappings), **host_meta,
    })


async def mcp_debug(request: Request) -> JSONResponse:
    """
    Debug endpoint to check MCP requests.
    Logs all incoming requests to /mcp for debugging.
    """
    logger.info(f"MCP Debug: {request.method} {request.url.path}")
    logger.info(f"Headers: {dict(request.headers)}")
    
    if request.method == "POST":
        try:
            body = await request.body()
            logger.info(f"Body: {body.decode('utf-8')[:500]}")
        except Exception as e:
            logger.error(f"Error reading body: {e}")
    
    return JSONResponse(
        status_code=200,
        content={
            "message": "MCP endpoint is working",
            "method": request.method,
            "path": request.url.path,
            "note": "This is a debug response. The actual MCP app should handle this."
        }
    )


# Starlette application with routes
routes = [
    Route("/mcp", McpUnifiedApp(), methods=["GET", "POST", "DELETE"]),
    Route("/mcp/message", LegacySseMessageApp(), methods=["POST"]),
    Route("/1c/host/register", register_host, methods=["POST"]),
    Route("/1c/host/poll", poll_host, methods=["GET"]),
    Route("/1c/host/status", host_states, methods=["GET"]),
    Route("/1c/host/release", release_host, methods=["POST"]),
    Route("/1c/poll", poll_command, methods=["GET"]),
    Route("/1c/commands/status", command_states, methods=["GET"]),
    Route("/1c/status", channel_status, methods=["GET"]),
    Route("/1c/result", receive_result, methods=["POST"]),
    Route("/1c/anonymization_mappings", anonymization_mappings, methods=["GET"]),
    Route("/health", health_check, methods=["GET"]),
    # REST API routes
    Route("/api/deanonymize", deanonymize_handler, methods=["GET", "POST"]),
    Route("/api/execute_query", execute_query_handler, methods=["POST"]),
    Route("/api/execute_code", execute_code_handler, methods=["POST"]),
    Route("/api/get_metadata", get_metadata_handler, methods=["GET", "POST"]),
    Route("/api/get_event_log", get_event_log_handler, methods=["POST"]),
    Route("/api/get_object_by_link", get_object_by_link_handler, methods=["POST"]),
    Route("/api/get_link_of_object", get_link_of_object_handler, methods=["POST"]),
    Route("/api/find_references_to_object", find_references_to_object_handler, methods=["POST"]),
    Route("/api/get_access_rights", get_access_rights_handler, methods=["POST"]),
    Route("/api/get_bsl_syntax_help", get_bsl_syntax_help_handler, methods=["POST"]),
    Route("/api/submit_for_deanonymization", submit_for_deanonymization_handler, methods=["POST"]),
    Route("/api/get_screenshot", get_screenshot_handler, methods=["POST"]),
    Route("/api/restart_1c_session", restart_1c_session_handler, methods=["POST"]),
    Route("/api/close_1c_session", close_1c_session_handler, methods=["POST"]),
]

app = Starlette(
    debug=False,
    routes=routes,
    lifespan=lifespan,
    middleware=[
        Middleware(QueryEncodingMiddleware),
        Middleware(ChannelMiddleware),
        Middleware(MCPLoggingMiddleware)
    ]
)
