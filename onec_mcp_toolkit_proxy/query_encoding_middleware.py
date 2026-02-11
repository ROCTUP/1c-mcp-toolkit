"""
Query Encoding Middleware for automatic detection and re-encoding of
non-UTF-8 percent-encoded query parameters.

Windows clients (e.g. curl from a CP1251 terminal) percent-encode raw CP1251
bytes. The server then decodes them as UTF-8 and gets garbage. This ASGI
middleware intercepts scope['query_string'] *before* any handler, detects
the encoding of each value, and re-encodes non-UTF-8 values into UTF-8
percent-encoded form.

Only processes /api/... routes (REST API). MCP, 1C, and health routes are
passed through unchanged.
"""

import logging
from urllib.parse import unquote_to_bytes, quote

from starlette.types import ASGIApp, Receive, Scope, Send

from .config import settings
from .rest_api import _encoding_quality_score

logger = logging.getLogger(__name__)


def _fix_query_string(raw_qs: bytes) -> bytes:
    """
    Detect non-UTF-8 percent-encoded values in a query string and re-encode
    them as UTF-8.

    Algorithm:
    1. Decode raw_qs as ASCII. If it contains non-ASCII bytes (unusual but
       possible with non-standard clients), return unchanged.
    2. Split by '&', each pair by first '='.
    3. For each value:
       - Replace '+' with '%20' before unquoting (preserves space semantics
         per application/x-www-form-urlencoded).
       - unquote_to_bytes -> raw bytes.
       - Try decode('utf-8'): if OK -> fast path, keep the ORIGINAL pair
         (no '+' replacement) so parse_qs still sees '+' as space.
       - If UTF-8 fails -> try ['cp1251', 'cp866'] with scoring via
         _encoding_quality_score. Winner -> encode('utf-8') ->
         quote(text, safe='') so spaces become '%20'.
    4. If nothing changed -> return original raw_qs (zero allocation).

    Args:
        raw_qs: Raw query string bytes from scope['query_string'].

    Returns:
        Possibly re-encoded query string bytes (UTF-8 percent-encoded).
    """
    if not raw_qs:
        return raw_qs

    # Step 1: ASCII guard
    try:
        qs_ascii = raw_qs.decode('ascii')
    except UnicodeDecodeError:
        # Non-ASCII bytes in raw query string — pass through unchanged
        return raw_qs

    # Step 2: Split into pairs
    pairs = qs_ascii.split('&')
    result_pairs = []
    changed = False

    for pair in pairs:
        if '=' not in pair:
            # Bare key (e.g. "debug") — keep as-is
            result_pairs.append(pair)
            continue

        key, value = pair.split('=', 1)

        if not value:
            # Empty value (e.g. "meta_type=") — keep as-is
            result_pairs.append(pair)
            continue

        # Step 3: Replace '+' with '%20' before unquoting
        value_plus_fixed = value.replace('+', '%20')
        raw_bytes = unquote_to_bytes(value_plus_fixed)

        # Fast path: try UTF-8
        try:
            raw_bytes.decode('utf-8')
            # Valid UTF-8 — keep the ORIGINAL pair (preserves '+' semantics)
            result_pairs.append(pair)
            continue
        except UnicodeDecodeError:
            pass

        # Slow path: try legacy encodings with scoring
        best_text = None
        best_score = None
        best_encoding = None

        for encoding in ('cp1251', 'cp866'):
            try:
                text = raw_bytes.decode(encoding)
            except UnicodeDecodeError:
                continue

            score = _encoding_quality_score(text)
            if best_score is None or score > best_score:
                best_text = text
                best_score = score
                best_encoding = encoding

        if best_text is not None:
            # Re-encode as UTF-8 percent-encoded; spaces become %20
            new_value = quote(best_text, safe='')
            result_pairs.append(f"{key}={new_value}")
            changed = True
            logger.info(
                f"Query param '{key}': re-encoded from {best_encoding} to UTF-8"
            )
        else:
            # Could not decode with any encoding — keep original
            result_pairs.append(pair)

    if not changed:
        return raw_qs

    return '&'.join(result_pairs).encode('ascii')


class QueryEncodingMiddleware:
    """
    Pure ASGI middleware that fixes non-UTF-8 percent-encoded query parameters.

    - Only processes HTTP requests.
    - Only processes /api/... paths (REST API routes).
    - Checks settings.enable_encoding_auto_detection feature flag.
    - Replaces scope['query_string'] with the fixed version before passing
      to downstream middleware/handlers.
    """

    def __init__(self, app: ASGIApp):
        self.app = app

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope['type'] != 'http':
            await self.app(scope, receive, send)
            return

        if not settings.enable_encoding_auto_detection:
            await self.app(scope, receive, send)
            return

        path = scope.get('path', '')
        if not path.startswith('/api/'):
            await self.app(scope, receive, send)
            return

        raw_qs = scope.get('query_string', b'')
        if raw_qs:
            fixed_qs = _fix_query_string(raw_qs)
            if fixed_qs is not raw_qs:
                scope['query_string'] = fixed_qs

        await self.app(scope, receive, send)
