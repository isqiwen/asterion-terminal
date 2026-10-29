"""The Python research service. The Agent starts it as

    python3 -I -B -m asterion_lab --owner-pid <pid> --session <name> --directory <dir>
        --health-endpoint <socket> --node-id <id> --endpoint <socket>

and probes the health endpoint. Requests and replies are Protobuf messages
(asterion/lab.proto) framed by a 4-byte big-endian length, as every Asterion
local channel is. The service exits when the Agent that owns it is gone.
"""
import argparse
import asyncio
import os
import signal
import struct
import sys
from pathlib import Path

from asterion.lab_pb2 import Request

from asterion_lab.log import ProcessLog
from asterion_lab.service import MODULE, Service
from asterion_lab.version import VERSION

MAX_REQUEST = 1024 * 1024


async def connection(service: Service, reader, writer):
    try:
        while True:
            try:
                size, = struct.unpack("!I", await reader.readexactly(4))
            except asyncio.IncompleteReadError:
                return
            if not 0 < size <= MAX_REQUEST:
                service.log.write("warning", MODULE, "frame.rejected", size=size)
                return
            request = Request()
            request.ParseFromString(await reader.readexactly(size))
            payload = (await service.reply(request)).SerializeToString()
            writer.write(struct.pack("!I", len(payload)) + payload)
            await writer.drain()
    except Exception as error:  # one client's failure never stops the service
        service.log.write("warning", MODULE, "connection.failed", message=str(error))
    finally:
        writer.close()


async def run(service: Service, owner: int, endpoints):
    stopping = asyncio.Event()
    loop = asyncio.get_running_loop()
    for number in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(number, stopping.set)
    servers = []
    for endpoint in endpoints:
        Path(endpoint).unlink(missing_ok=True)
        servers.append(await asyncio.start_unix_server(
            lambda reader, writer: connection(service, reader, writer), path=endpoint))
    service.initialized = True
    service.log.write("info", MODULE, "service.started", version=VERSION)

    async def watch_owner():
        # The Agent's death ends its services; nothing else would stop this one.
        while True:
            try:
                os.kill(owner, 0)
            except ProcessLookupError:
                service.log.write("warning", MODULE, "owner.lost", owner_pid=owner)
                stopping.set()
                return
            await asyncio.sleep(1)

    watcher = asyncio.create_task(watch_owner())
    await stopping.wait()
    watcher.cancel()
    for server in servers:
        server.close()
    service.log.write("info", MODULE, "service.stopped")


def main():
    parser = argparse.ArgumentParser(prog="asterion_lab", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", action="version", version=f"asterion_lab {VERSION}")
    parser.add_argument("--owner-pid", type=int, required=True)
    parser.add_argument("--session", required=True)
    parser.add_argument("--directory", required=True)
    parser.add_argument("--health-endpoint", required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--node-id", required=True)
    arguments = parser.parse_args()
    # <root>/lab/data/<session>: the lab's installation is <root>/lab.
    log = ProcessLog("asterion_lab", Path(arguments.directory).parents[1])
    try:
        directory = Path(arguments.directory)
        if arguments.owner_pid <= 1:
            raise ValueError("invalid owner process")
        if not directory.is_absolute() or not directory.is_dir():
            raise ValueError(f"service directory is missing: {directory}")
        service = Service(arguments.session, directory, log)
        asyncio.run(run(service, arguments.owner_pid,
                        [arguments.endpoint, arguments.health_endpoint]))
        # An acquisition thread stops at its next request; its stored months stay.
        os._exit(0)
    except Exception as error:
        log.write("error", MODULE, "service.failed", message=str(error))
        print(f"Lab service failed: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
