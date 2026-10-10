"""Build the C++ runtime and the Electron desktop on macOS or Linux."""
import hashlib
import os
import json
import platform
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import signal
from generated_resources import publish_tree
import macos_release

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
NATIVE_FILES = set(_resources["programs"]) | {name + LIBRARY_SUFFIX for name in _resources["libraries"]}
SERVICE_PROGRAMS = ("asterion-trading", "asterion-node-agent", "asterion-market-data", "asterion-data-service",
                    "asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline")

def verify_native_resources(directory):
    if directory.is_symlink():
        raise ValueError("Native resource directory cannot be a symlink")
    entries = list(directory.rglob("*"))
    actual = {path.relative_to(directory).as_posix() for path in entries if path.is_file()}
    if actual != NATIVE_FILES or any(path.is_symlink() or
            (path.is_dir() and path.relative_to(directory).as_posix() != "plugins") for path in entries):
        raise ValueError("Native resources differ from the current distribution manifest")


def stage_native_resources(build, destination):
    for name in NATIVE_FILES:
        source = build / name
        if source.is_symlink() or not source.is_file():
            raise ValueError("Missing or unsafe native resource: " + name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-stage-", dir=destination.parent) as temporary:
        prepared = Path(temporary) / "native"
        (prepared / "plugins").mkdir(parents=True)
        for name in sorted(NATIVE_FILES):
            stage_native(build / name, prepared / name)
        verify_native_resources(prepared)
        publish_tree(prepared, destination)


def exercise_packaged_programs(native):
    """Task recovery and both vendor SDK lifecycles, run by the packaged programs."""
    def packaged(programs):
        programs = {"ASTERION_NODE_AGENT_EXECUTABLE": "asterion-node-agent",
                    "ASTERION_TRADING_EXECUTABLE": "asterion-trading", **programs}
        return dict(env, **{variable: str(native / name) for variable, name in programs.items()})
    isolated = [sys.executable, str(ROOT / "tests/support/isolated_node.py"), sys.executable]
    run([*isolated, str(ROOT / "tests/tasks/task_agent.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")],
        environment=packaged({"ASTERION_DATA_SERVICE_EXECUTABLE": "asterion-data-service",
                              "ASTERION_TASK_EXECUTABLE": "asterion-task-service",
                              "ASTERION_BACKTEST_EXECUTABLE": "asterion-backtest",
                              "ASTERION_FACTOR_EXECUTABLE": "asterion-factor",
                              "ASTERION_DATA_PIPELINE_EXECUTABLE": "asterion-data-pipeline"}))
    run([*isolated, str(ROOT / "tests/market/ctp_sdk_smoke.py"), str(ROOT / "build/Release")],
        environment=packaged({"ASTERION_MARKET_EXECUTABLE": "asterion-market-data",
                              "ASTERION_CTP_LIBRARY": "ctp-md" + LIBRARY_SUFFIX,
                              "ASTERION_CTP_CATALOG_LIBRARY": "ctp-trader" + LIBRARY_SUFFIX}))


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
            raise SystemExit("Debian packaging requires dpkg-dev (dpkg-shlibdeps)")
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
        raise SystemExit("Debian package verification requires Linux")
    output = ROOT / "build" / ("desktop" if distribution else "desktop-test")
    packages = list(output.glob("*.deb"))
    if len(packages) != 1:
        raise SystemExit("Expected exactly one current Debian package; inspect build outputs")
    def field(name):
        return subprocess.check_output(["dpkg-deb", "--field", str(packages[0]), name], text=True).strip()
    version = json.loads((ROOT / "apps/clients/terminal/electron/package.json").read_text())["version"]
    if (field("Package"), field("Version"), field("Architecture")) != ("asterion-terminal", version, "amd64"):
        raise SystemExit("Debian package identity, version or architecture differs from this source")
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as folder:
        # Unpacked beside nothing else; the package is never installed here.
        run(["dpkg-deb", "--extract", str(packages[0]), folder])
        applications = [path for path in (Path(folder) / "opt").iterdir() if path.is_dir()]
        if len(applications) != 1:
            raise SystemExit("Expected exactly one application in the Debian package")
        app = applications[0]
        native = app / "resources/native"
        verify_native_resources(native)
        run([sys.executable, str(ROOT / "scripts/desktop/remote_resources.py"), "verify",
             "--directory", str(app / "resources/remote-linux")])
        # The bundled remote services run on other machines, not from this package.
        local = [path for path in elf_files(app) if app / "resources/remote-linux" not in path.parents]
        missing = set(debian_dependencies(local, [app])) - set(field("Depends").split(", "))
        if missing:
            raise SystemExit("Debian package does not declare what its programs need: " + ", ".join(sorted(missing)))
        for name in SERVICE_PROGRAMS:
            run([str(native / name), "--version"])
        exercise_packaged_programs(native)
    print("Debian package identity, declared dependencies, packaged task recovery and "
          "vendor market/trader lifecycles verified")


def write_linux_evidence(package, installed_report):
    acceptance = json.loads(installed_report.read_text())
    with package.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if acceptance.get("status") != "passed" or acceptance.get("sha256") != digest:
        raise ValueError("Installed acceptance does not prove this exact package")
    report = {"status": "passed", "architecture": platform.machine(), "sha256": digest,
              "installer": package.name, "installed_acceptance": str(installed_report),
              "depends": subprocess.check_output(["dpkg-deb", "--field", str(package), "Depends"], text=True).strip(),
              "checks": ["package identity, version and architecture", "exact native resource set",
                         "declared dependencies cover every packaged program",
                         "bundled remote Linux services", "packaged task recovery and vendor SDK lifecycles",
                         "unpacked application acceptance"],
              "limits": ["unsigned package", "not installed through dpkg; maintainer scripts not exercised"]}
    (package.parent / "distribution-acceptance.json").write_text(json.dumps(report, indent=2) + "\n")


def verify_macos_bundle(distribution=True):
    if sys.platform != "darwin":
        raise SystemExit("DMG verification requires macOS")
    output = ROOT / "build" / ("desktop" if distribution else "desktop-test")
    bundles = list(output.glob("*.dmg"))
    if len(bundles) != 1:
        raise SystemExit("Expected exactly one current DMG; inspect build outputs")
    if distribution:
        macos_release.verify_installer(bundles[0])
    run(["hdiutil", "verify", str(bundles[0])])
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as folder:
        run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", folder, str(bundles[0])])
        try:
            apps = list(Path(folder).glob("*.app"))
            if len(apps) != 1:
                raise SystemExit("Expected exactly one application in DMG")
            app = apps[0]
            verify_native_resources(app / "Contents/Resources/native")
            if distribution:
                macos_release.verify_application(app, NATIVE_FILES)
            minimum = "13.0"
            for program in (app / "Contents/Resources/native").iterdir():
                if program.is_file():
                    verify_macos_target(program, minimum)
            verify_macos_target(app / "Contents/Resources/native/ctp-md.dylib", minimum)
            run(["codesign", "--verify", "--deep", "--strict", str(app)])
            run([sys.executable, str(ROOT / "scripts/desktop/remote_resources.py"), "verify", "--directory", str(app / "Contents/Resources/remote-linux")])
            trading = app / "Contents/Resources/native/asterion-trading"
            if not trading.is_file():
                raise SystemExit("DMG does not contain the trading sidecar")
            run(["codesign", "--verify", "--strict", str(trading)])
            run([str(trading), "--version"])
            node = app / "Contents/Resources/native/asterion-node-agent"
            run(["codesign", "--verify", "--strict", str(node)])
            run([str(node), "--version"])
            market = app / "Contents/Resources/native/asterion-market-data"
            run(["codesign", "--verify", "--strict", str(market)])
            run([str(market), "--version"])
            for name in ("asterion-data-service", "asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline"):
                program = app / "Contents/Resources/native" / name
                run(["codesign", "--verify", "--strict", str(program)])
                run([str(program), "--version"])
            library = app / "Contents/Resources/native/ctp-md.dylib"
            run(["codesign", "--verify", "--strict", str(library)])
            exercise_packaged_programs(app / "Contents/Resources/native")
        finally:
            run(["hdiutil", "detach", folder])
    print("DMG checksum, signatures, packaged task recovery and vendor market/trader lifecycles verified")


def main():
    mode = sys.argv[1] if len(sys.argv) == 2 else "dev"
    if mode not in {"dev", "build", "package-test", "check", "verify", "verify-test"}:
        raise SystemExit("Usage: desktop.py dev|build|package-test|check|verify|verify-test")
    if sys.platform not in {"darwin", "linux"}:
        raise SystemExit("Terminal desktop development and packaging require macOS or Linux")
    verify_installer = verify_macos_bundle if MACOS else verify_linux_package
    if mode in {"verify", "verify-test"}:
        verify_installer(mode == "verify")
        return
    if mode == "build" and MACOS:
        macos_release.signing_preflight(env)
    resources = ROOT / "build/electron-resources/remote-linux"
    archives = Path(env.get("ASTERION_LINUX_BUNDLES", str(ROOT / "build/linux-bundles")))
    if mode in {"build", "package-test"} and not (archives / "asterion-services-linux-x86_64.zip").is_file():
        raise SystemExit(
            "Missing required Linux x86_64 release bundle: "
            + str(archives / "asterion-services-linux-x86_64.zip")
            + "\nObtain the matching source revision's remote-linux CI artifact and place it "
            "in this directory, or set ASTERION_LINUX_BUNDLES to its directory. "
            "Desktop packaging requires the bundled remote services."
        )
    stage = [sys.executable, str(ROOT / "scripts/desktop/remote_resources.py"), "stage", "--archives", str(archives)]
    if mode == "dev" and archives.is_dir():
        # Development must not block on the cross-built Linux bundle. A stale
        # bundle is never staged: remote Linux deployment reports it missing.
        if subprocess.run(stage, cwd=ROOT, env=env).returncode != 0:
            shutil.rmtree(resources, ignore_errors=True)
            resources.mkdir(parents=True, exist_ok=True)
            print("WARNING: Linux service bundle does not match this source; "
                  "remote Linux deployment is unavailable in this dev session. "
                  "Rebuild it before desktop:check/build.", file=sys.stderr)
    elif mode != "dev" or archives.is_dir():
        run(stage)
    else:
        resources.mkdir(parents=True, exist_ok=True)
    env["ASTERION_REMOTE_RESOURCES"] = str(resources)
    profile = ROOT / "build/local-profile"
    configuration = "Release" if mode in {"build", "package-test"} else "Debug"
    preset = "conan-" + configuration.lower()
    install = ["conan", "install", ".", "-s", "compiler.cppstd=20", "-c", "tools.cmake.cmaketoolchain:generator=Ninja", "--build=missing"]
    if profile.exists():
        install += ["-pr:h", str(profile), "-pr:b", str(profile)]
    # Desktop dependencies and flags come from Conan, not shell-wide
    # overrides that can link the app to libraries outside the lock.
    for variable in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS"):
        env.pop(variable, None)
    configure = ["cmake", "--preset", preset]
    if MACOS:
        install += ["-s", "build_type=" + configuration]
        compiler = subprocess.check_output(["/usr/bin/clang++", "--version"], env=env, text=True)
        if not compiler.startswith("Apple clang version "):
            raise SystemExit("macOS desktop builds require the system Apple Clang toolchain")
        compiler_version = compiler.split()[3].split(".")[0]
        executables = json.dumps({"c": "/usr/bin/clang", "cpp": "/usr/bin/clang++"})
        for context in ("h", "b"):
            install += ["-s:" + context, "compiler=apple-clang",
                        "-s:" + context, "compiler.version=" + compiler_version,
                        "-s:" + context, "compiler.libcxx=libc++",
                        "-c:" + context, "tools.build:compiler_executables=" + executables]
        minimum = "13.0"
        env["MACOSX_DEPLOYMENT_TARGET"] = minimum
        install += ["-s:h", "os.version=" + minimum, "-s:b", "os.version=" + minimum]
        sdk_arch = "armv8" if platform.machine() == "arm64" else "x86_64"
        sdk_root = ROOT / "build/ctp-sdk" / ("macos-" + sdk_arch)
        install += ["-c", "user.ctp:sdk_root=" + str(sdk_root)]
        # CMake's compiler detection survives toolchain changes. Reset only
        # when an earlier configure used a different compiler.
        compiler_records = (ROOT / "build" / configuration / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")
        if any('set(CMAKE_CXX_COMPILER "/usr/bin/clang++")' not in file.read_text()
               for file in compiler_records):
            configure.append("--fresh")
        # Reinitialize managed flags without deleting compiled object files.
        # Conan's cache default does not replace a previous deployment target.
        configure += ["-UCMAKE_*_FLAGS*", "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + minimum]
    else:
        # Linux uses the Conan profile's compiler. Dependencies build optimized:
        # embedded DuckDB with debug information exceeds the Agent artifact limit.
        install += ["-s", "&:build_type=" + configuration, "-s", "build_type=Release"]
    run(install)
    electron_version = json.loads((ROOT / "node_modules/electron/package.json").read_text())["version"]
    sdk = ROOT / "build/electron-sdk"
    run(["pnpm", "exec", "node-gyp", "install", "--ensure", "--target=" + electron_version,
         "--dist-url=https://electronjs.org/headers", "--devdir=" + str(sdk)])
    # The compile database is what editors' C++ language servers read.
    configure += ["-DASTERION_NODE_HEADERS=" + str(sdk / electron_version / "include/node"),
                  "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    run(configure)
    run(["cmake", "--build", "--preset", preset])
    env["ASTERION_CPP_BUILD"] = str(ROOT / "build" / configuration)
    native = ROOT / "build/electron-resources/native"
    stage_native_resources(ROOT / "build" / configuration, native)
    run(["pnpm", "build"])
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
    # Packages are built for acceptance and handed over by hand. On CI the
    # builder would otherwise try to publish them and fail without a token.
    builder = ["pnpm", "exec", "electron-builder", "--publish", "never",
               "--mac" if MACOS else "--linux",
               "--config", "apps/clients/terminal/electron/builder.cjs"]
    env["ASTERION_PACKAGE_MODE"] = "release" if mode == "build" else "test"
    if not MACOS:
        # What the Electron runtime and the native programs link against on this
        # host decides which systems the package installs on.
        runtime = Path(json.loads(subprocess.check_output(
            ["node", "-p", "JSON.stringify(require('./scripts/desktop/electron-path.cjs')())"],
            cwd=ROOT, env=env, text=True))).parent
        env["ASTERION_DEB_DEPENDS"] = json.dumps(
            debian_dependencies(elf_files(runtime) + elf_files(native), [runtime]))
    output = ROOT / "build" / ("desktop" if mode == "build" else "desktop-test")
    if output.exists():
        if output.is_symlink() or not output.is_dir():
            raise ValueError("Desktop output must be a generated directory")
        shutil.rmtree(output)
    run(builder)
    installers = list(output.glob("*.dmg" if MACOS else "*.deb"))
    if len(installers) != 1:
        raise ValueError("Expected exactly one current installer")
    if mode == "build" and MACOS:
        macos_release.notarize_installer(installers[0], env)
    verify_installer(mode == "build")
    if mode == "build":
        evidence = output / "installed"
        run([sys.executable, str(ROOT / "tests/desktop/electron_installer.py"), str(installers[0]),
             "--output", str(evidence)])
        write_evidence = macos_release.write_evidence if MACOS else write_linux_evidence
        write_evidence(installers[0], evidence / "acceptance.json")
    print(f"Desktop installer: {installers[0]}")


if __name__ == "__main__":
    main()
