"""
Channel Registry for managing MCP session to channel mappings.

This module provides the ChannelRegistry class for storing and managing
the correspondence between MCP session IDs and channel IDs.

Validates: Requirements 4.1, 4.2, 4.3
"""

from typing import Dict
import re
import logging

logger = logging.getLogger(__name__)

# Pattern for validating channel_id
# Validates: Requirement 4.1 (alphanumeric, dash, underscore)
# Validates: Requirement 4.2 (max 64 characters)
CHANNEL_ID_PATTERN = re.compile(r'^[a-zA-Z0-9_-]{1,64}$')
DEFAULT_CHANNEL = "default"


class ChannelRegistry:
    """
    Registry for storing session_id -> channel_id mappings.
    
    This is the single source of truth for channel validation and
    session-to-channel mapping in the system.
    """
    
    def __init__(self):
        self._sessions: Dict[str, str] = {}  # session_id -> channel_id
    
    def register(self, session_id: str, channel_id: str) -> None:
        """
        Register a channel for a session.
        
        Args:
            session_id: The MCP session ID (hex string)
            channel_id: The channel ID to associate with the session
        """
        validated_channel = self.validate_channel_id(channel_id)
        self._sessions[session_id] = validated_channel
        logger.info(f"Registered session {session_id[:8]}... to channel '{validated_channel}'")
    
    def get_channel(self, session_id: str) -> str:
        """
        Get the channel for a session.
        
        Args:
            session_id: The MCP session ID
            
        Returns:
            The channel ID for the session, or DEFAULT_CHANNEL if not found.
        """
        return self._sessions.get(session_id, DEFAULT_CHANNEL)
    
    def has_session(self, session_id: str) -> bool:
        """
        Check if a session is registered.
        
        Args:
            session_id: The MCP session ID
            
        Returns:
            True if the session is registered, False otherwise.
        """
        return session_id in self._sessions
    
    def unregister(self, session_id: str) -> None:
        """
        Remove a session from the registry.
        
        Args:
            session_id: The MCP session ID to remove
        """
        if session_id in self._sessions:
            channel = self._sessions[session_id]
            del self._sessions[session_id]
            logger.info(f"Unregistered session {session_id[:8]}... from channel '{channel}'")
    
    @staticmethod
    def validate_channel_id(channel_id: str) -> str:
        """
        Validate and normalize a channel_id.
        
        This is the SINGLE place for channel_id validation in the system.
        All components should use this method.
        
        Validates: Requirements 4.1, 4.2, 4.3
        - 4.1: Channel ID validated for allowed characters (alphanumeric, dash, underscore)
        - 4.2: Maximum length 64 characters
        - 4.3: Empty string returns "default"
        
        Args:
            channel_id: The channel ID to validate
            
        Returns:
            The validated channel ID, or DEFAULT_CHANNEL if invalid/empty.
        """
        if not channel_id or not channel_id.strip():
            return DEFAULT_CHANNEL
        
        channel_id = channel_id.strip()
        
        if not CHANNEL_ID_PATTERN.match(channel_id):
            logger.warning(f"Invalid channel_id '{channel_id}', using default")
            return DEFAULT_CHANNEL
        
        return channel_id
    
    def get_active_channels(self) -> Dict[str, int]:
        """
        Get statistics about active channels.
        
        Returns:
            Dictionary mapping channel_id to number of sessions using that channel.
        """
        stats: Dict[str, int] = {}
        for channel in self._sessions.values():
            stats[channel] = stats.get(channel, 0) + 1
        return stats


# Global instance
channel_registry = ChannelRegistry()
