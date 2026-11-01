"""
Per-channel anonymizer registry.
"""
import asyncio
import threading
from typing import Dict, Optional

from .anonymizer import Anonymizer
from ..config import settings
from .dictionary_preloader import dictionary_preloader
from ..consumer_registry import cache_key, check_current, ConsumerChanged


class AnonymizerRegistry:
    """Manages per-channel Anonymizer instances (like ChannelRegistry)."""

    _registry: Dict[str, Anonymizer] = {}
    _lock = threading.Lock()
    _initializing = {}

    @classmethod
    async def get_async(cls, channel: str) -> Anonymizer:
        """Build models off the event loop, then publish to the same owner only."""
        key = cache_key(channel)
        with cls._lock:
            existing = cls._registry.get(key)
        if existing is not None:
            return existing
        task = cls._initializing.get(key)
        if task is None:
            options = dict(radical_mode=settings.anonymization_radical_mode,
                           include_errors=settings.anonymization_include_errors,
                           tokenmap_max=settings.anonymization_tokenmap_max,
                           generation=key[1] if isinstance(key, tuple) else '')
            async def initialize():
                value = await asyncio.to_thread(Anonymizer, **options)
                check_current()
                if cache_key(channel) != key or cls._initializing.get(key) is not asyncio.current_task():
                    raise ConsumerChanged('Anonymizer owner changed during initialization')
                with cls._lock:
                    return cls._registry.setdefault(key, value)
            task = asyncio.create_task(initialize())
            cls._initializing[key] = task
            def finished(done):
                if cls._initializing.get(key) is done:
                    cls._initializing.pop(key, None)
                if not done.cancelled():
                    done.exception()  # observe failures if every caller cancelled
            task.add_done_callback(finished)
        # One cancelled request must not cancel initialization shared by others.
        result = await asyncio.shield(task)
        check_current()
        if cache_key(channel) != key:
            raise ConsumerChanged('Anonymizer owner changed during initialization')
        return result

    @classmethod
    def get(cls, channel: str) -> Anonymizer:
        """Get or create per-channel Anonymizer instance."""
        key = cache_key(channel)
        with cls._lock:
            if key not in cls._registry:
                cls._registry[key] = Anonymizer(
                    radical_mode=settings.anonymization_radical_mode,
                    include_errors=settings.anonymization_include_errors,
                    tokenmap_max=settings.anonymization_tokenmap_max,
                    generation=key[1] if isinstance(key, tuple) else '',
                )
            return cls._registry[key]

    @classmethod
    async def ensure_dictionary_loaded(cls, channel: str) -> None:
        """Ensure dictionary matcher is loaded for this channel's anonymizer.
        Called from async context before anonymization. No-op if disabled.
        """
        anon = await cls.get_async(channel)
        if anon._dict_matcher is not None:
            return  # already loaded
        matcher = await dictionary_preloader.get_matcher(channel)
        check_current()
        if matcher is not None:
            anon.set_dict_matcher(matcher)

    @classmethod
    def get_if_exists(cls, channel: str) -> Optional["Anonymizer"]:
        """Return anonymizer for channel if it exists, None otherwise."""
        with cls._lock:
            return cls._registry.get(cache_key(channel))

    @classmethod
    def clear(cls, channel: Optional[str] = None):
        """Clear mapping for channel (or all channels)."""
        with cls._lock:
            if channel:
                cls._registry.pop(cache_key(channel), None)
                cls._initializing.pop(cache_key(channel), None)
            else:
                cls._registry.clear()
                cls._initializing.clear()
