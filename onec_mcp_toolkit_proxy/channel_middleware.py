"""
Channel Middleware for extracting and managing channel IDs from MCP requests.

This module provides ASGI middleware for intercepting channel IDs from URL
query parameters and binding them to MCP sessions.

Works for Streamable HTTP transport where session_id is passed in headers.

Validates: Requirements 1.1, 1.2, 1.4, 1.5
"""

from urllib.parse import parse_qs
from starlette.types import ASGIApp, Receive, Scope, Send
from starlette.requests import Request
import logging

from .channel_registry import channel_registry, ChannelRegistry, DEFAULT_CHANNEL

logger = logging.getLogger(__name__)

MCP_SESSION_ID_HEADER = "mcp-session-id"


class ChannelMiddleware:
    """
    ASGI Middleware for extracting channel_id from URL and binding to MCP session.
    
    Works for Streamable HTTP transport:
    - session_id is passed in header mcp-session-id
    - channel is passed in query params
    
    Registration happens ONLY when creating a new session,
    to avoid overwriting channel on subsequent requests.
    
    Validates: Requirements 1.1, 1.2, 1.4, 1.5
    - 1.1: Channel ID passed via query parameter URL
    - 1.2: Works for Streamable HTTP transport
    - 1.4: Default "default" channel when parameter is missing
    - 1.5: Channel preserved for entire MCP session
    """
    
    def __init__(self, app: ASGIApp):
        self.app = app
    
    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        
        # Extract channel from query string and validate
        query_string = scope.get("query_string", b"").decode()
        params = parse_qs(query_string)
        raw_channel = params.get("channel", [DEFAULT_CHANNEL])[0]
        query_channel = ChannelRegistry.validate_channel_id(raw_channel)
        
        # Check if session_id already exists in request headers
        request = Request(scope, receive)
        request_session_id = request.headers.get(MCP_SESSION_ID_HEADER)
        
        # Determine effective channel
        if request_session_id and channel_registry.has_session(request_session_id):
            # Existing session - use saved channel
            effective_channel = channel_registry.get_channel(request_session_id)
        else:
            # New session or first request - use channel from query
            effective_channel = query_channel
        
        # Save effective channel in scope for access in handlers
        scope["channel"] = effective_channel
        
        # Flag: this is a new session (no session_id in request)
        is_new_session = request_session_id is None
        
        # Wrapper to intercept response headers
        async def send_wrapper(message):
            if message["type"] == "http.response.start" and is_new_session:
                # Register ONLY for new sessions
                headers = dict(message.get("headers", []))
                session_id_bytes = headers.get(b"mcp-session-id")
                
                if session_id_bytes:
                    session_id = session_id_bytes.decode()
                    # Register with effective_channel (not query_channel!)
                    channel_registry.register(session_id, effective_channel)
            
            await send(message)
        
        await self.app(scope, receive, send_wrapper)
