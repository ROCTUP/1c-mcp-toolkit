"""
Preloader: fetches names from 1C catalogs and builds DictionaryMatcher per channel.
"""
import asyncio
import logging
from typing import Any, Dict, List, Optional

from ..command_queue import channel_command_queue
from ..config import settings
from .dictionary_matcher import DictionaryMatcher
from .anonymization_defaults import DEFAULT_DICTIONARY_SOURCES
from ..consumer_registry import cache_key, check_current, ConsumerChanged, current_consumer

logger = logging.getLogger(__name__)

_PRELOAD_PAGE_SIZE = 1000  # public/server-host execute_query limit


class DictionarySourceError(RuntimeError):
    """A source/optional field is unavailable in this configuration."""


class IncompleteDictionaryError(RuntimeError):
    """Never publish a partially read source as a complete dictionary."""


class DictionaryPreloader:
    """Manages per-channel DictionaryMatcher loading."""

    def __init__(self) -> None:
        self._matchers: Dict[str, DictionaryMatcher] = {}
        self._loading_locks: Dict[str, asyncio.Lock] = {}
        self._global_lock = asyncio.Lock()

    async def get_matcher(self, channel: str) -> Optional[DictionaryMatcher]:
        """Get matcher for channel, triggering preload on first call.

        Returns None if dictionary feature is disabled.
        """
        if not settings.anonymization_dictionary_enabled:
            return None

        # Fast path: already loaded
        key = cache_key(channel)
        if key in self._matchers:
            return self._matchers[key]

        # Get or create per-channel lock
        async with self._global_lock:
            if key not in self._loading_locks:
                self._loading_locks[key] = asyncio.Lock()
            lock = self._loading_locks[key]

        async with lock:
            # Double-check after acquiring
            check_current()
            if key in self._matchers:
                return self._matchers[key]

            try:
                matcher = await self._preload(channel)
                check_current()
                self._matchers[key] = matcher
                return matcher
            except (ConsumerChanged, IncompleteDictionaryError):
                raise
            except Exception as e:
                owner = current_consumer.get()
                if owner is not None and owner.mode == 'server_host':
                    raise
                logger.warning(
                    f"Dictionary preload failed for channel '{channel}': {e}. "
                    "Will retry on next request."
                )
                return None

    async def _preload(self, channel: str) -> DictionaryMatcher:
        """Execute 1C queries and build matcher for channel.

        Raises RuntimeError if all sources failed (so get_matcher won't cache
        and will retry on next request). Caches normally if at least one source
        responded — even if catalogs are empty.
        """
        sources = self._get_sources()
        terms: Dict[str, str] = {}  # canonical_term → category, first wins
        any_source_ok = False

        for source in sources:
            check_current()
            catalog = source.get("from", "")
            category = source.get("category", "ORG")
            extra_fields = source.get("extra_fields", [])
            fields = ["Наименование"] + [f for f in extra_fields if f != "Наименование"]

            try:
                names = await self._fetch_catalog_names(channel, catalog, fields)
                any_source_ok = True
                for name in names:
                    if name and name.strip() and name.strip() not in terms:
                        terms[name.strip()] = category
            except (ConsumerChanged, IncompleteDictionaryError):
                raise
            except Exception as e:
                logger.warning(
                    f"Dictionary preload failed for '{catalog}' on channel '{channel}': {e}"
                )

        if not any_source_ok and sources:
            raise RuntimeError(
                f"All {len(sources)} dictionary sources failed for channel '{channel}'"
            )

        matcher = DictionaryMatcher()
        matcher.build(terms)
        logger.info(
            f"Dictionary loaded for channel '{channel}': "
            f"{matcher.term_count} terms from {len(sources)} sources"
        )
        return matcher

    async def _fetch_catalog_names(
        self, channel: str, catalog: str, fields: List[str]
    ) -> List[str]:
        """Read distinct columns completely; keep available optional fields."""
        names = []
        for field in dict.fromkeys(fields):
            try:
                names.extend(await self._fetch_field_names(channel, catalog, field))
            except DictionarySourceError:
                check_current()
                if field == 'Наименование':
                    raise
                logger.info("Optional dictionary field unavailable: %s.%s", catalog, field)
        return names

    async def _fetch_field_names(self, channel: str, catalog: str, field: str) -> List[str]:
        # Ordering and cursor comparison both use the database's collation.
        names = []
        cursor = None
        seen_cursors = set()
        while True:
            check_current()
            # Literal quoting avoids generic query-param ISO-date coercion for
            # names that look like dates. Preserve the untrimmed cursor value.
            where = '' if cursor is None else ' ГДЕ T.' + field + ' > "' + cursor.replace('"', '""') + '"'
            query = (f'ВЫБРАТЬ ПЕРВЫЕ {_PRELOAD_PAGE_SIZE} РАЗЛИЧНЫЕ T.{field} КАК Наименование '
                     f'ИЗ {catalog} КАК T{where} УПОРЯДОЧИТЬ ПО T.{field}')
            try:
                result = await self._execute_raw(channel, {'query': query, 'limit': _PRELOAD_PAGE_SIZE})
            except Exception as exc:
                check_current()
                if cursor is None and isinstance(exc, DictionarySourceError):
                    raise
                raise IncompleteDictionaryError(f'Dictionary source was not fully read: {catalog}.{field}') from exc
            rows = result.get('data') if isinstance(result, dict) else None
            if not isinstance(rows, list) or len(rows) > _PRELOAD_PAGE_SIZE:
                raise IncompleteDictionaryError('Invalid dictionary page')
            names.extend(self._extract_names(result, ['Наименование']))
            if len(rows) < _PRELOAD_PAGE_SIZE:
                return names
            cursor = rows[-1].get('Наименование') if isinstance(rows[-1], dict) else None
            if not isinstance(cursor, str) or cursor in seen_cursors:
                raise IncompleteDictionaryError('Dictionary page cursor did not advance')
            seen_cursors.add(cursor)

    async def _execute_raw(
        self, channel: str, params: Dict[str, Any]
    ) -> Dict[str, Any]:
        """Execute query via command queue, bypassing anonymization."""
        check_current()
        command_id = await channel_command_queue.add_command(
            channel, "execute_query", params, timeout=float(settings.timeout)
        )
        result = await channel_command_queue.wait_for_result(
            command_id, timeout=float(settings.timeout)
        )
        check_current()
        if isinstance(result, dict) and result.get("success") is False:
            raise DictionarySourceError(result.get("error", "unknown 1C error"))
        return result

    @staticmethod
    def _extract_names(result: Dict[str, Any], fields: List[str]) -> List[str]:
        """Extract name strings from query result rows."""
        names: List[str] = []
        data = result.get("data", [])
        if not isinstance(data, list):
            return names
        for row in data:
            if not isinstance(row, dict):
                continue
            for field in fields:
                val = row.get(field)
                if isinstance(val, str) and val.strip():
                    names.append(val.strip())
        return names

    def _get_sources(self) -> List[Dict]:
        """Return effective sources.

        If SOURCES_OVERRIDE is set — replaces defaults entirely.
        SOURCES_ADD always appended on top (to override or defaults).
        """
        override = settings.anonymization_dictionary_sources_override
        sources = list(override) if override is not None else list(DEFAULT_DICTIONARY_SOURCES)
        add = settings.anonymization_dictionary_sources_add
        if add:
            sources.extend(add)
        return sources


# Global singleton
dictionary_preloader = DictionaryPreloader()
