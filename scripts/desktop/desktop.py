"""Build the C++ runtime and the Electron desktop on macOS or Linux."""
import argparse
import hashlib
import os
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import signal
from generated_resources import publish_tree
import macos_release
import python_runtime
import runtime_manifest
import crash_resources

ROOT = Path(__file__).resolve().parents[2]
env = os.environ.copy()
# Direct Python entry must resolve the root package tools from app subdirectories.
env["PATH"] = str(ROOT / "node_modules/.bin") + os.pathsep + env["PATH"]


def run(args, cwd=ROOT, environment=None):
    executable = shutil.which(args[0], path=env["PATH"])
    if executable is None:
        raise SystemExit(f"Required build tool not found: {args[0]}")
    subprocess.run([executable, *args[1:]], cwd=cwd, env=environment or env, check=True)


def run_development():
    child = subprocess.Popen([shutil.which("node", path=env["PATH"]), str(ROOT / "scripts/desktop/electron-dev.cjs")],
                             cwd=ROOT, env=env, start_new_session=True)
    def forward(signum, _frame):
        if child.poll() is None:
            child.send_signal(signum)
    previous = {signum: signal.signal(signum, forward) for signum in (signal.SIGINT, signal.SIGTERM)}
    try:
        result = child.wait()
        if result:
            raise SystemExit(result)
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def stage_native(source, destination):
    # Replacing the inode avoids macOS retaining code-signature pages from a
    # previously loaded addon when rebuilding into the same resource directory.
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
        staged = Path(output.name)
    try:
        shutil.copy2(source, staged)
        os.replace(staged, destination)
    finally:
        staged.unlink(missing_ok=True)


def verify_macos_target(program, minimum):
    details = subprocess.check_output(["/usr/bin/vtool", "-show-build", str(program)], text=True)
    versions = re.findall(r"^\s*minos\s+([0-9.]+)\s*$", details, re.MULTILINE)
    def version(value):
        numbers = tuple(int(part) for part in value.split("."))
        return numbers + (0,) * (3 - len(numbers))
    if not versions or any(version(item) > version(minimum) for item in versions):
        raise SystemExit(f"{program.name} requires macOS {versions or 'unknown'}, above the declared minimum {minimum}")


MACOS = sys.platform == "darwin"
LIBRARY_SUFFIX = ".dylib" if MACOS else ".so"
# Programs keep their name; shared libraries take the platform's suffix.
_resources = json.loads((ROOT / "scripts/desktop/native-resources.json").read_text())
CONFIG_FILES = set(_resources["configuration"])
NATIVE_FILES = CONFIG_FILES | set(_resources["programs"]) | {name + LIBRARY_SUFFIX for name in _resources["libraries"]}

def verify_native_resources(directory):
    if directory.is_symlink():
        raise ValueError("Native resource directory cannot be a symlink")
    entries = list(directory.rglob("*"))
    actual = {path.relative_to(directory).as_posix() for path in entries if path.is_file()}
    if actual != NATIVE_FILES or any(path.is_symlink() or
            (path.is_dir() and path.relative_to(directory).as_posix() != "config") for path in entries):
        raise ValueError("Native resources differ from the current distribution manifest")


def file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def native_resources_current(build: Path, destination: Path) -> bool:
    try:
        verify_native_resources(destination)
    except ValueError:
        return False
    return all(
        (build / name).is_file() and not (build / name).is_symlink()
        and file_sha256(build / name) == file_sha256(destination / name)
        for name in NATIVE_FILES
    )


def configure_state(configuration: str, electron_version: str, compiler: str, profile: Path) -> dict:
    inputs = [ROOT / "conanfile.py", ROOT / "conan.lock",
              *sorted(path for path in (ROOT / "conan").rglob("*") if path.is_file())]
    if profile.is_file():
        inputs.append(profile)
    return {
        "configuration": configuration,
        "electron": electron_version,
        "compiler": compiler,
        "files": {path.relative_to(ROOT).as_posix(): file_sha256(path) for path in inputs},
    }


def configure_current(build: Path, configuration: str, node_headers: Path, state: dict) -> bool:
    stamp = build / ".desktop-configure.json"
    cache = build / "CMakeCache.txt"
    presets = ROOT / "CMakeUserPresets.json"
    if not stamp.is_file() or not (build / "build.ninja").is_file() \
            or not (build / "generators/conan_toolchain.cmake").is_file() \
            or not (node_headers / "node_api.h").is_file() \
            or not cache.is_file() or not presets.is_file():
        return False
    try:
        if json.loads(stamp.read_text()) != state:
            return False
    except (OSError, ValueError):
        return False
    if f"build/{configuration}/generators/CMakePresets.json" not in presets.read_text():
        return False
    if f"ASTERION_NODE_HEADERS:PATH={node_headers}\n" not in cache.read_text():
        return False
    if MACOS:
        records = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        if not records or any('set(CMAKE_CXX_COMPILER "/usr/bin/clang++")' not in path.read_text()
                              for path in records):
            return False
    return True


def has_python_home(build: Path) -> bool:
    cache = build / "CMakeCache.txt"
    return cache.is_file() and bool(
        re.search(r"^ASTERION_PYTHON_HOME:[^=]*=\S+", cache.read_text(), re.MULTILINE)
    )


def dev_targets(build: Path) -> list[str]:
    targets = ["asterion_terminal_node", "asterion_python_protocol"]
    if has_python_home(build):
        targets.append("asterion_engine")
    return targets


def repair_ninja_deps(build: Path):
    # An interrupted build can leave a truncated .ninja_deps that forces full rebuilds until recompacted.
    if (build / ".ninja_deps").is_file():
        run(["ninja", "-C", str(build), "-t", "recompact"])


def stage_native_resources(build, destination):
    for name in NATIVE_FILES:
        source = build / name
        if source.is_symlink() or not source.is_file():
            raise ValueError("Missing or unsafe native resource: " + name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-stage-", dir=destination.parent) as temporary:
        prepared = Path(temporary) / "native"
        (prepared / "config").mkdir(parents=True)
        for name in sorted(NATIVE_FILES):
            if name in CONFIG_FILES:
                shutil.copyfile(build / name, prepared / name)
            else:
                stage_native(build / name, prepared / name)
        verify_native_resources(prepared)
        publish_tree(prepared, destination)


def elf_files(directory):
    return [path for path in sorted(directory.rglob("*"))
            if path.is_file() and not path.is_symlink() and path.open("rb").read(4) == b"\x7fELF"]


def debian_dependencies(files, libraries):
    """Packages and minimum versions the ELF files need, from this host's package database.

    Libraries shipped beside the programs are found in `libraries` and need no package.
    """
    with tempfile.TemporaryDirectory(prefix="asterion-shlibdeps-") as temporary:
        (Path(temporary) / "debian").mkdir()
        (Path(temporary) / "debian/control").write_text(
            "Source: asterion-terminal\n\nPackage: asterion-terminal\nArchitecture: any\n"
            "Description: Asterion Terminal\n")
        executable = shutil.which("dpkg-shlibdeps", path=env["PATH"])
        if executable is None:
            raise SystemExit("Runtime dependency analysis requires dpkg-dev (dpkg-shlibdeps)")
        output = subprocess.check_output(
            [executable, "-O", "--ignore-missing-info", *["-l" + str(path) for path in libraries],
             *["-e" + str(path) for path in files]],
            cwd=temporary, env=env, text=True, stderr=subprocess.DEVNULL)
    records = [line.removeprefix("shlibs:Depends=") for line in output.splitlines()
               if line.startswith("shlibs:Depends=")]
    if len(records) != 1 or not records[0]:
        raise SystemExit("Cannot determine the package's runtime dependencies")
    return sorted(records[0].split(", "))


def verify_linux_package(distribution=True):
    if sys.platform != "linux":
        raise SystemExit("Package verification requires Linux")
    output = ROOT / "build" / ("desktop" if distribution else "desktop-test")
    packages = list(output.glob("*.deb"))
    if len(packages) != 1:
        raise SystemExit("Expected exactly one current package; inspect build outputs")
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as folder:
        run(["dpkg-deb", "-x", str(packages[0]), folder])
        apps = list(Path(folder).rglob("asterion_terminal"))
        if len(apps) != 1:
            raise SystemExit("Expected exactly one Terminal program in the package")
        app = apps[0].parent
        verify_native_resources(app / "resources/native")
        runtime_manifest.verify(app / "resources/runtime")
        declared = subprocess.check_output(["dpkg-deb", "-f", str(packages[0]), "Depends"], text=True)
        missing = set(debian_dependencies(elf_files(app), [app])) - set(declared.strip().split(", "))
        if missing:
            raise SystemExit("Package dependencies are incomplete: " + ", ".join(sorted(missing)))
    print("Package resources, Python runtime manifest and dependencies verified")


def verify_macos_bundle(distribution=True):
    if sys.platform != "darwin":
        raise SystemExit("DMG verification requires macOS")
    output = ROOT / "build" / ("desktop" if distribution else "desktop-test")
    bundles = list(output.glob("*.dmg"))
    if len(bundles) != 1:
        raise SystemExit("Expected exactly one current DMG; inspect build outputs")
    if distribution:
        macos_release.verify_dmg(bundles[0])
    run(["hdiutil", "verify", str(bundles[0])])
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as folder:
        run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", folder, str(bundles[0])])
        try:
            apps = list(Path(folder).glob("*.app"))
            if len(apps) != 1:
                raise SystemExit("Expected exactly one application in DMG")
            app = apps[0]
            verify_native_resources(app / "Contents/Resources/native")
            runtime_manifest.verify(app / "Contents/Resources/runtime")
            if distribution:
                macos_release.verify_application(app, NATIVE_FILES - CONFIG_FILES)
            minimum = "13.0"
            for program in (app / "Contents/Resources/runtime").rglob("*"):
                if program.is_file():
                    with program.open("rb") as stream:
                        magic = stream.read(4)
                    if magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
                        verify_macos_target(program, minimum)
            for program in (app / "Contents/Resources/native").iterdir():
                if program.is_file() and program.name not in CONFIG_FILES:
                    verify_macos_target(program, minimum)
            run(["codesign", "--verify", "--deep", "--strict", str(app)])
        finally:
            run(["hdiutil", "detach", folder])
    print("DMG checksum and signatures verified")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", nargs="?", default="dev", choices=("dev", "build", "package-test", "check", "sync", "verify", "verify-test"))
    parser.add_argument("--prepare-python", action="store_true", help="Explicitly download the locked Python runtime and its dependencies")
    parser.add_argument("--debug", action="store_true", help="Use a Debug build for dev, check or sync")
    arguments = parser.parse_args()
    mode = arguments.mode
    if arguments.debug and mode not in {"dev", "check", "sync"}:
        parser.error("--debug is only available for dev, check and sync")
    if sys.platform not in {"darwin", "linux"}:
        raise SystemExit("Terminal desktop development and packaging require macOS or Linux")
    verify_package = verify_macos_bundle if MACOS else verify_linux_package
    if mode in {"verify", "verify-test"}:
        verify_package(mode == "verify")
        return
    if mode == "sync":
        run(["conan", "profile", "detect", "--exist-ok"])
        run([sys.executable, str(ROOT / "scripts/prepare_ctp.py")])
        run(["pnpm", "install", "--frozen-lockfile"])
        run(["pnpm", "exec", "install-electron"])
    if mode == "build" and MACOS:
        macos_release.signing_preflight(env)
    profile = ROOT / "build/local-profile"
    configuration = "Debug" if arguments.debug else "Release"
    preset = "conan-" + configuration.lower()
    build = ROOT / "build" / configuration
    # Desktop dependencies and flags come from Conan, not shell-wide
    # overrides that can link the app to libraries outside the lock.
    for variable in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS"):
        env.pop(variable, None)
    electron_version = json.loads((ROOT / "node_modules/electron/package.json").read_text())["version"]
    sdk = ROOT / "build/electron-sdk"
    node_headers = sdk / electron_version / "include/node"
    compiler = ""
    if MACOS:
        compiler = subprocess.check_output(["/usr/bin/clang++", "--version"], env=env, text=True)
        if not compiler.startswith("Apple clang version "):
            raise SystemExit("macOS desktop builds require the system Apple Clang toolchain")
        env["MACOSX_DEPLOYMENT_TARGET"] = "13.0"
    state = configure_state(configuration, electron_version, compiler, profile)
    if mode != "dev" or not configure_current(build, configuration, node_headers, state):
        install = ["conan", "install", ".", "-s", "compiler.cppstd=20",
                   "-c", "tools.cmake.cmaketoolchain:generator=Ninja", "--build=missing"]
        if profile.exists():
            install += ["-pr:h", str(profile), "-pr:b", str(profile)]
        configure = ["cmake", "--preset", preset]
        if MACOS:
            install += ["-s", "build_type=" + configuration]
            compiler_version = compiler.split()[3].split(".")[0]
            executables = json.dumps({"c": "/usr/bin/clang", "cpp": "/usr/bin/clang++"})
            for context in ("h", "b"):
                install += ["-s:" + context, "compiler=apple-clang",
                            "-s:" + context, "compiler.version=" + compiler_version,
                            "-s:" + context, "compiler.libcxx=libc++",
                            "-c:" + context, "tools.build:compiler_executables=" + executables]
            minimum = "13.0"
            install += ["-s:h", "os.version=" + minimum, "-s:b", "os.version=" + minimum]
            install += ["-c", "user.ctp:sdk_root=" + str(ROOT / "build/ctp-sdk/macos-armv8")]
            # CMake's compiler detection survives toolchain changes. Reset only
            # when an earlier configure used a different compiler.
            compiler_records = (build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")
            if any('set(CMAKE_CXX_COMPILER "/usr/bin/clang++")' not in file.read_text()
                   for file in compiler_records):
                configure.append("--fresh")
            # Reinitialize managed flags without deleting compiled object files.
            # Conan's cache default does not replace a previous deployment target.
            configure += ["-UCMAKE_*_FLAGS*", "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + minimum]
        else:
            # Linux uses the Conan profile's compiler. Dependencies build optimized.
            install += ["-s", "&:build_type=" + configuration, "-s", "build_type=Release"]
            # Conan's prebuilt cpptrace lacks the host glibc's signal-safe _dl_find_object.
            # On a CI Conan cache hit for this runner image, the cached package was built from source.
            if env.get("ASTERION_CONAN_CACHE_HIT") != "1":
                install += ["--build=cpptrace/*"]
        # Third-party dependencies live in the Conan cache; keep them out of ccache.
        conan_env = {k: v for k, v in env.items()
                     if k not in ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER")}
        run(install, environment=conan_env)
        run(["pnpm", "exec", "node-gyp", "install", "--ensure", "--target=" + electron_version,
             "--dist-url=https://electronjs.org/headers", "--devdir=" + str(sdk)])
        # The compile database is what editors' C++ language servers read.
        configure += ["-DASTERION_NODE_HEADERS=" + str(node_headers),
                      "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        run(configure)
        (build / ".desktop-configure.json").write_text(json.dumps(state, indent=2) + "\n")
    repair_ninja_deps(build)
    if mode == "dev":
        run(["cmake", "--build", "--preset", preset, "--target", *dev_targets(build)])
    else:
        run(["cmake", "--build", "--preset", preset])
    if mode == "sync":
        if arguments.prepare_python or not has_python_home(build) \
                or not list((build / "engine-python").glob("asterion_engine.*")):
            python_runtime.prepare(build)
        else:
            python_runtime.ensure(build)
        return
    env["ASTERION_CPP_BUILD"] = str(build)
    native = ROOT / "build/electron-resources/native"
    native_changed = mode != "dev" or not native_resources_current(build, native)
    if native_changed:
        stage_native_resources(build, native)
    if MACOS:
        symbols = ROOT / "build/electron-resources/symbols"
        dsym = symbols / "asterion_terminal.node.dSYM"
        if native_changed or not dsym.is_dir():
            symbols.mkdir(parents=True, exist_ok=True)
            run(["dsymutil", str(build / "asterion_terminal.node"), "-o", str(dsym)])
    crash_resources.prepare(build)
    if MACOS and native_changed:
        verify_macos_target(crash_resources.DESTINATION / "minidump-stackwalk", "13.0")
    if arguments.prepare_python or not has_python_home(build) \
            or not list((build / "engine-python").glob("asterion_engine.*")):
        python_runtime.prepare(build)
    else:
        python_runtime.ensure(build)
    # Development runs on the prepared runtime; a package ships the manifest
    # its first launch installs from.
    runtime_manifest.verify()
    if mode != "dev":
        run(["pnpm", "build"])
    if native_changed:
        # Loading and calling the compiled addon is part of desktop:check.
        run([sys.executable, str(ROOT / "tests/support/isolated_node.py"), "node",
             str(ROOT / "tests/desktop/electron_bridge.cjs"), str(native / "asterion_terminal.node")])
    if mode == "check":
        run(["node", "--check", "apps/clients/terminal/electron/main.cjs"])
        run(["node", "--check", "apps/clients/terminal/electron/preload.cjs"])
        return
    if mode == "dev":
        run_development()
        return
    # CI uploads packages as artifacts; electron-builder must not publish them.
    builder = ["pnpm", "exec", "electron-builder", "--publish", "never",
               "--mac" if MACOS else "--linux", "--arm64" if MACOS else "--x64",
               "--config", "apps/clients/terminal/electron/builder.cjs"]
    env["ASTERION_PACKAGE_MODE"] = "release" if mode == "build" else "test"
    # CI packages use faster compression.
    env["ELECTRON_BUILDER_COMPRESSION_LEVEL"] = "9" if mode == "build" else "1"
    if not MACOS:
        # What the Electron runtime and the native programs link against on this
        # host decides which systems the package installs on.
        runtime = Path(json.loads(subprocess.check_output(
            ["node", "-p", "JSON.stringify(require('./scripts/desktop/electron-path.cjs')())"],
            cwd=ROOT, env=env, text=True))).parent
        env["ASTERION_DEB_DEPENDS"] = json.dumps(
            debian_dependencies(elf_files(runtime) + elf_files(native) + elf_files(runtime_manifest.DESTINATION),
                                [runtime]))
    output = ROOT / "build" / ("desktop" if mode == "build" else "desktop-test")
    if output.exists():
        if output.is_symlink() or not output.is_dir():
            raise ValueError("Desktop output must be a generated directory")
        shutil.rmtree(output)
    run(builder)
    packages = list(output.glob("*.dmg" if MACOS else "*.deb"))
    if len(packages) != 1:
        raise ValueError("Expected exactly one current package")
    if mode == "build" and MACOS:
        macos_release.notarize_dmg(packages[0], env)
    verify_package(mode == "build")
    print(f"Terminal package: {packages[0]}")


if __name__ == "__main__":
    main()
