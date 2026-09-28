from conan import ConanFile
from conan.tools.files import copy
from conan.errors import ConanInvalidConfiguration
from pathlib import Path

class CtpMd(ConanFile):
    name = "ctp-md"
    version = "6.7.7"
    package_type = "shared-library"
    settings = "os", "arch"
    def package(self):
        root = self.conf.get("user.ctp:sdk_root", default="")
        if not root or not Path(root).is_dir():
            raise ConanInvalidConfiguration("Run scripts/prepare_ctp.py explicitly to stage the pinned SDK")
        copy(self, "*.h", src=root, dst=str(Path(self.package_folder) / "include"))
        for suffix in ("dylib", "so", "dll"):
            copy(self, "ctp-md." + suffix, src=root, dst=str(Path(self.package_folder) / "bin"))
        if str(self.settings.os) == "Macos":
            import subprocess
            source = str(Path(root) / "ctp-md.dylib")
            target = str(Path(self.package_folder) / "bin/ctp-md.dylib")
            subprocess.run(["lipo", source, "-thin", "arm64" if str(self.settings.arch) == "armv8" else "x86_64", "-output", target], check=True)
    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ctp-md")
        self.cpp_info.set_property("cmake_target_name", "ctp-md::ctp-md")
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = ["bin"]
