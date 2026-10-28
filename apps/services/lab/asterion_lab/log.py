"""Process log in the format of the C++ services' logs (kernel/logger.cpp).

One compact JSON object per line with sorted keys, in
`<ASTERION_LOG_DIRECTORY>/<program>-<pid>_<local date>.log`. The level comes
from the node's `config/logging.json` beside the log directory.
"""
import datetime
import json
import os
import time
from pathlib import Path

LEVELS = {"debug": 10, "info": 20, "warning": 30, "error": 40, "off": 100}


class ProcessLog:
    def __init__(self, program: str, instance: str):
        self.program = program
        self.directory = None
        self.threshold = LEVELS["info"]
        self.process = {"instance": instance, "pid": os.getpid(), "program": program,
                        "started_ns": str(time.time_ns())}
        directory = os.environ.get("ASTERION_LOG_DIRECTORY")
        if not directory:
            return
        self.directory = Path(directory)
        if not self.directory.is_absolute():
            raise ValueError(f"invalid log directory: {directory}")
        policy = self.directory.parent / "config" / "logging.json"
        if policy.exists():
            config = json.loads(policy.read_text())
            if (not isinstance(config, dict) or set(config) != {"level", "retention_days"}
                    or config["level"] not in LEVELS):
                raise ValueError(f"invalid node logging configuration: {policy}")
            self.threshold = LEVELS[config["level"]]
        self.directory.mkdir(parents=True, exist_ok=True)

    def write(self, level: str, module: str, event: str, **fields):
        if self.directory is None or LEVELS[level] < self.threshold:
            return
        record = json.dumps({"event": event, "fields": fields, "level": level, "module": module,
                             "process": self.process, "timestamp_ns": str(time.time_ns())},
                            sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        day = datetime.date.today().isoformat()
        path = self.directory / f"{self.program}-{os.getpid()}_{day}.log"
        with path.open("a", encoding="utf-8") as stream:
            stream.write(record + "\n")
