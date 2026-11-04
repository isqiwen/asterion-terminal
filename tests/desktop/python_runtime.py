"""Desktop startup prepares outdated Python resources and reuses current ones."""
import json
from pathlib import Path
import platform
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/desktop"))
import python_runtime as runtime


class Startup(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.project, self.destination, self.build = root / "lab", root / "python", root / "build"
        self.project.mkdir()
        (self.project / "uv.lock").write_text("locked dependencies")
        for name, value in (("PROJECT", self.project), ("DESTINATION", self.destination)):
            patched = patch.object(runtime, name, value)
            patched.start()
            self.addCleanup(patched.stop)

    def prepare(self, build):
        site = self.destination / runtime.SITE
        site.mkdir(parents=True, exist_ok=True)
        executable = runtime.executable(self.destination)
        executable.parent.mkdir(exist_ok=True)
        executable.touch(mode=0o755)
        (self.destination / "manifest.json").write_text(json.dumps({
            "platform": [sys.platform, platform.machine()], "python": runtime.PYTHON}))
        (site / "asterion-source.pth").write_text(runtime.source_path(build))
        (self.destination / "uv.lock").write_bytes((self.project / "uv.lock").read_bytes())

    def test_startup_prepares_missing_or_outdated_runtime(self):
        for state in ("missing", "copied-code", "dependencies", "build"):
            with self.subTest(state=state):
                if state != "missing":
                    self.prepare(self.build)
                    if state == "copied-code":
                        (self.destination / runtime.SITE / "asterion-source.pth").unlink()
                    elif state == "dependencies":
                        (self.project / "uv.lock").write_text("updated dependencies")
                    else:
                        self.prepare(self.build / "other")
                with patch.object(runtime, "prepare", side_effect=self.prepare) as prepare:
                    runtime.ensure(self.build)
                    prepare.assert_called_once_with(self.build)
                runtime.verify(self.destination, self.build)

    def test_source_changes_do_not_reinstall_dependencies(self):
        self.prepare(self.build)
        (self.project / "repository.py").write_text("# changed source\n")
        with patch.object(runtime, "prepare") as prepare:
            runtime.ensure(self.build)
            prepare.assert_not_called()


if __name__ == "__main__":
    unittest.main()
