"""Desktop dev mode skips reconfiguration and restaging only when inputs and outputs match."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts/desktop"))
import desktop


class DesktopDev(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "conanfile.py").write_text("conanfile v1\n")
        (self.root / "conan.lock").write_text("lock v1\n")
        (self.root / "conan/ctp").mkdir(parents=True)
        (self.root / "conan/ctp/sources.json").write_text("{}\n")
        self.build = self.root / "build/Release"
        self.headers = self.root / "build/electron-sdk/44.4.5/include/node"
        self.profile = self.root / "build/local-profile"
        patched = patch.object(desktop, "ROOT", self.root)
        patched.start()
        self.addCleanup(patched.stop)

    def write_configured(self, compiler="Apple clang version 17"):
        (self.build / "generators").mkdir(parents=True, exist_ok=True)
        (self.build / "CMakeFiles/4.5").mkdir(parents=True, exist_ok=True)
        self.headers.mkdir(parents=True, exist_ok=True)
        (self.headers / "node_api.h").write_text("// header\n")
        (self.build / "build.ninja").write_text("# ninja\n")
        (self.build / "generators/conan_toolchain.cmake").write_text("# toolchain\n")
        (self.build / "CMakeFiles/4.5/CMakeCXXCompiler.cmake").write_text(
            'set(CMAKE_CXX_COMPILER "/usr/bin/clang++")\n'
        )
        (self.build / "CMakeCache.txt").write_text(
            f"ASTERION_NODE_HEADERS:PATH={self.headers}\n"
        )
        (self.root / "CMakeUserPresets.json").write_text(
            json.dumps({"include": ["build/Release/generators/CMakePresets.json"]})
        )
        state = desktop.configure_state("Release", "44.4.5", compiler, self.profile)
        (self.build / ".desktop-configure.json").write_text(json.dumps(state))
        return state

    def test_configure_current_invalidates_when_inputs_or_build_files_change(self):
        state = self.write_configured()
        self.assertTrue(desktop.configure_current(self.build, "Release", self.headers, state))

        # Conan lock or Electron version change forces reconfigure.
        (self.root / "conan.lock").write_text("lock v2\n")
        changed = desktop.configure_state("Release", "44.4.5", "Apple clang version 17", self.profile)
        self.assertFalse(desktop.configure_current(self.build, "Release", self.headers, changed))

        self.write_configured()
        upgraded = desktop.configure_state("Release", "45.0.0", "Apple clang version 17", self.profile)
        self.assertFalse(desktop.configure_current(self.build, "Release", self.headers, upgraded))

        # Missing Node headers or cleared CMake cache forces reconfigure.
        state = self.write_configured()
        (self.build / "CMakeCache.txt").write_text("ASTERION_NODE_HEADERS:PATH=\n")
        self.assertFalse(desktop.configure_current(self.build, "Release", self.headers, state))

    def test_dev_targets_include_engine_only_when_python_home_is_configured(self):
        self.build.mkdir(parents=True, exist_ok=True)
        (self.build / "CMakeCache.txt").write_text("ASTERION_NODE_HEADERS:PATH=/tmp\n")
        self.assertEqual(desktop.dev_targets(self.build),
                         ["asterion_terminal_node", "asterion_python_protocol"])
        (self.build / "CMakeCache.txt").write_text(
            "ASTERION_PYTHON_HOME:UNINITIALIZED=/opt/python3.12\n"
        )
        self.assertEqual(desktop.dev_targets(self.build),
                         ["asterion_terminal_node", "asterion_python_protocol", "asterion_engine"])

    def test_native_resources_current_detects_missing_or_modified_files(self):
        native = self.root / "build/electron-resources/native"
        (self.build / "config").mkdir(parents=True, exist_ok=True)
        for name in desktop.NATIVE_FILES:
            (self.build / name).write_bytes(f"v1:{name}".encode())
        self.assertFalse(desktop.native_resources_current(self.build, native))

        desktop.stage_native_resources(self.build, native)
        self.assertTrue(desktop.native_resources_current(self.build, native))

        (self.build / "asterion_terminal.node").write_bytes(b"v2:binary")
        self.assertFalse(desktop.native_resources_current(self.build, native))


if __name__ == "__main__":
    unittest.main()
