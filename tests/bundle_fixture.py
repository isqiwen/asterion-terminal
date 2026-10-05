"""Explicit test-only bundle: synthetic ELF headers unless native Linux binaries supplied."""
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from service_fingerprint import fingerprint


def make_bundle(root, native=None, *, strip_debug=False):
    source=Path(__file__).resolve().parents[1]
    version=json.loads((source/'apps/clients/terminal/electron/package.json').read_text())['version']
    native_arch={'aarch64':'arm64','arm64':'arm64','x86_64':'x86_64','AMD64':'x86_64'}.get(platform.machine())
    for arch, machine in [('x86_64',62)]:
        folder=root/arch; folder.mkdir(parents=True,exist_ok=True)
        header=bytearray(64); header[:6]=b'\x7fELF\x02\x01'; header[18:20]=machine.to_bytes(2,'little')
        for name in ['asterion-node-agent','asterion-trading','asterion-market-data', 'asterion-data-service', 'asterion-task-service', 'asterion-backtest', 'asterion-factor', 'asterion-data-pipeline','plugins/asterion-tushare.so','plugins/asterion-order-limits.so']+(['ctp-md.so','ctp-trader.so'] if arch=='x86_64' else []):
            (folder/name).parent.mkdir(parents=True,exist_ok=True)
            if native and platform.system()=='Linux' and arch==native_arch:
                shutil.copyfile(Path(native)/name,folder/name)
                if strip_debug:
                    # Transfer executable code, not hundreds of MiB of DWARF.
                    # Only this disposable copy changes; sanitizer runtime and
                    # the original symbolized binaries remain intact.
                    subprocess.run(['strip','--strip-debug',str(folder/name)],check=True)
            else: (folder/name).write_bytes(header)
        shutil.copyfile(source/'scripts/node/initialize-linux.py',folder/'initialize-linux.py')
        manifest=dict(version=2,source_sha256=fingerprint(),product_version=version,os='linux',arch=arch,files={p.relative_to(folder).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file() and p.name!='manifest.json'})
        (folder/'manifest.json').write_text(json.dumps(manifest))
    return root
