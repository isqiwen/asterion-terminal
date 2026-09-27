"""Explicit test-only bundle: synthetic ELF headers unless native Linux binaries supplied."""
import hashlib
import json
from pathlib import Path
import platform
import shutil


def make_bundle(root, native=None):
    source=Path(__file__).resolve().parents[1]
    version=json.loads((source/'apps/terminal/src-tauri/tauri.conf.json').read_text())['version']
    native_arch={'aarch64':'arm64','arm64':'arm64','x86_64':'x86_64','AMD64':'x86_64'}.get(platform.machine())
    for arch, machine in [('x86_64',62)]:
        folder=root/arch; folder.mkdir(parents=True,exist_ok=True)
        header=bytearray(64); header[:6]=b'\x7fELF\x02\x01'; header[18:20]=machine.to_bytes(2,'little')
        for name in ['asterion-node-agent','asterion-trading','asterion-market-data', 'asterion-task-service', 'asterion-backtest', 'asterion-factor', 'asterion-data-pipeline', 'asterion-strategy']+(['ctp-md.so'] if arch=='x86_64' else []):
            if native and platform.system()=='Linux' and arch==native_arch:
                shutil.copyfile(Path(native)/name,folder/name)
            else: (folder/name).write_bytes(header)
        shutil.copyfile(source/'scripts/node/initialize-linux.py',folder/'initialize-linux.py')
        manifest=dict(version=1,product_version=version,os='linux',arch=arch,files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir() if p.name!='manifest.json'})
        (folder/'manifest.json').write_text(json.dumps(manifest))
    return root
