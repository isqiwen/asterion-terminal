"""Syntax-check the _WIN32 branches of C++ sources from a macOS/Linux checkout.

Uses mingw-w64 headers (for example `brew install mingw-w64`) and the host
build's compile_commands.json (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).
This approximates MSVC: it catches missing declarations, include-order and
type errors in Windows-only code before CI, but it is not a Windows build.

    python3 scripts/check_windows_syntax.py [build directory]
"""
import json, shlex, subprocess, sys, pathlib, concurrent.futures
root = pathlib.Path(__file__).resolve().parents[1]
build = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/Debug"
commands = json.load(open(build / "compile_commands.json"))
def windows_code(path):
    try: return "_WIN32" in open(path, encoding="utf-8").read()
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
        keep.append(a)
    cmd = ["x86_64-w64-mingw32-g++", "-fsyntax-only", "-std=c++20", "-D_WIN32_WINNT=0x0A00", "-DNOMINMAX", "-DSPDLOG_WCHAR_FILENAMES", *keep, src]
    r = subprocess.run(cmd, cwd=entry["directory"], capture_output=True, text=True)
    errors = [l for l in r.stderr.splitlines() if "error:" in l]
    return (src, errors[:6]) if errors else (src, [])
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
