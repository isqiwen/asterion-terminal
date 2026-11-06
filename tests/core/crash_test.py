"""Fatal failures leave a stack on disk and retain their fatal signal status."""
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

for kind, expected, symbol in (("segfault", signal.SIGSEGV, "crash_site"),
                                ("exception", signal.SIGABRT, "fixture uncaught exception")):
    with tempfile.TemporaryDirectory() as directory:
        child = subprocess.run([sys.argv[1], directory, kind], capture_output=True, timeout=15)
        assert child.returncode == -expected, (child.returncode, child.stderr)
        files = list(Path(directory).glob("*-crash.log"))
        assert len(files) == 1, files
        text = files[0].read_text()
        assert "Asterion " in text and "stack (address function+offset):" in text, text
        assert signal.Signals(expected).name in text and symbol in text, text
        assert files[0].stat().st_mode & 0o777 == 0o600
        report = files[0].with_name(files[0].stem + ".report.log")
        rendered = report.read_text()
        assert "crash_process.cpp:" in rendered and symbol in rendered, rendered
        print(rendered.split("modules (/proc/self/maps):")[0])
        report.unlink()
        subprocess.run([sys.argv[1], directory, "recover"], check=True, capture_output=True, timeout=15)
        assert report.read_text() == rendered
