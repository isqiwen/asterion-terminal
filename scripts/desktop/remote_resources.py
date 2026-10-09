"""Validate and stage Linux x86_64 services inside a desktop installer."""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import sys
import zipfile
import tempfile
from generated_resources import publish_tree

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/services"))
from service_fingerprint import fingerprint
ARCHES = ('x86_64',)
def files_for(arch):
    if arch not in ARCHES:
        raise ValueError('Linux currently supports x86_64 only')
    return ('asterion-node-agent', 'asterion-trading', 'asterion-market-data', 'asterion-data-service', 'asterion-task-service', 'asterion-backtest', 'asterion-factor', 'asterion-data-pipeline', 'initialize-linux.py', 'plugins/asterion-tushare.so', 'plugins/asterion-order-limits.so') + (('ctp-md.so', 'ctp-trader.so') if arch == 'x86_64' else ())


def validate(manifest, files, arch):
    version = json.loads((ROOT/'apps/clients/terminal/electron/package.json').read_text())['version']
    if set(manifest) != {'version','product_version','source_sha256','os','arch','files'} or manifest['version'] != 2 or manifest['product_version'] != version or manifest['os'] != 'linux' or manifest['arch'] != arch or set(manifest['files']) != set(files_for(arch)):
        raise ValueError('Linux service manifest/version/architecture mismatch')
    if manifest['source_sha256'] != fingerprint():
        raise ValueError('Linux service source differs from this desktop source; rebuild the remote bundle')
    for name in files_for(arch):
        data = files[name]
        if hashlib.sha256(data).hexdigest() != manifest['files'][name]:
            raise ValueError(f'Linux service checksum mismatch: {arch}/{name}')
        if name != 'initialize-linux.py':
            if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or int.from_bytes(data[18:20],'little') != 62:
                raise ValueError(f'Linux ELF architecture mismatch: {arch}/{name}')
    if files['initialize-linux.py'] != (ROOT/'scripts/node/initialize-linux.py').read_bytes():
        raise ValueError('Initializer differs from this desktop source version')


def read_archive(path, arch):
    with zipfile.ZipFile(path) as archive:
        expected = {*files_for(arch), 'manifest.json'}
        if len(archive.infolist()) != len(expected) or set(archive.namelist()) != expected:
            raise ValueError('Unexpected archive members; no paths or extra entries allowed')
        for entry in archive.infolist():
            if entry.file_size > (65536 if entry.filename=='manifest.json' else 256*1024*1024) or stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError('Oversized or symlink archive entry')
        manifest=json.loads(archive.read('manifest.json'))
        files={name:archive.read(name) for name in files_for(arch)}
        validate(manifest,files,arch)
        return {**files,'manifest.json':json.dumps(manifest,indent=2).encode()+b'\n'}


def verify(directory):
    if directory.is_symlink():
        raise ValueError('Bundled resource directory cannot be a symlink')
    expected = {f'{arch}/{name}' for arch in ARCHES for name in (*files_for(arch), 'manifest.json')}
    actual = {path.relative_to(directory).as_posix() for path in directory.rglob('*') if path.is_file()}
    directories = {arch for arch in ARCHES} | {f'{arch}/plugins' for arch in ARCHES}
    if actual != expected or any(path.is_symlink() or (path.is_dir() and path.relative_to(directory).as_posix() not in directories) for path in directory.rglob('*')):
        raise ValueError('Unexpected or missing bundled Linux resources')
    for arch in ARCHES:
        folder=directory/arch
        paths=[folder/name for name in (*files_for(arch),'manifest.json')]
        if folder.is_symlink() or (folder/'plugins').is_symlink() or any(path.is_symlink() or not path.is_file() for path in paths):
            raise ValueError(f'Missing or unsafe bundled Linux resources: {arch}')
        validate(json.loads((folder/'manifest.json').read_text()),{name:(folder/name).read_bytes() for name in files_for(arch)},arch)


def stage(archives, destination):
    payload={arch:read_archive(archives/f'asterion-services-linux-{arch}.zip',arch) for arch in ARCHES}
    # Validate every archive first, then publish an exact fresh generated tree.
    if destination.is_symlink():
        raise ValueError('Generated resource directory cannot be a symlink')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.remote-stage-', dir=destination.parent) as temporary:
        prepared = Path(temporary) / 'resources'
        for arch, files in payload.items():
            for name, data in files.items():
                target = prepared / arch / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                target.chmod(0o644)
        verify(prepared)
        publish_tree(prepared, destination)



if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['stage','verify'])
    parser.add_argument('--archives',type=Path,default=ROOT/'build/linux-bundles')
    parser.add_argument('--directory',type=Path,default=ROOT/'build/electron-resources/remote-linux')
    args=parser.parse_args()
    if args.action=='stage': stage(args.archives,args.directory)
    else: verify(args.directory)
    print('Verified bundled Linux x86_64 services and initializer')
