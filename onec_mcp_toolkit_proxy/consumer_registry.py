"""Versioned 1C consumer binding. State is confined to the asyncio event loop."""
import asyncio
from contextvars import ContextVar
from dataclasses import dataclass, field
from datetime import datetime, timezone
import time
import uuid

HOST_TOOLS = frozenset({'execute_query', 'execute_code', 'get_metadata', 'get_event_log',
                       'get_object_by_link', 'get_link_of_object',
                       'find_references_to_object', 'get_access_rights', 'get_bsl_syntax_help'})
LEASE_SECONDS = 60.0


class ConsumerChanged(RuntimeError):
    pass


@dataclass(eq=False)
class Consumer:
    channel: str
    mode: str
    identity: tuple = ()
    tools: frozenset = HOST_TOOLS
    anon_confirm: bool = False
    lease: str = field(default_factory=lambda: uuid.uuid4().hex)
    generation: str = field(default_factory=lambda: uuid.uuid4().hex.upper())
    seen: float = field(default_factory=time.monotonic)
    retired: bool = False
    last_seen_monotonic: float | None = None
    last_seen_at: datetime | None = None

    @property
    def cache_key(self):
        return (self.channel, self.generation) if self.mode == 'server_host' else self.channel


current_consumer: ContextVar[Consumer | None] = ContextVar('current_consumer', default=None)


class ConsumerRegistry:
    def __init__(self):
        self.consumers = {}
        self.waiters = {}

    def check(self, consumer):
        if consumer is not None and (consumer.retired or self.consumers.get(consumer.channel) is not consumer):
            raise ConsumerChanged('1C channel owner changed; request was not repeated')

    def record_activity(self, consumer):
        """Record an accepted external 1C request without changing its lease."""
        if consumer is None or consumer.retired or self.consumers.get(consumer.channel) is not consumer:
            return
        consumer.last_seen_monotonic = time.monotonic()
        consumer.last_seen_at = datetime.now(timezone.utc)

    def activity_status(self, channel, now):
        """Read one channel, including its last contact before explicit release."""
        consumer = self.consumers.get(channel)
        if consumer is None or consumer.last_seen_monotonic is None:
            return {'last_seen_at': None, 'last_seen_seconds_ago': None}
        return {
            'last_seen_at': consumer.last_seen_at.isoformat(timespec='milliseconds').replace('+00:00', 'Z'),
            'last_seen_seconds_ago': round(max(0, now - consumer.last_seen_monotonic), 3),
        }

    def retire(self, consumer):
        if consumer is None:
            return
        consumer.retired = True
        from .command_queue import channel_command_queue
        channel_command_queue.invalidate_consumer(consumer)
        from .anonymizer.registry import AnonymizerRegistry
        from .anonymizer.dictionary_preloader import dictionary_preloader
        AnonymizerRegistry._registry.pop(consumer.cache_key, None)
        dictionary_preloader._matchers.pop(consumer.cache_key, None)
        dictionary_preloader._loading_locks.pop(consumer.cache_key, None)

    def bind(self, channel, mode, identity=(), tools=HOST_TOOLS, anon_confirm=False):
        old = self.consumers.get(channel)
        if old and not old.retired:
            if old.mode == mode and old.identity == identity:
                if old.tools != tools or old.anon_confirm != anon_confirm:
                    raise ConsumerChanged('Capabilities changed without a new owner')
                old.seen = time.monotonic()
                return old
            from .command_queue import channel_command_queue
            queue = channel_command_queue._channels.get(channel)
            now = time.monotonic()
            # A sequential BSL command can occupy its controller longer than a
            # heartbeat interval. Silence during a live command is not vacancy.
            busy = queue is not None and any(
                command.consumer is old and not command.result_event.is_set()
                and command.deadline is not None and command.deadline > now
                for command in queue._pending.values()
            )
            if now - old.seen < LEASE_SECONDS or busy:
                raise ConsumerChanged('Channel already has an active 1C consumer')
        self.retire(old)
        value = Consumer(channel, mode, identity, frozenset(tools), anon_confirm)
        self.consumers[channel] = value
        for event in self.waiters.get(channel, ()):
            event.set()
        return value

    def require(self, channel, lease):
        value = self.consumers.get(channel)
        if value is None or value.retired or value.mode != 'server_host' or value.lease != lease:
            raise ConsumerChanged('Server host registration required')
        value.seen = time.monotonic()
        return value

    async def wait(self, channel):
        while True:
            value = self.consumers.get(channel)
            if value is not None and not value.retired:
                return value
            event = asyncio.Event()
            waiters = self.waiters.setdefault(channel, set())
            waiters.add(event)
            try:
                await event.wait()
            finally:
                waiters.discard(event)
                if not waiters and self.waiters.get(channel) is waiters:
                    self.waiters.pop(channel, None)


consumer_registry = ConsumerRegistry()


def check_current():
    consumer_registry.check(current_consumer.get())


def cache_key(channel):
    value = current_consumer.get()
    if value is not None:
        if value.channel != channel:
            raise ConsumerChanged('Consumer channel mismatch')
        consumer_registry.check(value)
        return value.cache_key
    value = consumer_registry.consumers.get(channel)
    return value.cache_key if value and not value.retired else channel
