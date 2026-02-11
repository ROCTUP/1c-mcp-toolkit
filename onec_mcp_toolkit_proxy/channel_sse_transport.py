"""
Channel-aware SSE Transport for legacy MCP clients.

This module provides SSE transport with channel support for legacy MCP clients
that use the SSE protocol instead of Streamable HTTP.

Validates: Requirement 1.3 (Channel ID works for SSE transport)
"""

from urllib.parse import parse_qs, quote
from uuid import UUID, uuid4
from contextlib import asynccontextmanager
from typing import Any
import logging
import anyio
from anyio.streams.memory import MemoryObjectReceiveStream, MemoryObjectSendStream
from pydantic import ValidationError
from sse_starlette import EventSourceResponse
from starlette.requests import Request
from starlette.responses import Response
from starlette.types import Receive, Scope, Send

import mcp.types as types
from mcp.shared.message import ServerMessageMetadata, SessionMessage

from .channel_registry import channel_registry, ChannelRegistry, DEFAULT_CHANNEL

logger = logging.getLogger(__name__)


class ChannelAwareSseTransport:
    """
    SSE transport with channel support.
    
    Full implementation based on SseServerTransport from MCP SDK,
    with additions:
    - Extract channel from query params of GET request
    - Register session_id → channel when creating session
    - Inject channel into endpoint URL for client
    
    Stream types match SDK:
    - read_stream: MemoryObjectReceiveStream[SessionMessage | Exception]
    - write_stream: MemoryObjectSendStream[SessionMessage]
    
    Validates: Requirement 1.3
    """
    
    _endpoint: str
    _read_stream_writers: dict[UUID, MemoryObjectSendStream[SessionMessage | Exception]]
    
    def __init__(self, endpoint: str):
        """
        Args:
            endpoint: Relative path for POST messages (e.g., "/mcp/message")
        """
        if not endpoint.startswith("/"):
            endpoint = "/" + endpoint
        self._endpoint = endpoint
        self._read_stream_writers = {}
    
    @asynccontextmanager
    async def connect_sse(self, scope: Scope, receive: Receive, send: Send):
        """
        Establish SSE connection with channel registration.
        
        Yields:
            Tuple[MemoryObjectReceiveStream[SessionMessage | Exception], 
                  MemoryObjectSendStream[SessionMessage]]
        """
        if scope["type"] != "http":
            raise ValueError("connect_sse can only handle HTTP requests")
        
        # Extract channel from query params of GET request
        query_string = scope.get("query_string", b"").decode()
        params = parse_qs(query_string)
        raw_channel = params.get("channel", [DEFAULT_CHANNEL])[0]
        channel = ChannelRegistry.validate_channel_id(raw_channel)
        
        # Save channel in scope for access in handlers
        scope["channel"] = channel
        
        # Create streams with correct types (as in SDK)
        read_stream: MemoryObjectReceiveStream[SessionMessage | Exception]
        read_stream_writer: MemoryObjectSendStream[SessionMessage | Exception]
        write_stream: MemoryObjectSendStream[SessionMessage]
        write_stream_reader: MemoryObjectReceiveStream[SessionMessage]
        
        read_stream_writer, read_stream = anyio.create_memory_object_stream(0)
        write_stream, write_stream_reader = anyio.create_memory_object_stream(0)
        
        # Generate session_id as UUID object
        session_id = uuid4()
        self._read_stream_writers[session_id] = read_stream_writer
        
        # Register session_id.hex → channel
        channel_registry.register(session_id.hex, channel)
        logger.info(f"SSE session {session_id.hex[:8]}... registered to channel '{channel}'")
        
        # Form endpoint URL with channel (injection!)
        root_path = scope.get("root_path", "")
        full_message_path = root_path.rstrip("/") + self._endpoint
        
        # Add channel to endpoint URL
        client_post_uri = f"{quote(full_message_path)}?session_id={session_id.hex}"
        if channel != DEFAULT_CHANNEL:
            client_post_uri += f"&channel={channel}"
        
        # SSE stream for EventSourceResponse
        sse_stream_writer, sse_stream_reader = anyio.create_memory_object_stream[dict[str, Any]](0)
        
        async def sse_writer():
            """Send SSE events to client."""
            async with sse_stream_writer, write_stream_reader:
                # Send endpoint with channel to client
                await sse_stream_writer.send({"event": "endpoint", "data": client_post_uri})
                logger.debug(f"Sent endpoint event: {client_post_uri}")
                
                # Forward messages from write_stream
                async for session_message in write_stream_reader:
                    # Serialize SessionMessage to JSON (as in SDK)
                    await sse_stream_writer.send({
                        "event": "message",
                        "data": session_message.message.model_dump_json(
                            by_alias=True, 
                            exclude_none=True
                        ),
                    })
        
        async with anyio.create_task_group() as tg:
            async def response_wrapper(scope: Scope, receive: Receive, send: Send):
                """EventSourceResponse + cleanup on disconnect."""
                await EventSourceResponse(
                    content=sse_stream_reader, 
                    data_sender_callable=sse_writer
                )(scope, receive, send)
                await read_stream_writer.aclose()
                await write_stream_reader.aclose()
                logger.debug(f"Client session disconnected {session_id.hex[:8]}...")
            
            tg.start_soon(response_wrapper, scope, receive, send)
            
            try:
                yield (read_stream, write_stream)
            finally:
                # Cleanup
                self._read_stream_writers.pop(session_id, None)
                channel_registry.unregister(session_id.hex)
    
    async def handle_post_message(self, scope: Scope, receive: Receive, send: Send) -> None:
        """
        Handle POST requests from client.
        
        Creates SessionMessage with ServerMessageMetadata (as in SDK).
        Channel is already in query params (injected by server).
        """
        request = Request(scope, receive)
        
        # Extract session_id
        session_id_param = request.query_params.get("session_id")
        if session_id_param is None:
            response = Response("session_id is required", status_code=400)
            return await response(scope, receive, send)
        
        # Convert hex to UUID for writer lookup
        try:
            session_id = UUID(hex=session_id_param)
        except ValueError:
            response = Response("Invalid session ID", status_code=400)
            return await response(scope, receive, send)
        
        writer = self._read_stream_writers.get(session_id)
        if not writer:
            response = Response("Could not find session", status_code=404)
            return await response(scope, receive, send)
        
        # Save channel in scope for access in handlers
        scope["channel"] = channel_registry.get_channel(session_id.hex)
        
        # Read and validate JSON body
        body = await request.body()
        try:
            message = types.JSONRPCMessage.model_validate_json(body)
        except ValidationError as err:
            logger.warning(f"Failed to parse message: {err}")
            response = Response("Could not parse message", status_code=400)
            await response(scope, receive, send)
            await writer.send(err)
            return
        
        # Create SessionMessage with metadata (as in SDK)
        metadata = ServerMessageMetadata(request_context=request)
        session_message = SessionMessage(message, metadata=metadata)
        
        # Send response and message
        response = Response("Accepted", status_code=202)
        await response(scope, receive, send)
        await writer.send(session_message)
