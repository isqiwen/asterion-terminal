"""Lab-owned children bind their lifetime before importing task or user code."""
import asyncio
import importlib
import os
import struct
import sys
import threading
import time

_FRAME_HEADER = struct.Struct("!I")


def write_frame(stream, payload: bytes):
    stream.write(_FRAME_HEADER.pack(len(payload)))
    stream.write(payload)
    stream.flush()


def read_frame(stream) -> bytes | None:
    header = _read_exact(stream, _FRAME_HEADER.size)
    if not header:
        return None
    if len(header) < _FRAME_HEADER.size:
        raise EOFError("truncated frame header")
    (length,) = _FRAME_HEADER.unpack(header)
    data = _read_exact(stream, length)
    if len(data) < length:
        raise EOFError("truncated frame payload")
    return data


def _read_exact(stream, size: int) -> bytes:
    if size == 0:
        return b""
    chunks = []
    remaining = size
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


async def async_write_frame(writer: asyncio.StreamWriter, payload: bytes):
    writer.write(_FRAME_HEADER.pack(len(payload)))
    writer.write(payload)
    await writer.drain()


async def async_read_frame(reader: asyncio.StreamReader) -> bytes | None:
    try:
        header = await reader.readexactly(_FRAME_HEADER.size)
    except asyncio.IncompleteReadError as exc:
        if not exc.partial:
            return None
        raise
    (length,) = _FRAME_HEADER.unpack(header)
    return await reader.readexactly(length)


def switch_source(previous: str | None, current: str) -> str:
    """Switch a reused worker's active code snapshot without leaking prior user modules."""
    if previous == current:
        return current
    if previous:
        prev_resolved = os.path.realpath(previous)
        sys.path[:] = [p for p in sys.path if os.path.realpath(p) != prev_resolved]
        for mod_name, mod in list(sys.modules.items()):
            mod_file = getattr(mod, "__file__", None)
            if mod_file and os.path.realpath(mod_file).startswith(prev_resolved + os.sep):
                sys.modules.pop(mod_name, None)
            elif mod_name in ("strategies", "factors") or mod_name.startswith(("strategies.", "factors.")):
                sys.modules.pop(mod_name, None)
        importlib.invalidate_caches()
    os.chdir(current)
    if current not in sys.path:
        sys.path.insert(0, current)
    return current


def command(module: str) -> list:
    path = [os.path.abspath(entry) for entry in sys.path]
    start = (f"import sys; sys.path[:] = {path!r}; "
             f"from asterion_lab.worker import main; main({os.getpid()}, {module!r})")
    return [sys.executable, "-I", "-B", "-X", "faulthandler", "-c", start]


def main(owner: int, module: str):
    if os.getppid() != owner:
        sys.exit(1)

    def watch():
        while os.getppid() == owner:
            time.sleep(1)
        os._exit(1)

    threading.Thread(target=watch, daemon=True).start()
    importlib.import_module(module).main()


async def output(module: str, *arguments, timeout: float, input=None, cwd=None) -> bytes:
    process = await asyncio.create_subprocess_exec(
        *command(module), *arguments, cwd=cwd, stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        result, error = await asyncio.wait_for(process.communicate(input), timeout)
    except BaseException:
        if process.returncode is None:
            process.kill()
        await process.communicate()
        raise
    if process.returncode != 0:
        lines = error.decode(errors="replace").strip().splitlines()
        raise ValueError(lines[-1] if lines else f"{module} exited with {process.returncode}")
    return result
