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


def verify_macos_bundle():
    if sys.platform != "darwin":
        raise SystemExit("DMG verification requires macOS")
    bundles = list((ROOT / "build/desktop").glob("*.dmg"))
    if len(bundles) != 1:
        raise SystemExit("Expected exactly one current DMG; inspect build outputs")
    run(["hdiutil", "verify", str(bundles[0])])
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as folder:
        run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", folder, str(bundles[0])])
        try:
            apps = list(Path(folder).glob("*.app"))
            if len(apps) != 1:
                raise SystemExit("Expected exactly one application in DMG")
            app = apps[0]
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
            run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/remote_trading.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge"), str(trading), str(ROOT / "build/Release/asterion_test_certificates")])
            node = app / "Contents/Resources/native/asterion-node-agent"
            run(["codesign", "--verify", "--strict", str(node)])
            run([str(node), "--version"])
            market = app / "Contents/Resources/native/asterion-market-data"
            run(["codesign", "--verify", "--strict", str(market)])
            run([str(market), "--version"])
            for name in ("asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline", "asterion-strategy"):
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
                run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/paper_recovery.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")])
                research_variables = {"ASTERION_TASK_EXECUTABLE": "asterion-task-service", "ASTERION_BACKTEST_EXECUTABLE": "asterion-backtest", "ASTERION_FACTOR_EXECUTABLE": "asterion-factor", "ASTERION_DATA_PIPELINE_EXECUTABLE": "asterion-data-pipeline", "ASTERION_STRATEGY_EXECUTABLE": "asterion-strategy"}
                research_saved = {name: env.get(name) for name in research_variables}
                try:
                    for name, program in research_variables.items():
                        env[name] = str(app / "Contents/Resources/native" / program)
                    run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/research_agent.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")])
                    run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/strategy_terminal.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")])
                    run(["ctest", "--test-dir", str(ROOT / "build/Release"), "-R", "^strategy_(replay|calendar)_process$", "--output-on-failure", "--no-tests=error"])
                finally:
                    for name, value in research_saved.items():
                        if value is None:
                            env.pop(name, None)
                        else:
                            env[name] = value
                saved = {name: env.get(name) for name in ("ASTERION_MARKET_EXECUTABLE", "ASTERION_CTP_LIBRARY")}
                try:
                    env["ASTERION_MARKET_EXECUTABLE"] = str(market)
                    env["ASTERION_CTP_LIBRARY"] = str(library)
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
    print("DMG checksum, signatures, packaged strategy/research/trading recovery and vendor market lifecycle verified")


def main():
    mode = sys.argv[1] if len(sys.argv) == 2 else "dev"
    if mode not in {"dev", "build", "check", "verify"}:
        raise SystemExit("Usage: desktop.py dev|build|check|verify")
    if mode == "verify":
        verify_macos_bundle()
        return
    if sys.platform != "darwin":
        raise SystemExit("Terminal desktop development and packaging require macOS")
    resources = ROOT / "build/electron-resources/remote-linux"
    archives = Path(env.get("ASTERION_LINUX_BUNDLES", str(ROOT / "build/linux-bundles")))
    if mode == "build" and not (archives / "asterion-services-linux-x86_64.zip").is_file():
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
    configuration = "Release" if mode == "build" else "Debug"
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
    native.mkdir(parents=True, exist_ok=True)
    names = ("asterion-trading", "asterion-node-agent", "asterion-market-data", "asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline", "asterion-strategy", "asterion-keychain")
    programs = [ROOT / "build" / configuration / name for name in names]
    programs.append(ROOT / "build" / configuration / "asterion_terminal.node")
    for source in programs:
        stage_native(source, native / source.name)
    plugin_root = ROOT / "build" / configuration / "plugins"
    (native / "plugins").mkdir(parents=True, exist_ok=True)
    for plugin in plugin_root.glob("*.dylib"):
        stage_native(plugin, native / "plugins" / plugin.name)
    library_name = "ctp-md.dylib"
    library = ROOT / "build" / configuration / library_name
    stage_native(library, native / library_name)
    catalog_name = library_name.replace("ctp-md", "ctp-trader")
    stage_native(ROOT / "build" / configuration / catalog_name, native / catalog_name)
    run(["pnpm", "build"])
    # Loading and calling the compiled addon is part of desktop:check.
    run(["node", str(ROOT / "tests/electron_bridge.cjs"), str(native / "asterion_terminal.node")])
    if mode == "check":
        run(["node", "--check", "apps/clients/terminal/electron/main.cjs"])
        run(["node", "--check", "apps/clients/terminal/electron/preload.cjs"])
        return
    if mode == "dev":
        run_development()
        return
    builder = ["pnpm", "exec", "electron-builder", "--mac", "--config", "apps/clients/terminal/electron/builder.cjs"]
    run(builder)
    verify_macos_bundle()
    installers = list((ROOT / "build/desktop").glob("*.dmg"))
    print(f"Desktop installer: {installers[0]}")


if __name__ == "__main__":
    main()
