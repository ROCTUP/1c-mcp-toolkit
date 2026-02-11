"""
Starlette HTTP server for 1C MCP Toolkit Proxy.

Provides endpoints for:
- /mcp - MCP HTTP Streamable Transport for AI agents
- /1c/poll - Long polling for 1C processing (get commands)
- /1c/result - Receive results from 1C processing
- /health - Health check for Docker

Validates: Requirements 1.4, 2.1, 3.1, 4.1, 5.3, 5.4, 6.2, 6.3, 6.4
"""

import json
import logging
import os
from contextlib import asynccontextmanager
from typing import Any, Optional

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
from .command_queue import command_queue, channel_command_queue
from .config import settings
from .mcp_handler import get_mcp_server
from .channel_registry import channel_registry, ChannelRegistry, DEFAULT_CHANNEL
from .channel_middleware import ChannelMiddleware
from .query_encoding_middleware import QueryEncodingMiddleware
from .channel_sse_transport import ChannelAwareSseTransport
from .rest_api import execute_query_handler, execute_code_handler, get_metadata_handler, get_event_log_handler, get_object_by_link_handler, get_link_of_object_handler, find_references_to_object_handler, get_access_rights_handler

# Configure logging
logging.basicConfig(
    level=getattr(logging, settings.log_level.upper()),
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Get the MCP server instance
mcp_server = get_mcp_server()

# Create transports
streamable_app = mcp_server.streamable_http_app()
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


@asynccontextmanager
async def lifespan(app: Starlette):
    """Lifespan context manager for startup and shutdown events."""
    # Startup
    logger.info(f"1C MCP Toolkit Proxy starting on port {settings.port}")
    logger.info(f"Log level: {settings.log_level}")
    logger.info(f"Command timeout: {settings.timeout}s")
    logger.info("MCP server available at /mcp endpoint")
    
    # Start the MCP session manager
    async with mcp_server.session_manager.run():
        yield
    
    # Shutdown
    logger.info("1C MCP Toolkit Proxy shutting down")


# Pydantic models for request/response validation
class CommandResult(BaseModel):
    """Result from 1C processing."""
    id: str
    success: bool
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


class HealthResponse(BaseModel):
    """Health check response."""
    status: str
    pending_commands: int
    mcp_endpoint: str


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
    poll_timeout = float(timeout_param) if timeout_param else float(settings.poll_timeout)
    
    logger.debug(f"1C poll request on channel '{channel}', timeout={poll_timeout}s")
    
    command = await channel_command_queue.get_next_command(channel, timeout=poll_timeout)
    
    if command is None:
        logger.debug(f"No command available on channel '{channel}', returning empty response")
        return Response(content=b"", status_code=204)
    
    logger.info(f"Returning command to 1C on channel '{channel}': id={command.id}, tool={command.tool}")
    return JSONResponse(content=command.to_dict())


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
    
    if result.error:
        logger.warning(f"Command {result.id} failed: {result.error}")
    
    # Convert Pydantic model to dict for storage
    result_data = {
        "success": result.success,
        "data": result.data,
        "error": result.error
    }
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
    Route("/1c/poll", poll_command, methods=["GET"]),
    Route("/1c/result", receive_result, methods=["POST"]),
    Route("/health", health_check, methods=["GET"]),
    # REST API routes
    Route("/api/execute_query", execute_query_handler, methods=["POST"]),
    Route("/api/execute_code", execute_code_handler, methods=["POST"]),
    Route("/api/get_metadata", get_metadata_handler, methods=["GET", "POST"]),
    Route("/api/get_event_log", get_event_log_handler, methods=["POST"]),
    Route("/api/get_object_by_link", get_object_by_link_handler, methods=["POST"]),
    Route("/api/get_link_of_object", get_link_of_object_handler, methods=["POST"]),
    Route("/api/find_references_to_object", find_references_to_object_handler, methods=["POST"]),
    Route("/api/get_access_rights", get_access_rights_handler, methods=["POST"]),
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
