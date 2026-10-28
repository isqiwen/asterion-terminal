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
import time
from pathlib import Path

from asterion.lab_pb2 import Request, Response

from asterion_lab.log import ProcessLog
from asterion_lab.version import VERSION

MAX_FRAME = 1024 * 1024
MODULE = "lab-service"


class Service:
    def __init__(self, arguments, log: ProcessLog):
        self.name = arguments.session
        self.directory = Path(arguments.directory)
        self.owner = arguments.owner_pid
        self.log = log
        self.started = time.monotonic()
        self.initialized = False
        self.stopping = asyncio.Event()

    def reply(self, request: Request) -> Response:
        response = Response(version=1, service_id=self.name, correlation_id=request.correlation_id)
        if request.version != 1 or request.service_id != self.name:
            response.error.code = "invalid_request"
            response.error.message = "request is for another service or protocol version"
        elif request.HasField("health"):
            health = response.health
            health.instance_id = self.name
            health.process_id = str(os.getpid())
            health.version = VERSION
            health.uptime_ms = int((time.monotonic() - self.started) * 1000)
            health.initialized = self.initialized
        else:
            response.error.code = "invalid_request"
            response.error.message = "request has no operation"
        return response

    async def connection(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        try:
            while True:
                try:
                    size, = struct.unpack("!I", await reader.readexactly(4))
                except asyncio.IncompleteReadError:
                    return
                if not 0 < size <= MAX_FRAME:
                    self.log.write("warning", MODULE, "frame.rejected", size=size)
                    return
                request = Request()
                request.ParseFromString(await reader.readexactly(size))
                payload = self.reply(request).SerializeToString()
                writer.write(struct.pack("!I", len(payload)) + payload)
                await writer.drain()
        except Exception as error:  # one client's failure never stops the service
            self.log.write("warning", MODULE, "connection.failed", message=str(error))
        finally:
            writer.close()

    async def watch_owner(self):
        # The Agent's death ends its services; nothing else would stop this one.
        while not self.stopping.is_set():
            try:
                os.kill(self.owner, 0)
            except ProcessLookupError:
                self.log.write("warning", MODULE, "owner.lost", owner_pid=self.owner)
                self.stopping.set()
                return
            await asyncio.sleep(1)

    async def run(self, endpoints):
        loop = asyncio.get_running_loop()
        for number in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(number, self.stopping.set)
        servers = []
        for endpoint in endpoints:
            Path(endpoint).unlink(missing_ok=True)
            servers.append(await asyncio.start_unix_server(self.connection, path=endpoint))
        self.initialized = True
        self.log.write("info", MODULE, "service.started", directory=str(self.directory),
                       version=VERSION)
        watcher = asyncio.create_task(self.watch_owner())
        await self.stopping.wait()
        watcher.cancel()
        for server in servers:
            server.close()
        self.log.write("info", MODULE, "service.stopped")


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
    log = ProcessLog("lab-service", arguments.session)
    try:
        if arguments.owner_pid <= 1:
            raise ValueError("invalid owner process")
        if not Path(arguments.directory).is_absolute() or not Path(arguments.directory).is_dir():
            raise ValueError(f"service directory is missing: {arguments.directory}")
        asyncio.run(Service(arguments, log).run([arguments.endpoint, arguments.health_endpoint]))
    except Exception as error:
        log.write("error", MODULE, "service.failed", message=str(error))
        print(f"Lab service failed: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
