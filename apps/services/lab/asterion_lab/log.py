"""Process log of the lab, configured like the C++ programs' logs (kernel/log.hpp).

The lab's installation is <root>/lab: its settings are config/logging.json,

    {"console": {"enabled": false, "level": "info", "pattern": "..."},
     "file": {"enabled": true, "level": "info", "pattern": "...",
              "path": "../logs/{program}-{pid}.log", "retention_days": 30}}

with the file path relative to the installation's bin/ (the lab has none of
its own: its interpreter is the bundled Python) or absolute. Files are split
by local day as "<stem>_YYYY-MM-DD.log"; older ones than retention_days go
when the log opens. Patterns use the spdlog tokens %Y %m %d %H %M %S %e %z %l
%n %v. Without settings, records go to standard error at info.
"""
import datetime
import json
import os
import re
import sys
from pathlib import Path

LEVELS = {"debug": 10, "info": 20, "warning": 30, "error": 40}
DEFAULT_PATTERN = "%Y-%m-%d %H:%M:%S.%e %z %l %n %v"
DAILY = re.compile(r".+_(\d{4})-(\d{2})-(\d{2})\.log")


class Output:
    def __init__(self, enabled, level="info", pattern=DEFAULT_PATTERN):
        self.enabled, self.threshold, self.pattern = enabled, LEVELS[level], pattern


def read_settings(file: Path):
    """Returns (console, file, path, retention_days); refuses a mistake naming it."""
    def invalid(what):
        return ValueError(f"invalid logging configuration {file}: {what}")

    config = json.loads(file.read_text())
    if not isinstance(config, dict):
        raise invalid("the file must be an object")
    for key in config:
        if key not in ("console", "file"):
            raise invalid(f"{key} is not a setting")
    outputs = {}
    for name, required in (("console", {"enabled", "level"}),
                           ("file", {"enabled", "level", "path", "retention_days"})):
        if name not in config:
            raise invalid(f"{name} is missing")
        value = config[name]
        if not isinstance(value, dict):
            raise invalid(f"{name} must be an object")
        for key in value:
            if key not in required | {"pattern"}:
                raise invalid(f"{name}.{key} is not a setting")
        for key in required:
            if key not in value:
                raise invalid(f"{name}.{key} is missing")
        if not isinstance(value["enabled"], bool):
            raise invalid(f"{name}.enabled must be true or false")
        if value["level"] not in LEVELS:
            raise invalid(f"{name}.level must be debug, info, warning or error")
        pattern = value.get("pattern", DEFAULT_PATTERN)
        if not isinstance(pattern, str) or not 1 <= len(pattern) <= 256:
            raise invalid(f"{name}.pattern must be 1-256 characters")
        outputs[name] = Output(value["enabled"], value["level"], pattern)
    path, days = config["file"]["path"], config["file"]["retention_days"]
    if not isinstance(path, str) or not path:
        raise invalid("file.path must be a path")
    if isinstance(days, bool) or not isinstance(days, int) or not 1 <= days <= 366:
        raise invalid("file.retention_days must be 1-366")
    return outputs["console"], outputs["file"], path, days


def render(pattern: str, level: str, program: str, message: str) -> str:
    now = datetime.datetime.now().astimezone()
    zone = now.strftime("%z")
    tokens = {"Y": now.strftime("%Y"), "m": now.strftime("%m"), "d": now.strftime("%d"),
              "H": now.strftime("%H"), "M": now.strftime("%M"), "S": now.strftime("%S"),
              "e": f"{now.microsecond // 1000:03d}", "z": f"{zone[:3]}:{zone[3:]}",
              "l": level, "n": program, "v": message, "%": "%"}
    return re.sub(r"%(.)", lambda match: tokens.get(match[1], match[0]), pattern)


class ProcessLog:
    def __init__(self, program: str, installation: Path):
        self.program = program
        self.console, self.file, self.base = Output(True), Output(False), None
        settings = installation / "config" / "logging.json"
        if not settings.exists():
            return
        self.console, self.file, path, days = read_settings(settings)
        if self.file.enabled:
            path = path.replace("{program}", program).replace("{pid}", str(os.getpid()))
            base = Path(path)
            if not base.is_absolute():
                base = installation / "bin" / base
            self.base = Path(os.path.normpath(base))
            self.base.parent.mkdir(parents=True, exist_ok=True)
            remove_expired(self.base.parent, days)

    def write(self, level: str, module: str, event: str, **fields):
        message = f"{module} {event}" + "".join(f" {key}={describe(value)}"
                                                for key, value in fields.items())
        if self.console.enabled and LEVELS[level] >= self.console.threshold:
            print(render(self.console.pattern, level, self.program, message), file=sys.stderr,
                  flush=True)
        if self.base and LEVELS[level] >= self.file.threshold:
            day = datetime.date.today().isoformat()
            path = self.base.with_name(f"{self.base.stem}_{day}{self.base.suffix}")
            with path.open("a", encoding="utf-8") as stream:
                stream.write(render(self.file.pattern, level, self.program, message) + "\n")


def describe(value) -> str:
    text = str(value)
    return json.dumps(text, ensure_ascii=False) if not text or re.search(r'[\s"=]', text) else text


def remove_expired(directory: Path, days: int):
    limit = datetime.date.today() - datetime.timedelta(days=days)
    for entry in directory.iterdir():
        match = DAILY.fullmatch(entry.name)
        if not match or entry.is_symlink() or not entry.is_file():
            continue
        try:
            if datetime.date(int(match[1]), int(match[2]), int(match[3])) < limit:
                entry.unlink()
        except ValueError:
            continue
