"""
Command queue for managing commands between MCP clients and 1C processing.

This module provides the Command, CommandQueue, and ChannelCommandQueue classes
for handling asynchronous command execution between AI agents and 1C:Enterprise.

The ChannelCommandQueue provides channel isolation, ensuring commands from
different MCP sessions are routed to the correct 1C clients.

Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5
"""

import asyncio
import time
import uuid
import logging
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)


@dataclass
class Command:
    """Represents a command to be executed by 1C processing."""
    
    id: str
    tool: str
    params: Dict[str, Any]
    created_at: datetime = field(default_factory=datetime.utcnow)
    result_event: asyncio.Event = field(default_factory=asyncio.Event)
    
    def to_dict(self) -> Dict[str, Any]:
        """Convert command to dictionary for JSON serialization."""
        return {
            "id": self.id,
            "tool": self.tool,
            "params": self.params
        }


class CommandQueue:
    """
    Queue for managing commands between MCP clients and 1C processing.
    
    Commands are added by MCP tool handlers and retrieved by 1C processing
    via long polling. Results are set by 1C processing and awaited by
    MCP tool handlers.
    """
    
    def __init__(self):
        self._pending: Dict[str, Command] = {}
        self._results: Dict[str, Any] = {}
        self._queue: asyncio.Queue[Command] = asyncio.Queue()
        self._lock = asyncio.Lock()
    
    async def add_command(self, tool: str, params: Dict[str, Any]) -> str:
        """
        Add a new command to the queue.
        
        Args:
            tool: Name of the MCP tool (execute_query, execute_code, get_metadata)
            params: Parameters for the tool
            
        Returns:
            Command ID (UUID string)
        """
        command_id = str(uuid.uuid4())
        command = Command(
            id=command_id,
            tool=tool,
            params=params
        )
        
        async with self._lock:
            self._pending[command_id] = command
        
        await self._queue.put(command)
        return command_id
    
    async def get_next_command(self, timeout: Optional[float] = None) -> Optional[Command]:
        """
        Get the next command from the queue (for 1C polling).
        
        Args:
            timeout: Maximum time to wait for a command (seconds).
                    If None, returns immediately if no command available.
                    
        Returns:
            Next command or None if no command available within timeout.
        """
        try:
            if timeout is None or timeout <= 0:
                # Non-blocking get (timeout <= 0 should not miss queued items)
                return self._queue.get_nowait()
            # Blocking get with timeout
            return await asyncio.wait_for(
                self._queue.get(),
                timeout=timeout
            )
        except (asyncio.QueueEmpty, asyncio.TimeoutError):
            return None
    
    async def set_result(self, command_id: str, result: Any) -> bool:
        """
        Set the result for a command (called by 1C after execution).
        
        Args:
            command_id: ID of the command
            result: Result data from 1C
            
        Returns:
            True if command was found and result was set, False otherwise.
        """
        async with self._lock:
            if command_id not in self._pending:
                return False
            
            command = self._pending[command_id]
            self._results[command_id] = result
            command.result_event.set()
            return True
    
    async def wait_for_result(self, command_id: str, timeout: float) -> Any:
        """
        Wait for the result of a command.
        
        Args:
            command_id: ID of the command to wait for
            timeout: Maximum time to wait (seconds)
            
        Returns:
            Result data from 1C
            
        Raises:
            asyncio.TimeoutError: If timeout is exceeded
            KeyError: If command_id is not found
        """
        async with self._lock:
            if command_id not in self._pending:
                raise KeyError(f"Command {command_id} not found")
            command = self._pending[command_id]
        
        # Wait for result with timeout
        await asyncio.wait_for(command.result_event.wait(), timeout=timeout)
        
        # Get and clean up result
        async with self._lock:
            result = self._results.pop(command_id, None)
            self._pending.pop(command_id, None)
        
        return result
    
    async def get_pending_count(self) -> int:
        """Get the number of pending commands."""
        async with self._lock:
            return len(self._pending)
    
    async def cleanup_expired(self, max_age_seconds: float) -> int:
        """
        Remove expired commands that have been pending too long.
        
        Args:
            max_age_seconds: Maximum age of commands to keep
            
        Returns:
            Number of commands removed
        """
        now = datetime.utcnow()
        removed = 0
        
        async with self._lock:
            expired_ids = [
                cmd_id for cmd_id, cmd in self._pending.items()
                if (now - cmd.created_at).total_seconds() > max_age_seconds
            ]
            
            for cmd_id in expired_ids:
                self._pending.pop(cmd_id, None)
                self._results.pop(cmd_id, None)
                removed += 1
        
        return removed
    
    async def remove_command(self, command_id: str) -> bool:
        """
        Remove a command from pending (for cleanup on timeout).
        
        Args:
            command_id: ID of the command to remove
            
        Returns:
            True if command was found and removed, False otherwise.
        """
        async with self._lock:
            if command_id in self._pending:
                del self._pending[command_id]
                self._results.pop(command_id, None)
                return True
            return False


class ChannelCommandQueue:
    """
    Command queue with channel isolation support.
    
    Each channel has its own isolated queue. Commands from MCP sessions
    with channel=X are only delivered to 1C clients polling channel=X.
    
    The command_index provides O(1) lookup of channel by command_id.
    
    Locking strategy:
    - _lock protects only dict operations (_channels, _command_index)
    - await operations are performed OUTSIDE lock to avoid head-of-line blocking
    
    Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5
    """
    
    def __init__(self):
        self._channels: Dict[str, CommandQueue] = {}
        self._command_index: Dict[str, str] = {}  # command_id -> channel
        self._lock = asyncio.Lock()
        
        # Default channel always exists
        self._channels["default"] = CommandQueue()
    
    async def add_command(self, channel: str, tool: str, params: Dict[str, Any]) -> str:
        """
        Add a command to a channel's queue.
        
        Creates the queue for the channel if it doesn't exist.
        Stores command_id -> channel mapping in the index.
        
        Args:
            channel: Channel ID to add the command to
            tool: Name of the MCP tool
            params: Parameters for the tool
            
        Returns:
            Command ID (UUID string)
        """
        # Under lock: get/create queue
        async with self._lock:
            if channel not in self._channels:
                self._channels[channel] = CommandQueue()
                logger.info(f"Created queue for channel '{channel}'")
            queue = self._channels[channel]
        
        # Outside lock: add command (may await)
        command_id = await queue.add_command(tool, params)
        
        # Under lock: update index
        async with self._lock:
            self._command_index[command_id] = channel
        
        logger.info(f"Command {command_id} added to channel '{channel}'")
        return command_id
    
    async def get_next_command(self, channel: str, timeout: Optional[float] = None) -> Optional[Command]:
        """
        Get the next command from a channel's queue.
        
        Does NOT create a queue for unknown channels.
        For non-existent channels, returns None (204).
        
        Checks that the command is still in pending (not cancelled by timeout).
        If cancelled, skips and gets the next one.
        
        Uses deadline to preserve remaining wait time when skipping cancelled commands.
        
        Args:
            channel: Channel ID to poll
            timeout: Maximum time to wait for a command (seconds)
            
        Returns:
            Next command or None if no command available within timeout.
        """
        # Under lock: get queue (without creating)
        async with self._lock:
            queue = self._channels.get(channel)
        
        if queue is None:
            logger.debug(f"Poll for unknown channel '{channel}', returning empty")
            return None
        
        # Calculate deadline to preserve wait time
        deadline = time.monotonic() + (timeout or 0) if timeout else None
        
        # Outside lock: get command with validity check
        while True:
            # Recalculate remaining time
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                current_timeout = remaining
            else:
                current_timeout = timeout
            
            command = await queue.get_next_command(current_timeout)
            if command is None:
                return None
            
            # Check that command is still in index (not cancelled)
            async with self._lock:
                if command.id in self._command_index:
                    return command
            
            # Command was cancelled by timeout - skip, get next
            logger.debug(f"Skipping cancelled command {command.id}")
    
    async def set_result(self, command_id: str, result: Any) -> bool:
        """
        Set the result for a command.
        
        Uses the index for O(1) channel lookup.
        Does NOT delete the index - that's done by wait_for_result.
        
        Args:
            command_id: ID of the command
            result: Result data from 1C
            
        Returns:
            True if command was found and result was set, False otherwise.
        """
        # Under lock: find channel and queue
        async with self._lock:
            channel = self._command_index.get(command_id)
            if channel is None:
                logger.warning(f"Command {command_id} not found in index")
                return False
            queue = self._channels.get(channel)
        
        if queue is None:
            return False
        
        # Outside lock: set result
        # Do NOT delete index here - that's done by wait_for_result
        return await queue.set_result(command_id, result)
    
    async def wait_for_result(self, command_id: str, timeout: float) -> Any:
        """
        Wait for the result of a command.
        
        Uses the index for O(1) channel lookup.
        Cleans up the index on success or timeout.
        
        Args:
            command_id: ID of the command to wait for
            timeout: Maximum time to wait (seconds)
            
        Returns:
            Result data from 1C
            
        Raises:
            asyncio.TimeoutError: If timeout is exceeded
            KeyError: If command_id is not found
        """
        # Under lock: find channel
        async with self._lock:
            channel = self._command_index.get(command_id)
        
        if channel is None:
            raise KeyError(f"Command {command_id} not found in index")
        
        # Under lock: get queue
        async with self._lock:
            queue = self._channels.get(channel)
        
        if queue is None:
            raise KeyError(f"Queue for channel '{channel}' not found")
        
        try:
            # Outside lock: wait for result
            result = await queue.wait_for_result(command_id, timeout)
            
            # Under lock: clean up index after success
            async with self._lock:
                self._command_index.pop(command_id, None)
            
            return result
        except asyncio.TimeoutError:
            # Under lock: clean up index on timeout
            async with self._lock:
                self._command_index.pop(command_id, None)
            
            # Outside lock: remove command from pending
            await queue.remove_command(command_id)
            
            logger.warning(f"Command {command_id} timed out, cleaned up")
            raise
    
    async def get_stats(self) -> Dict[str, int]:
        """
        Get statistics of pending commands by channel.
        
        Makes a snapshot under lock, iterates outside lock.
        
        Returns:
            Dictionary mapping channel_id to number of pending commands.
        """
        # Under lock: make snapshot
        async with self._lock:
            channels_snapshot: List[Tuple[str, CommandQueue]] = list(self._channels.items())
        
        # Outside lock: collect stats
        stats = {}
        for channel, queue in channels_snapshot:
            count = await queue.get_pending_count()
            if count > 0:
                stats[channel] = count
        return stats
    
    def get_active_channels_count(self) -> int:
        """Get the number of active channels."""
        return len(self._channels)
    
    async def _get_command_channel(self, command_id: str) -> Optional[str]:
        """Get the channel for a command (for testing)."""
        async with self._lock:
            return self._command_index.get(command_id)


# Global command queue instance (legacy, for backward compatibility)
command_queue = CommandQueue()

# Global channel command queue instance
channel_command_queue = ChannelCommandQueue()
