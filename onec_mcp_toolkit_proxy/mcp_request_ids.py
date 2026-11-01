"""Preserve JSON-RPC ID types across SDK Streamable HTTP stream routing.

The SDK uses str(id) as a stream key, conflating 501 and "501". Encode all
client request IDs (and cancellation targets) into disjoint string namespaces
before the SDK sees them, and restore response IDs on the wire. No per-request
mapping is retained. Server-initiated requests and client responses are untouched.
This adapter belongs only around Streamable HTTP, not legacy SSE or REST.
"""

import json

from starlette.types import ASGIApp, Receive, Scope, Send


def _encode(identifier):
    if type(identifier) is int:
        return "mcp:i:" + str(identifier)
    if type(identifier) is str:
        return "mcp:s:" + identifier
    return identifier  # Leave invalid IDs to SDK validation.


def _decode(identifier):
    if isinstance(identifier, str):
        if identifier.startswith("mcp:s:"):
            return identifier[6:]
        if identifier.startswith("mcp:i:"):
            try:
                return int(identifier[6:])
            except ValueError:
                pass
    return identifier


def _rewrite_json(body: bytes, incoming: bool) -> bytes:
    try:
        value = json.loads(body)
    except (ValueError, UnicodeError):
        return body
    if not isinstance(value, dict) or value.get("jsonrpc") != "2.0":
        return body
    if incoming:
        if isinstance(value.get("method"), str) and "id" in value:
            value["id"] = _encode(value["id"])
        if value.get("method") == "notifications/cancelled":
            params = value.get("params")
            if isinstance(params, dict) and "requestId" in params:
                params["requestId"] = _encode(params["requestId"])
    elif "method" not in value and "id" in value:
        value["id"] = _decode(value["id"])
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


class _SseResponse:
    """Buffer one event, including split UTF-8 and CRLF, never the whole stream."""

    def __init__(self):
        self.buffer = b""
        self.lines: list[bytes] = []

    def _event(self) -> bytes:
        lines, self.lines = self.lines, []
        data = [line[5:].removeprefix(b" ") for line in lines if line.startswith(b"data:")]
        if not data:
            return b"\n".join(lines) + b"\n\n"
        rewritten = _rewrite_json(b"\n".join(data), incoming=False)
        result = []
        written = False
        for line in lines:
            if line.startswith(b"data:"):
                if not written:
                    result.extend(b"data: " + part for part in rewritten.split(b"\n"))
                    written = True
            else:
                result.append(line)
        return b"\n".join(result) + b"\n\n"

    def feed(self, chunk: bytes, final: bool) -> bytes:
        self.buffer += chunk
        output = []
        while True:
            positions = [p for p in (self.buffer.find(b"\r"), self.buffer.find(b"\n")) if p >= 0]
            if not positions:
                break
            end = min(positions)
            if self.buffer[end:] == b"\r" and not final:
                break
            size = 2 if self.buffer[end:end + 2] == b"\r\n" else 1
            line, self.buffer = self.buffer[:end], self.buffer[end + size:]
            if line:
                self.lines.append(line)
            else:
                output.append(self._event())
        if final:
            # Preserve incomplete events without manufacturing a dispatch.
            output.append(b"\n".join(self.lines + [self.buffer]))
            self.lines = []
            self.buffer = b""
        return b"".join(output)


class McpRequestIdMiddleware:
    def __init__(self, app: ASGIApp):
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        if scope.get("method") == "POST":
            parts = []
            while True:
                message = await receive()
                if message["type"] == "http.disconnect":
                    return
                parts.append(message.get("body", b""))
                if not message.get("more_body", False):
                    break
            body = _rewrite_json(b"".join(parts), incoming=True)
            scope = dict(scope)
            scope["headers"] = [(k, v) for k, v in scope.get("headers", []) if k.lower() != b"content-length"]
            scope["headers"].append((b"content-length", str(len(body)).encode("ascii")))
            original_receive = receive
            delivered = False

            async def receive_body():
                nonlocal delivered
                if not delivered:
                    delivered = True
                    return {"type": "http.request", "body": body, "more_body": False}
                return await original_receive()

            receive = receive_body

        content_type = b""
        json_parts = []
        sse = _SseResponse()

        async def send_response(message):
            nonlocal content_type
            if message["type"] == "http.response.start":
                headers = message.get("headers", [])
                content_type = next((v.split(b";", 1)[0].strip().lower() for k, v in headers if k.lower() == b"content-type"), b"")
                if content_type in (b"application/json", b"text/event-stream"):
                    message = {**message, "headers": [(k, v) for k, v in headers if k.lower() != b"content-length"]}
            elif message["type"] == "http.response.body":
                final = not message.get("more_body", False)
                if content_type == b"application/json":
                    json_parts.append(message.get("body", b""))
                    if not final:
                        return
                    message = {**message, "body": _rewrite_json(b"".join(json_parts), incoming=False)}
                elif content_type == b"text/event-stream":
                    message = {**message, "body": sse.feed(message.get("body", b""), final)}
            await send(message)

        await self.app(scope, receive, send_response)
