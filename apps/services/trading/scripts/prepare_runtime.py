"""Build the relocatable, offline Trading runtime on Linux x86_64.

Only the release build downloads dependencies. This tree contains no Lab code,
research dependencies, pip or package-installed commands.
"""
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
PYTHON = '3.12.12'


def prepare(destination):
    if (sys.platform, platform.machine()) != ('linux', 'x86_64'):
        raise SystemExit('Trading runtime releases must be built on Linux x86_64')

    def run(*args, **options):
        return subprocess.run(list(map(str, args)), check=True, **options)

    run('uv', 'python', 'install', PYTHON)
    found = run('uv', 'python', 'find', '--managed-python', '--no-project', PYTHON,
                capture_output=True, text=True).stdout.strip()
    standalone = Path(found).resolve().parents[1]
    requirements = run('uv', 'export', '--project', PROJECT, '--locked', '--no-dev',
        '--no-emit-project', '--quiet', capture_output=True, text=True).stdout
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-', dir=destination.parent) as folder:
        folder = Path(folder)
        prepared = folder / 'python'
        shutil.copytree(standalone, prepared)
        site = prepared / 'lib/python3.12/site-packages'
        shutil.rmtree(site)
        site.mkdir()
        locked = folder / 'requirements.txt'
        locked.write_text(requirements)
        (prepared / 'lib/python3.12/EXTERNALLY-MANAGED').unlink(missing_ok=True)
        run('uv', 'pip', 'install', '--python', prepared / 'bin/python3',
            '--require-hashes', '--only-binary=:all:', '--no-python-downloads',
            '--python-platform', 'x86_64-manylinux_2_28', '-r', locked)
        for pattern in ('include', 'share', 'lib/pkgconfig', 'lib/libpython3.so',
                        'lib/libpython3.12.so', 'lib/libtcl*', 'lib/libtk*', 'lib/tcl*',
                        'lib/tk*', 'lib/itcl*', 'lib/thread*', 'lib/python3.12/config-*',
                        'lib/python3.12/test', 'lib/python3.12/idlelib',
                        'lib/python3.12/tkinter', 'lib/python3.12/turtledemo',
                        'lib/python3.12/ensurepip', 'lib/python3.12/lib2to3',
                        'lib/python3.12/lib-dynload/_tkinter*', '**/__pycache__'):
            for path in prepared.glob(pattern):
                shutil.rmtree(path) if path.is_dir() else path.unlink()
        for path in (prepared / 'bin').iterdir():
            if path.name not in ('python3', 'python3.12'):
                path.unlink()
        run(prepared / 'bin/python3', '-I', '-B', '-c',
            'import sys, numpy, google.protobuf; '
            'assert sys.version_info[:3] == (3, 12, 12); '
            'assert numpy.__version__ == "2.3.5"; '
            'assert google.protobuf.__version__ == "5.29.3"')
        if destination.exists():
            shutil.rmtree(destination)
        prepared.rename(destination)
    print(f'Prepared offline Trading Python runtime: {destination}')


if __name__ == '__main__':
    prepare(Path(sys.argv[1]).resolve())
