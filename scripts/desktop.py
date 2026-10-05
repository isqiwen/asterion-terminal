"""Build the C++ runtime and macOS Electron desktop."""
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

ROOT = Path(__file__).resolve().parents[1]
env = os.environ.copy()
# Direct Python entry must resolve the root package tools from app subdirectories.
env["PATH"] = str(ROOT / "node_modules/.bin") + os.pathsep + env["PATH"]


def run(args, cwd=ROOT):
    executable = shutil.which(args[0], path=env["PATH"])
    if executable is None:
        raise SystemExit(f"Required build tool not found: {args[0]}")
    subprocess.run([executable, *args[1:]], cwd=cwd, env=env, check=True)


def run_development():
    child = subprocess.Popen([shutil.which("node", path=env["PATH"]), str(ROOT / "scripts/electron-dev.cjs")],
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


NATIVE_FILES = set(json.loads((ROOT / "scripts/native-resources.json").read_text())["files"])

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
            run([sys.executable, str(ROOT / "scripts/remote_resources.py"), "verify", "--directory", str(app / "Contents/Resources/remote-linux")])
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
            previous_node = env.get("ASTERION_NODE_AGENT_EXECUTABLE")
            env["ASTERION_NODE_AGENT_EXECUTABLE"] = str(node)
            previous = env.get("ASTERION_TRADING_EXECUTABLE")
            try:
                env["ASTERION_TRADING_EXECUTABLE"] = str(trading)
                data_task_variables = {"ASTERION_DATA_SERVICE_EXECUTABLE": "asterion-data-service", "ASTERION_TASK_EXECUTABLE": "asterion-task-service", "ASTERION_BACKTEST_EXECUTABLE": "asterion-backtest", "ASTERION_FACTOR_EXECUTABLE": "asterion-factor", "ASTERION_DATA_PIPELINE_EXECUTABLE": "asterion-data-pipeline"}
                data_task_saved = {name: env.get(name) for name in data_task_variables}
                try:
                    for name, program in data_task_variables.items():
                        env[name] = str(app / "Contents/Resources/native" / program)
                    run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/task_agent.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")])
                finally:
                    for name, value in data_task_saved.items():
                        if value is None:
                            env.pop(name, None)
                        else:
                            env[name] = value
                saved = {name: env.get(name) for name in ("ASTERION_MARKET_EXECUTABLE", "ASTERION_CTP_LIBRARY", "ASTERION_CTP_CATALOG_LIBRARY")}
                try:
                    env["ASTERION_MARKET_EXECUTABLE"] = str(market)
                    env["ASTERION_CTP_LIBRARY"] = str(library)
                    env["ASTERION_CTP_CATALOG_LIBRARY"] = str(app / "Contents/Resources/native/ctp-trader.dylib")
                    run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/ctp_sdk_smoke.py"), str(ROOT / "build/Release")])
                finally:
                    for name, value in saved.items():
                        if value is None:
                            env.pop(name, None)
                        else:
                            env[name] = value
            finally:
                if previous_node is None:
                    env.pop("ASTERION_NODE_AGENT_EXECUTABLE", None)
                else:
                    env["ASTERION_NODE_AGENT_EXECUTABLE"] = previous_node
                if previous is None:
                    env.pop("ASTERION_TRADING_EXECUTABLE", None)
                else:
                    env["ASTERION_TRADING_EXECUTABLE"] = previous
        finally:
            run(["hdiutil", "detach", folder])
    print("DMG checksum, signatures, packaged task recovery and vendor market/trader lifecycles verified")


def main():
    mode = sys.argv[1] if len(sys.argv) == 2 else "dev"
    if mode not in {"dev", "build", "package-test", "check", "verify", "verify-test"}:
        raise SystemExit("Usage: desktop.py dev|build|package-test|check|verify|verify-test")
    if mode in {"verify", "verify-test"}:
        verify_macos_bundle(mode == "verify")
        return
    if sys.platform != "darwin":
        raise SystemExit("Terminal desktop development and packaging require macOS")
    if mode == "build":
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
    stage = [sys.executable, str(ROOT / "scripts/remote_resources.py"), "stage", "--archives", str(archives)]
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
    install = ["conan", "install", ".", "-s", "build_type=" + configuration, "-s", "compiler.cppstd=20", "-c", "tools.cmake.cmaketoolchain:generator=Ninja", "--build=missing"]
    if profile.exists():
        install += ["-pr:h", str(profile), "-pr:b", str(profile)]
    # Desktop dependencies and flags come from Conan, not shell-wide LLVM
    # overrides that can link a distributable app to Homebrew libraries.
    for variable in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS"):
        env.pop(variable, None)
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
    run(install)
    configure = ["cmake", "--preset", preset]
    # CMake's compiler detection survives toolchain changes. Reset only
    # when an earlier configure used a different compiler.
    compiler_records = (ROOT / "build" / configuration / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake")
    if any('set(CMAKE_CXX_COMPILER "/usr/bin/clang++")' not in file.read_text()
           for file in compiler_records):
        configure.append("--fresh")
    # Reinitialize managed flags without deleting compiled object files.
    # Conan's cache default does not replace a previous deployment target.
    configure += ["-UCMAKE_*_FLAGS*", "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + minimum]
    electron_version = json.loads((ROOT / "node_modules/electron/package.json").read_text())["version"]
    sdk = ROOT / "build/electron-sdk"
    run(["pnpm", "exec", "node-gyp", "install", "--ensure", "--target=" + electron_version,
         "--dist-url=https://electronjs.org/headers", "--devdir=" + str(sdk)])
    configure += ["-DASTERION_NODE_HEADERS=" + str(sdk / electron_version / "include/node")]
    run(configure)
    run(["cmake", "--build", "--preset", preset])
    env["ASTERION_CPP_BUILD"] = str(ROOT / "build" / configuration)
    native = ROOT / "build/electron-resources/native"
    stage_native_resources(ROOT / "build" / configuration, native)
    run(["pnpm", "build"])
    # Loading and calling the compiled addon is part of desktop:check.
    run([sys.executable, str(ROOT / "tests/isolated_node.py"), "node",
         str(ROOT / "tests/electron_bridge.cjs"), str(native / "asterion_terminal.node")])
    if mode == "check":
        run(["node", "--check", "apps/clients/terminal/electron/main.cjs"])
        run(["node", "--check", "apps/clients/terminal/electron/preload.cjs"])
        return
    if mode == "dev":
        run_development()
        return
    builder = ["pnpm", "exec", "electron-builder", "--mac", "--config", "apps/clients/terminal/electron/builder.cjs"]
    env["ASTERION_PACKAGE_MODE"] = "release" if mode == "build" else "test"
    output = ROOT / "build" / ("desktop" if mode == "build" else "desktop-test")
    if output.exists():
        if output.is_symlink() or not output.is_dir():
            raise ValueError("Desktop output must be a generated directory")
        shutil.rmtree(output)
    run(builder)
    installers = list(output.glob("*.dmg"))
    if len(installers) != 1:
        raise ValueError("Expected exactly one current installer")
    if mode == "build":
        macos_release.notarize_installer(installers[0], env)
    verify_macos_bundle(mode == "build")
    if mode == "build":
        evidence = output / "installed"
        run([sys.executable, str(ROOT / "tests/electron_installer.py"), str(installers[0]),
             "--output", str(evidence)])
        macos_release.write_evidence(installers[0], evidence / "acceptance.json")
    print(f"Desktop installer: {installers[0]}")


if __name__ == "__main__":
    main()
