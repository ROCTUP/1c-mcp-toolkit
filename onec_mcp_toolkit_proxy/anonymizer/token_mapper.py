import threading
import re
from collections import OrderedDict
from typing import Dict


TOKEN_RE = re.compile(
    r"\[([A-Z]+)-(\d{5,})\]",
    re.IGNORECASE,
)  # matches [ORG-00001], [org-00001], [ORG-100000] (6+ digits) etc.


class TokenMapper:
    """Bidirectional mapping real_value ↔ token, thread-safe.

    The token map is a soft-capped LRU (see ``prune``): ``tokenize`` never
    evicts, so a single large response can never evict its own tokens before
    the agent gets them. Eviction happens once per request boundary via
    ``prune``, after incoming tokens have been detokenized (and LRU-touched).
    ``max_size == 0`` disables eviction entirely.
    """

    def __init__(self, max_size: int = 0):
        # OrderedDict preserves insertion / recency order for LRU eviction.
        self._real_to_token: "OrderedDict[str, str]" = OrderedDict()
        self._token_to_real: Dict[str, str] = {}
        self._counters: Dict[str, int] = {}
        self._max_size = max_size
        self._lock = threading.Lock()

    def tokenize(self, value: str, category: str = "STR") -> str:
        """Stable: same value → same token always. Never evicts (see prune)."""
        with self._lock:
            existing = self._real_to_token.get(value)
            if existing is not None:
                self._real_to_token.move_to_end(value)  # LRU-touch
                return existing
            category = category.upper()
            count = self._counters.get(category, 0) + 1
            self._counters[category] = count
            token = f"[{category}-{count:05d}]"
            self._real_to_token[value] = token
            self._token_to_real[token] = value
            return token

    def prune(self) -> None:
        """Evict oldest entries down to max_size. Call at request boundary only.

        Never called during response construction, so a response is never
        forced to evict its own tokens. Category counters are NOT reset — that
        would let surviving tokens collide with reused numbers.
        """
        if not self._max_size:
            return
        with self._lock:
            while len(self._real_to_token) > self._max_size:
                _val, tok = self._real_to_token.popitem(last=False)  # oldest
                self._token_to_real.pop(tok, None)

    def _touch(self, tokens) -> None:
        """Mark used tokens as recently-used so they survive the next prune."""
        with self._lock:
            for tok in tokens:
                real = self._token_to_real.get(tok)
                if real is not None and real in self._real_to_token:
                    self._real_to_token.move_to_end(real)

    def detokenize(self, text: str) -> str:
        """Replace all known tokens in text with real values."""
        touched = []
        with self._lock:
            def _replace(m):
                normalized = f"[{m.group(1).upper()}-{m.group(2)}]"
                real = self._token_to_real.get(normalized)
                if real is None:
                    return m.group(0)
                touched.append(normalized)
                return real
            result = TOKEN_RE.sub(_replace, text)
            for tok in touched:
                self._real_to_token.move_to_end(self._token_to_real[tok])
            return result

    def detokenize_escape_double(self, text: str) -> str:
        """Replace tokens with real values, escaping \" → \"\" unconditionally.

        For BSL code (execute_code): BSL uses only double-quoted string literals,
        so all quote-escaping is always double-quote, regardless of context.
        """
        touched = []
        with self._lock:
            def _replace(m):
                normalized = f"[{m.group(1).upper()}-{m.group(2)}]"
                real = self._token_to_real.get(normalized)
                if real is None:
                    return m.group(0)
                touched.append(normalized)
                return real.replace('"', '""')
            result = TOKEN_RE.sub(_replace, text)
            for tok in touched:
                self._real_to_token.move_to_end(self._token_to_real[tok])
            return result

    def detokenize_for_query(self, text: str) -> str:
        """Replace tokens with real values using state-machine quote context tracking.

        For 1C query language (execute_query): string literals can use either
        \"...\" or '...'. Tracks current quote mode character-by-character and
        escapes accordingly:
          inside \"...\" → escape \" → \"\"
          inside '...'  → escape ' → ''
          outside       → no escaping
        """
        with self._lock:
            snapshot = dict(self._token_to_real)

        touched = []
        OUTSIDE, IN_DOUBLE, IN_SINGLE = 0, 1, 2
        state = OUTSIDE
        result = []
        i = 0
        n = len(text)

        while i < n:
            ch = text[i]

            # Update quote state
            if state == OUTSIDE:
                if ch == '"':
                    state = IN_DOUBLE
                    result.append(ch)
                    i += 1
                    continue
                elif ch == "'":
                    state = IN_SINGLE
                    result.append(ch)
                    i += 1
                    continue
            elif state == IN_DOUBLE:
                if ch == '"':
                    if i + 1 < n and text[i + 1] == '"':
                        result.append('""')  # escaped quote, keep as-is
                        i += 2
                        continue
                    else:
                        state = OUTSIDE
                        result.append(ch)
                        i += 1
                        continue
            elif state == IN_SINGLE:
                if ch == "'":
                    if i + 1 < n and text[i + 1] == "'":
                        result.append("''")  # escaped quote, keep as-is
                        i += 2
                        continue
                    else:
                        state = OUTSIDE
                        result.append(ch)
                        i += 1
                        continue

            # Check for token start
            if ch == '[':
                end = text.find(']', i)
                if end != -1:
                    candidate = text[i:end + 1]
                    m = TOKEN_RE.fullmatch(candidate)
                    if m:
                        normalized = f"[{m.group(1).upper()}-{m.group(2)}]"
                        real = snapshot.get(normalized)
                        if real is not None:
                            touched.append(normalized)
                            if state == IN_DOUBLE:
                                result.append(real.replace('"', '""'))
                            elif state == IN_SINGLE:
                                result.append(real.replace("'", "''"))
                            else:
                                result.append(real)
                            i = end + 1
                            continue

            result.append(ch)
            i += 1

        # LRU-touch used tokens so multi-step references survive the next prune.
        if touched:
            self._touch(touched)
        return ''.join(result)

    def has_tokens(self, text: str) -> bool:
        with self._lock:
            return bool(TOKEN_RE.search(text))

    def get_stats(self) -> Dict[str, int]:
        with self._lock:
            return dict(self._counters)

    def get_mappings(self) -> list:
        """Return all mappings as list of {token, real_value, category} dicts."""
        with self._lock:
            result = []
            for real_val, token in self._real_to_token.items():
                m = TOKEN_RE.match(token)
                category = m.group(1).upper() if m else "UNKNOWN"
                result.append({"token": token, "real_value": real_val, "category": category})
            return result

    def clear(self):
        with self._lock:
            self._real_to_token.clear()
            self._token_to_real.clear()
            self._counters.clear()
