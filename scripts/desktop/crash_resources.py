"""Pinned crash parser and native Breakpad symbols shipped with the desktop."""
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

from generated_resources import publish_tree

ROOT = Path(__file__).resolve().parents[2]
DESTINATION = ROOT / "build/electron-resources/crash"
TOOLS = {
    "minidump-stackwalk": ("rust-minidump/rust-minidump", "0.27.0", {
        "linux": ("x86_64-unknown-linux-musl", "bfaf97804965c87155b91765f4042440a38bb002f20a62fc4da9b6b6bcb20faf"),
        "darwin": ("aarch64-apple-darwin", "5fcd4d25a5b0bbdc8faac6f4f81629bc67483c3eb458b86a8304733a7c6a2ca9"),
    }),
    "dump_syms": ("mozilla/dump_syms", "2.3.9", {
        "linux": ("x86_64-unknown-linux-gnu", "0fc852a86b00337407d9d423cc388a24c3b489ccaaedcf92623cad57af5ca8ad"),
        "darwin": ("aarch64-apple-darwin", "4945b7e5de0d7ad403822635f0fb912b69990fc412741ff2b7b512e168c40a32"),
    }),
}


def tool(name):
    repository, version, targets = TOOLS[name]
    target, checksum = targets[sys.platform]
    cache = ROOT / "build/crash-tools" / name / version / target
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "download.tar.xz"
    if not archive.is_file():
        url = f"https://github.com/{repository}/releases/download/v{version}/{name}-{target}.tar.xz"
        with urllib.request.urlopen(url) as response, (cache / "download.tmp").open("wb") as output:
            shutil.copyfileobj(response, output)
        (cache / "download.tmp").replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != checksum:
        raise ValueError(f"Crash tool checksum mismatch: {archive}")
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            filename = Path(member.name).name
            if member.isfile() and (filename == name or filename.startswith(("LICENSE", "COPYING"))):
                with bundle.extractfile(member) as source, (cache / filename).open("wb") as output:
                    shutil.copyfileobj(source, output)
    (cache / name).chmod(0o755)
    return cache / name


def prepare(build):
    if (sys.platform, platform.machine()) not in (("linux", "x86_64"), ("darwin", "arm64")):
        raise ValueError("Crash resources support Linux x86_64 and macOS arm64")
    binary = build / "asterion_terminal.node"
    identity = {"binary": hashlib.sha256(binary.read_bytes()).hexdigest(), "tools": TOOLS,
                "product": json.loads((build / "product.json").read_text())}
    # JSON normalizes the tool tuples to arrays before comparison.
    identity = json.loads(json.dumps(identity))
    manifest = DESTINATION / "manifest.json"
    if manifest.is_file() and json.loads(manifest.read_text()) == identity \
            and (DESTINATION / "minidump-stackwalk").is_file() \
            and any((DESTINATION / "symbols").glob("*/*/*.sym")):
        return
    walker, dumper = tool("minidump-stackwalk"), tool("dump_syms")
    DESTINATION.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=DESTINATION.parent, prefix=".crash-") as temporary:
        staged = Path(temporary) / "crash"
        staged.mkdir()
        shutil.copy2(walker, staged / walker.name)
        for license in walker.parent.glob("LICENSE*"):
            shutil.copy2(license, staged / license.name)
        symbols = Path(temporary) / "native.sym"
        source = (ROOT / "build/electron-resources/symbols/asterion_terminal.node.dSYM") if sys.platform == "darwin" else binary
        with symbols.open("wb") as output:
            subprocess.run([str(dumper), str(source)], stdout=output, check=True)
        with symbols.open() as stream:
            record, _, _, identifier, name = stream.readline().strip().split(" ", 4)
        if record != "MODULE" or Path(name).name != name:
            raise ValueError("Invalid native symbol module")
        destination = staged / "symbols" / name / identifier
        destination.mkdir(parents=True)
        shutil.move(symbols, destination / (name + ".sym"))
        (staged / "manifest.json").write_text(json.dumps(identity, indent=2) + "\n")
        publish_tree(staged, DESTINATION)


if __name__ == "__main__":
    prepare(Path(sys.argv[1]).resolve())
