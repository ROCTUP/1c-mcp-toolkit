"""
Per-channel anonymizer registry.
"""
import threading
from typing import Dict, Optional

from .anonymizer import Anonymizer
from ..config import settings
from .dictionary_preloader import dictionary_preloader


class AnonymizerRegistry:
    """Manages per-channel Anonymizer instances (like ChannelRegistry)."""

    _registry: Dict[str, Anonymizer] = {}
    _lock = threading.Lock()

    @classmethod
    def get(cls, channel: str) -> Anonymizer:
        """Get or create per-channel Anonymizer instance."""
        with cls._lock:
            if channel not in cls._registry:
                cls._registry[channel] = Anonymizer(
                    radical_mode=settings.anonymization_radical_mode,
                    include_errors=settings.anonymization_include_errors,
                    tokenmap_max=settings.anonymization_tokenmap_max,
                )
            return cls._registry[channel]

    @classmethod
    async def ensure_dictionary_loaded(cls, channel: str) -> None:
        """Ensure dictionary matcher is loaded for this channel's anonymizer.
        Called from async context before anonymization. No-op if disabled.
        """
        anon = cls.get(channel)
        if anon._dict_matcher is not None:
            return  # already loaded
        matcher = await dictionary_preloader.get_matcher(channel)
        if matcher is not None:
            anon.set_dict_matcher(matcher)

    @classmethod
    def get_if_exists(cls, channel: str) -> Optional["Anonymizer"]:
        """Return anonymizer for channel if it exists, None otherwise."""
        with cls._lock:
            return cls._registry.get(channel)

    @classmethod
    def clear(cls, channel: Optional[str] = None):
        """Clear mapping for channel (or all channels)."""
        with cls._lock:
            if channel:
                cls._registry.pop(channel, None)
            else:
                cls._registry.clear()
