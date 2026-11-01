"""Server-host handshake and fenced command delivery (protocol version 1)."""
import math
from starlette.responses import JSONResponse, Response
from .channel_registry import DEFAULT_CHANNEL, CHANNEL_ID_PATTERN
from .command_queue import channel_command_queue, CommandQueue
from .consumer_registry import consumer_registry, ConsumerChanged, HOST_TOOLS


def channel_of(request):
    channel = request.query_params.get('channel', DEFAULT_CHANNEL) or DEFAULT_CHANNEL
    if not CHANNEL_ID_PATTERN.fullmatch(channel):
        raise ValueError('Invalid channel')
    return channel


def host_of(request):
    return consumer_registry.require(channel_of(request), request.headers.get('X-MCP-Host-Lease', ''))


async def register_host(request):
    try:
        body = await request.json()
        if type(body.get('version')) is not int or body['version'] != 1 or body.get('finalization_owner') != 'proxy':
            raise ValueError('Unsupported server-host contract')
        identity = tuple(body.get(key) for key in ('base', 'registration', 'user', 'instance', 'epoch'))
        # Scheduled 1C jobs may run without a named infobase user. The other
        # identity fields still fence the registration and running instance.
        if any(not isinstance(v, str) or (not v and i != 2) or len(v) > 512
               for i, v in enumerate(identity)):
            raise ValueError('Invalid server-host identity')
        tools = body.get('tools')
        if not isinstance(tools, list) or any(not isinstance(t, str) or t not in HOST_TOOLS for t in tools):
            raise ValueError('Invalid server-host capabilities')
        anon_confirm = body.get('anon_confirm', False)
        if type(anon_confirm) is not bool:
            raise ValueError('Invalid confirmation policy')
        owner = consumer_registry.bind(channel_of(request), 'server_host', identity, frozenset(tools), anon_confirm)
        consumer_registry.record_activity(owner)
        channel_command_queue._channels.setdefault(owner.channel, CommandQueue())
        from .config import settings
        # Do not hand out a lease until cold model imports have finished. Some
        # native imports hold the GIL even in a worker thread; an already waiting
        # short 1C poll could otherwise time out before its handler starts.
        # Registration retries share the same owner and initialization task.
        if settings.anonymization_enabled:
            from .anonymizer.registry import AnonymizerRegistry
            await AnonymizerRegistry.get_async(owner.channel)
            consumer_registry.check(owner)
        return JSONResponse({'version': 1, 'finalization_owner': 'proxy', 'lease': owner.lease,
                             'generation': owner.generation, 'anonymization': settings.anonymization_enabled,
                             'format': settings.response_format})
    except ConsumerChanged as exc:
        return JSONResponse({'error': str(exc)}, status_code=409)
    except (ValueError, TypeError, AttributeError):
        return JSONResponse({'error': 'Invalid server-host registration'}, status_code=400)


async def poll_host(request):
    try:
        owner = host_of(request)
        timeout = float(request.query_params.get('timeout', '1'))
        if not math.isfinite(timeout) or timeout < 0 or timeout > 5:
            raise ValueError()
        consumer_registry.record_activity(owner)
        command = await channel_command_queue.get_next_command(owner.channel, timeout=timeout, consumer=owner)
        consumer_registry.check(owner)
        if command is None:
            return Response(status_code=204)
        remaining = channel_command_queue.command_states(owner.channel, [command.id])[0]['remaining_ms']
        return JSONResponse(dict(command.to_dict(), remaining_ms=remaining, version=1, lease=owner.lease))
    except ConsumerChanged:
        return JSONResponse({'error': 'Server host registration required'}, status_code=409)
    except ValueError:
        return JSONResponse({'error': 'Invalid poll timeout'}, status_code=400)


async def host_states(request):
    try:
        owner = host_of(request)
        ids = request.query_params.get('ids', '').split(',')
        if ids == ['']: ids = []
        if len(ids) > 16 or any(not x or len(x) > 36 for x in ids):
            return JSONResponse({'error': 'Invalid command IDs'}, status_code=400)
        consumer_registry.record_activity(owner)
        items = channel_command_queue.command_states(owner.channel, ids)
        for item in items:
            command = channel_command_queue.get_command(item['id'])
            if command is None or command.consumer is not owner:
                item.update(active=False, remaining_ms=0)
        return JSONResponse({'version': 1, 'items': items})
    except ConsumerChanged:
        return JSONResponse({'error': 'Server host registration required'}, status_code=409)
    except ValueError:
        return JSONResponse({'error': 'Invalid channel'}, status_code=400)


async def release_host(request):
    try:
        owner = host_of(request)
        consumer_registry.record_activity(owner)
        consumer_registry.retire(owner)
        return JSONResponse({'version': 1, 'released': True})
    except ConsumerChanged:
        return JSONResponse({'error': 'Server host registration required'}, status_code=409)
    except ValueError:
        return JSONResponse({'error': 'Invalid channel'}, status_code=400)
