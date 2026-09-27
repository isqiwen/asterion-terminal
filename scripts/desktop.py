"""Build the C++ runtime first; Cargo is only the Tauri shell."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
env = os.environ.copy()
# Direct Python entry must resolve the root package tools from app subdirectories.
env["PATH"] = str(ROOT / "node_modules/.bin") + os.pathsep + env["PATH"]
local_cargo = ROOT / ".state/toolchain/cargo"
cargo_name = "cargo.exe" if sys.platform == "win32" else "cargo"
if (local_cargo / "bin" / cargo_name).exists():
    env.update(CARGO_HOME=str(local_cargo), RUSTUP_HOME=str(ROOT / ".state/toolchain/rustup"))
    env["PATH"] = str(local_cargo / "bin") + os.pathsep + env["PATH"]


def run(args, cwd=ROOT):
    executable = shutil.which(args[0], path=env["PATH"])
    if executable is None:
        raise SystemExit(f"Required build tool not found: {args[0]}")
    subprocess.run([executable, *args[1:]], cwd=cwd, env=env, check=True)


def verify_macos_bundle():
    if sys.platform != "darwin":
        raise SystemExit("DMG verification requires macOS")
    bundles = list((ROOT / "apps/terminal/src-tauri/target/release/bundle/dmg").glob("*.dmg"))
    if len(bundles) != 1:
        raise SystemExit("Expected exactly one current DMG; inspect build outputs")
    run(["hdiutil", "verify", str(bundles[0])])
    with tempfile.TemporaryDirectory(prefix="asterion-verify-") as folder:
        run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", folder, str(bundles[0])])
        try:
            app = Path(folder) / "Asterion Terminal.app"
            run(["codesign", "--verify", "--deep", "--strict", str(app)])
            run([sys.executable, str(ROOT / "scripts/remote_resources.py"), "verify", "--directory", str(app / "Contents/Resources/remote-linux")])
            trading = app / "Contents/MacOS/asterion-trading"
            if not trading.is_file():
                raise SystemExit("DMG does not contain the trading sidecar")
            run(["codesign", "--verify", "--strict", str(trading)])
            run([str(trading), "--version"])
            run([sys.executable, str(ROOT / "tests/remote_trading.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge"), str(trading), str(ROOT / "build/Release/asterion_test_certificates")])
            node = app / "Contents/MacOS/asterion-node-agent"
            run(["codesign", "--verify", "--strict", str(node)])
            run([str(node), "--version"])
            market = app / "Contents/MacOS/asterion-market-data"
            run(["codesign", "--verify", "--strict", str(market)])
            run([str(market), "--version"])
            for name in ("asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline", "asterion-strategy"):
                program = app / "Contents/MacOS" / name
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
                        env[name] = str(app / "Contents/MacOS" / program)
                    run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/research_agent.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")])
                    run([sys.executable, str(ROOT / "tests/isolated_node.py"), sys.executable, str(ROOT / "tests/strategy_terminal.py"), str(ROOT / "build/Release/asterion_terminal_dev_bridge")])
                    run(["ctest", "--test-dir", str(ROOT / "build/Release"), "-R", "^strategy_calendar_process$", "--output-on-failure", "--no-tests=error"])
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
    if sys.platform not in {"darwin", "win32", "linux"}:
        raise SystemExit("Supported desktop platforms: macOS, Windows, Linux")
    resources = ROOT / "apps/terminal/src-tauri/resources/remote-linux"
    archives = Path(env.get("ASTERION_LINUX_BUNDLES", str(ROOT / "build/linux-bundles")))
    if mode == "build" or archives.is_dir():
        run([sys.executable, str(ROOT / "scripts/remote_resources.py"), "stage", "--archives", str(archives)])
    else:
        resources.mkdir(parents=True, exist_ok=True)
    env["ASTERION_REMOTE_RESOURCES"] = str(resources)
    profile = ROOT / "build/local-profile"
    configuration = "Release" if mode == "build" else "Debug"
    preset = "conan-" + configuration.lower()
    install = ["conan", "install", ".", "-s", "build_type=" + configuration, "-s", "compiler.cppstd=20", "-c", "tools.cmake.cmaketoolchain:generator=Ninja", "--build=missing"]
    if profile.exists():
        install += ["-pr:h", str(profile), "-pr:b", str(profile)]
    if sys.platform == "win32":
        # Rust MSVC uses the release dynamic CRT, including debug builds.
        install += ["-s:h", "compiler.runtime=dynamic", "-s:h", "compiler.runtime_type=Release"]
    run(install)
    run(["cmake", "--preset", preset])
    run(["cmake", "--build", "--preset", preset])
    env["ASTERION_CPP_BUILD"] = str(ROOT / "build" / configuration)
    rustc = shutil.which("rustc", path=env["PATH"])
    if rustc is None:
        raise SystemExit("rustc is required to identify the native sidecar target")
    version = subprocess.check_output([rustc, "-vV"], env=env, text=True)
    triple = next(line.removeprefix("host: ") for line in version.splitlines() if line.startswith("host: "))
    suffix = ".exe" if sys.platform == "win32" else ""
    binaries = ROOT / "apps/terminal/src-tauri/binaries"
    binaries.mkdir(exist_ok=True)
    for name in ("asterion-trading", "asterion-node-agent", "asterion-market-data", "asterion-task-service", "asterion-backtest", "asterion-factor", "asterion-data-pipeline", "asterion-strategy"):
        source = ROOT / "build" / configuration / (name + suffix)
        shutil.copy2(source, binaries / (name + "-" + triple + suffix))
    native = ROOT / "apps/terminal/src-tauri/resources/native"
    native.mkdir(parents=True, exist_ok=True)
    library_name = "ctp-md" + {"darwin":".dylib", "win32":".dll", "linux":".so"}[sys.platform]
    library = ROOT / "build" / configuration / library_name
    if library.exists():
        shutil.copy2(library, native / library_name)
        env["ASTERION_CTP_LIBRARY"] = str(library)
    env["ASTERION_MARKET_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-market-data" + suffix))
    env["ASTERION_STRATEGY_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-strategy" + suffix))
    env["ASTERION_DATA_PIPELINE_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-data-pipeline" + suffix))
    env["ASTERION_FACTOR_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-factor" + suffix))
    env["ASTERION_TASK_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-task-service" + suffix))
    env["ASTERION_BACKTEST_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-backtest" + suffix))
    env["ASTERION_TRADING_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-trading" + suffix))
    env["ASTERION_NODE_AGENT_EXECUTABLE"] = str(ROOT / "build" / configuration / ("asterion-node-agent" + suffix))
    run(["pnpm", "build"])
    if mode == "check":
        run(["cargo", "build", "--locked", "--manifest-path", "apps/terminal/src-tauri/Cargo.toml"])
    else:
        command = ["pnpm", "exec", "tauri", mode]
        if mode == "build":
            target = "dmg" if sys.platform == "darwin" else "nsis" if sys.platform == "win32" else "deb"
            command += ["--bundles", target]
        run(command, ROOT / "apps/terminal")

    if mode == "build":
        folder, extension = {"darwin": ("dmg", "dmg"), "win32": ("nsis", "exe"), "linux": ("deb", "deb")}[sys.platform]
        output = ROOT / "apps/terminal/src-tauri/target/release/bundle" / folder
        installers = list(output.glob("*." + extension))
        if len(installers) != 1:
            raise SystemExit(f"Expected exactly one {extension} installer in {output}; inspect older build outputs")
        if sys.platform == "darwin":
            verify_macos_bundle()
        print(f"Desktop installer: {installers[0]}")


if __name__ == "__main__":
    main()
