"""Lab-owned children bind their lifetime before importing task or user code."""
import asyncio
import importlib
import os
import sys
import threading
import time


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
