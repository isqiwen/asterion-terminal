from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout


class Asterion(ConanFile):
    name = "asterion"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"
    requires = "ctp/6.7.7", "nlohmann_json/3.12.0", "spdlog/1.17.0", "cli11/2.6.0", "protobuf/5.29.3", "openssl/3.6.2", "asio/1.30.2", "cpp-httplib/0.47.0"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True, "cpp-httplib/*:with_openssl": True, "spdlog/*:header_only": True, "fmt/*:header_only": True, "gtest/*:with_gmock": False, "protobuf/*:with_zlib": False}

    def validate(self):
        if str(self.settings.os) == "Linux" and str(self.settings.arch) != "x86_64":
            from conan.errors import ConanInvalidConfiguration
            raise ConanInvalidConfiguration("Linux currently supports x86_64 only")

    def build_requirements(self):
        if self.options.with_tests:
            self.test_requires("gtest/1.18.0")

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["BUILD_TESTING"] = bool(self.options.with_tests)
        toolchain.generate()
        from conan.tools.files import copy
        import os
        sdk = self.dependencies["ctp"]
        for library in ("ctp-md", "ctp-trader"):
            for directory in sdk.cpp_info.bindirs:
                copy(self, library + ".*", src=directory, dst=os.path.dirname(self.generators_folder))
            if str(self.settings.os) == "Macos":
                import subprocess
                subprocess.run(["codesign", "--force", "--sign", "-", os.path.join(os.path.dirname(self.generators_folder), library + ".dylib")], check=True)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
