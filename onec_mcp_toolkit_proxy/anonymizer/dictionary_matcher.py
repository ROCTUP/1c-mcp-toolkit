"""
Dictionary-based substring anonymizer using Aho-Corasick automaton.
"""
import logging
from typing import Callable, Dict, List, Optional, Tuple

import ahocorasick

from .token_mapper import TOKEN_RE as _TOKEN_RE
from .anonymization_defaults import DICT_APPLY_KEY_SUBSTRINGS

logger = logging.getLogger(__name__)


def _is_word_char(ch: str) -> bool:
    return ch.isalnum()


class DictionaryMatcher:
    """Aho-Corasick multi-pattern substring matcher with word-boundary check."""

    def __init__(self) -> None:
        self._automaton: Optional[ahocorasick.Automaton] = None
        self._term_count: int = 0

    @property
    def is_loaded(self) -> bool:
        return self._automaton is not None

    @property
    def term_count(self) -> int:
        return self._term_count

    def build(self, terms: Dict[str, str]) -> None:
        """Build automaton from {canonical_term: category}.

        Keys stored in lowercase for case-insensitive matching.
        On collision (same lowercase key), first entry wins.
        """
        A = ahocorasick.Automaton()
        seen: set = set()
        count = 0
        for term, category in terms.items():
            key = term.lower()
            if key in seen:
                continue  # first wins
            seen.add(key)
            A.add_word(key, (term, category))  # value = (canonical_term, category)
            count += 1

        if count:
            A.make_automaton()
            self._automaton = A
            self._term_count = count
            logger.info(f"Dictionary automaton built with {count} terms")
        else:
            logger.warning("Dictionary: no terms, matcher inactive")

    def replace(
        self,
        text: str,
        tokenize_fn: Callable[[str, str], str],
        exclude_fragment: Optional[Callable[[str], bool]] = None,
    ) -> str:
        """Replace dictionary matches in text with tokens.

        - Case-insensitive match (searches text.lower())
        - Word-boundary check (no match inside other words)
        - Skips spans inside existing [TOKEN-NNNN]
        - Longest match wins on overlaps (greedy left-to-right)
        - Calls tokenize_fn(canonical_term, category) — BOTH args required
        - exclude_fragment(found_text) — optional predicate over the ACTUALLY
          matched fragment text[s:e] (case-sensitive). When it returns True the
          fragment is kept verbatim and NOT tokenized. Used to honor the shared
          inline-exclusion predicate for the "error" field.

        end convention: end is EXCLUSIVE (first char after match).
        pyahocorasick.iter() returns end-inclusive → converted: end = aho_end + 1
        Boundary check uses original text (not lowered).
        """
        if not self._automaton or not text:
            return text

        text_lower = text.lower()

        # Collect all matches from automaton (end-inclusive → convert to exclusive)
        raw: List[Tuple[int, int, str, str]] = []
        for aho_end, (canonical_term, category) in self._automaton.iter(text_lower):
            term_len = len(canonical_term)
            start = aho_end - term_len + 1
            end = aho_end + 1  # exclusive
            raw.append((start, end, canonical_term, category))

        if not raw:
            return text

        # Build token spans for "outside tokens" check
        token_spans: List[Tuple[int, int]] = [
            (m.start(), m.end()) for m in _TOKEN_RE.finditer(text)
        ]

        def _overlaps_token(s: int, e: int) -> bool:
            for ts, te in token_spans:
                if not (e <= ts or s >= te):
                    return True
            return False

        def _word_boundary_ok(s: int, e: int) -> bool:
            # left boundary: start of string OR prev char is not alnum
            left_ok = (s == 0) or (not _is_word_char(text[s - 1]))
            # right boundary: end of string OR next char is not alnum
            # e is exclusive, so text[e] is the char after the match
            right_ok = (e == len(text)) or (not _is_word_char(text[e]))
            return left_ok and right_ok

        # Filter: skip overlapping tokens and non-boundary matches
        candidates = [
            (s, e, term, cat)
            for s, e, term, cat in raw
            if not _overlaps_token(s, e) and _word_boundary_ok(s, e)
        ]

        if not candidates:
            return text

        # Resolve overlaps: sort by start asc, length desc → greedy longest-first
        candidates.sort(key=lambda m: (m[0], -(m[1] - m[0])))
        selected: List[Tuple[int, int, str, str]] = []
        last_end = 0
        for s, e, term, cat in candidates:
            if s >= last_end:
                selected.append((s, e, term, cat))
                last_end = e

        # Build result
        parts: List[str] = []
        prev = 0
        for s, e, canonical_term, category in selected:
            parts.append(text[prev:s])
            found = text[s:e]
            # Shared error-field exclusion: check the actually-matched fragment
            # (case-sensitive), keep it verbatim instead of tokenizing.
            if exclude_fragment is not None and exclude_fragment(found):
                parts.append(found)
            else:
                # Pass canonical_term (from dict), not text[s:e], for stable tokens
                parts.append(tokenize_fn(canonical_term, category))
            prev = e
        parts.append(text[prev:])
        return "".join(parts)
