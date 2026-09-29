from conan import ConanFile
from conan.tools.files import copy
from conan.errors import ConanInvalidConfiguration
from pathlib import Path

class Ctp(ConanFile):
    # CTP 6.7.7 market (ctp-md) and trader (ctp-trader) APIs.
    name = "ctp"
    version = "6.7.7"
    package_type = "shared-library"
    settings = "os", "arch"
    def package(self):
        root = self.conf.get("user.ctp:sdk_root", default="")
        if not root or not Path(root).is_dir():
            raise ConanInvalidConfiguration("Run scripts/prepare_ctp.py explicitly to stage the pinned SDK")
        copy(self, "*.h", src=root, dst=str(Path(self.package_folder) / "include"))
        for library in ("ctp-md", "ctp-trader"):
            for suffix in ("dylib", "so", "dll"):
                copy(self, library + "." + suffix, src=root, dst=str(Path(self.package_folder) / "bin"))
            if str(self.settings.os) == "Macos":
                import subprocess
                source = str(Path(root) / (library + ".dylib"))
                target = str(Path(self.package_folder) / "bin" / (library + ".dylib"))
                subprocess.run(["lipo", source, "-thin", "arm64" if str(self.settings.arch) == "armv8" else "x86_64", "-output", target], check=True)
    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ctp")
        self.cpp_info.set_property("cmake_target_name", "ctp::ctp")
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = ["bin"]
