"""Fatal failures leave a stack on disk and retain their fatal signal status."""
from pathlib import Path
from datetime import datetime, timezone
import re
import signal
import subprocess
import sys
import tempfile

for kind, expected, symbol in (("segfault", signal.SIGSEGV, "crash_site"),
                                ("exception", signal.SIGABRT, "fixture uncaught exception")):
    with tempfile.TemporaryDirectory() as directory:
        before = datetime.now(timezone.utc)
        child = subprocess.run([sys.argv[1], directory, kind], capture_output=True, timeout=15)
        after = datetime.now(timezone.utc)
        assert child.returncode == -expected, (child.returncode, child.stderr)
        files = list(Path(directory).glob("*-crash.frames"))
        assert len(files) == 1, files
        assert files[0].stat().st_mode & 0o777 == 0o600
        report = files[0].with_suffix(".report.log")
        rendered = report.read_text()
        assert "Asterion " in rendered and signal.Signals(expected).name in rendered, rendered
        assert "crash_process.cpp:" in rendered and symbol in rendered, rendered
        assert set(Path(directory).iterdir()) == {files[0], report}
        started = re.search(r"^started_at: (.+)$", rendered, re.MULTILINE)[1]
        crashed = re.search(r"^crashed_at: (.+)$", rendered, re.MULTILINE)[1]
        assert re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z", started), started
        earliest = before.replace(microsecond=(before.microsecond // 1000) * 1000)
        assert earliest <= datetime.fromisoformat(started) <= datetime.fromisoformat(crashed) <= after
        stamp = started.replace("-", "").replace(":", "")
        assert re.fullmatch(r"crash-test-" + re.escape(stamp) + r"-\d+-crash.frames", files[0].name)
        print(rendered)
        report.unlink()
        subprocess.run([sys.argv[1], directory, "recover"], check=True, capture_output=True, timeout=15)
        assert report.read_text() == rendered
        assert set(Path(directory).iterdir()) == {files[0], report}
