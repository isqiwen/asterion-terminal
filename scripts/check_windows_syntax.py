"""Syntax-check the _WIN32 branches of C++ sources from a macOS/Linux checkout.

Uses mingw-w64 headers (for example `brew install mingw-w64`) and the host
build's compile_commands.json (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).
This approximates MSVC: it catches missing declarations, include-order and
type errors in Windows-only code before CI, but it is not a Windows build.

    python3 scripts/prepare_ctp.py --os windows --arch x86_64
    python3 scripts/check_windows_syntax.py [build directory]
"""
import argparse, hashlib, json, shlex, subprocess, sys, pathlib, concurrent.futures
root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", nargs="?", type=pathlib.Path, default=root / "build/Debug")
parser.add_argument("--ctp-sdk", type=pathlib.Path, default=root / "build/ctp-sdk/windows-x86_64")
options = parser.parse_args()
build = options.build.resolve()
ctp_sdk = options.ctp_sdk.resolve()
manifest = json.loads((root / "conan/ctp/sources.json").read_text())["windows"]
for name, entry in manifest.items():
    if not name.endswith(".h"):
        continue
    path = ctp_sdk / name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
        parser.error("Missing or mismatched Windows CTP headers; run python3 scripts/prepare_ctp.py --os windows --arch x86_64 explicitly")
if not (build / "compile_commands.json").is_file():
    parser.error("Missing compile_commands.json; configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
commands = json.load(open(build / "compile_commands.json"))
def windows_code(path):
    try:
        text = pathlib.Path(path).read_text(encoding="utf-8")
        return any(marker in text for marker in ("_WIN32", "_MSC_VER", "__APPLE__"))
    except OSError: return False
def check(entry):
    src = entry["file"]
    if "/build/" in src or not windows_code(src):
        return None
    args = shlex.split(entry["command"])[1:]
    keep = []; skip = False
    for i, a in enumerate(args):
        if skip: skip = False; continue
        if a in ("-o", "-arch", "-isysroot", "-MF", "-MT", "-MQ"): skip = True; continue
        if a.startswith(("-mmacosx", "-fsanitize", "-fcolor", "-MD", "-MMD", "-W", "-g", "-stdlib")) or a == "-c" or a == src: continue
        # The host Conan package has host-specific CTP signatures. Use the
        # separately provisioned, hash-verified Windows headers for this check.
        if i > 0 and args[i - 1] in ("-I", "-isystem") and (pathlib.Path(a) / "ThostFtdcTraderApi.h").is_file():
            a = str(ctp_sdk)
        keep.append(a)
    cmd = ["x86_64-w64-mingw32-g++", "-fsyntax-only", "-std=c++20", "-D_WIN32_WINNT=0x0A00", "-DNOMINMAX",  # mirrors the project-wide Windows definition in CMakeLists.txt
           "-DSPDLOG_WCHAR_FILENAMES", *keep, src]
    r = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
    errors = [l for l in r.stderr.splitlines() if "error:" in l]
    if r.returncode and not errors:
        errors = [f"compiler exited {r.returncode}: {r.stderr[-1000:]}"]
    return (src, errors[:6]) if r.returncode else (src, [])
with concurrent.futures.ThreadPoolExecutor(8) as pool:
    results = [r for r in pool.map(check, commands) if r]
seen=set()
for src, errors in results:
    if src in seen: continue
    seen.add(src)
    rel = pathlib.Path(src).relative_to(root)
    print(("FAIL " if errors else "ok   ") + str(rel))
    for e in errors: print("     " + e.replace(str(root) + "/", ""))
sys.exit(1 if any(errors for _, errors in results) else 0)
