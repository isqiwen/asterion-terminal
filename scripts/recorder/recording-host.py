#!/usr/bin/python3 -I
"""Install and upgrade asterion_recorder on a recording host.

Run locally on the host as root (sudo). The recorder is installed in
/opt/asterion-recorder, laid out like the bundle:

  bin/     asterion_recorder, recording-host.py, configure.sh
  lib/     ctp-md.so, ctp-trader.so
  config/  recorder.json (copied once from the bundle, never overwritten)
  logs/
  data/    recordings

It runs as the system service asterion-recorder.service under its own
account asterion-recorder, which cannot log in; nothing else on the host
manages it. Clients read it over plain TCP (port 7450 by default); open the
port in the firewall only to your own machines.

  install BUNDLE                program, account and unit
  upgrade BUNDLE                a new program, outside trading hours only
  trading-hours                 fails during trading hours if the recorder runs

The settings, the account and its password are entered with bin/configure.sh.

Anything that restarts a running recorder is refused during trading hours
(Beijing time, weekdays 08:45-15:30 and 20:45-02:45, holidays included).
"""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import subprocess
import sys
import tempfile
import zipfile

UNIT = 'asterion-recorder.service'
# The installation: bin/ lib/ config/ logs/ data/. The bundle has the same layout.
ROOT = Path('/opt/asterion-recorder')
ACCOUNT = 'asterion-recorder'
CREDENTIALS = Path('/etc/credstore.encrypted')
UNITS = Path('/etc/systemd/system')
SETTINGS = ROOT / 'config/recorder.json'
PROGRAMS = ('bin/asterion_recorder', 'bin/recording-host.py', 'bin/configure.sh', 'lib/ctp-md.so', 'lib/ctp-trader.so')
FILES = (*PROGRAMS, 'config/recorder.json')
ENV = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'}
BEIJING = timezone(timedelta(hours=8))


def run(*args, **kwargs):
    return subprocess.run(args, check=True, timeout=120, env=ENV, **kwargs)


def trading(now):
    """Whether `now` falls in or near a session. Holidays are not known here,
    so every weekday counts; a night session runs past midnight into the next
    calendar day."""
    local = now.astimezone(BEIJING)
    minute = local.hour * 60 + local.minute
    weekday = local.weekday()  # Monday is 0
    day = weekday < 5 and (8 * 60 + 45 <= minute < 15 * 60 + 30 or minute >= 20 * 60 + 45)
    night = 1 <= weekday <= 5 and minute < 2 * 60 + 45
    return day or night


def outside_trading(action, now=None):
    now = now or datetime.now(timezone.utc)
    if trading(now) and active():
        raise ValueError(f'{action} would restart the recorder during trading hours (Beijing '
                         'weekdays 08:45-15:30 and 20:45-02:45); run it outside them')


def active():
    return subprocess.run(['/usr/bin/systemctl', 'is-active', '--quiet', UNIT], env=ENV).returncode == 0


def unit_text():
    return ('[Unit]\nDescription=Asterion market recorder\n'
            'Wants=network-online.target\nAfter=network-online.target\n'
            # Never give up restarting: a recorder that stays down loses the market.
            'StartLimitIntervalSec=0\n'
            f'[Service]\nUser={ACCOUNT}\nGroup={ACCOUNT}\nUMask=0077\n'
            # It finds config/, lib/, logs/ and data/ beside its bin/.
            f'ExecStart={ROOT}/bin/asterion_recorder\n'
            f'ReadWritePaths={ROOT}/logs {ROOT}/data\n'
            # The counter password and authentication code, decrypted at start.
            'ImportCredential=asterion.ctp.*\n'
            'Restart=always\nRestartSec=5\nTimeoutStopSec=60\n'
            'NoNewPrivileges=yes\nProtectSystem=strict\nProtectHome=yes\nPrivateTmp=yes\n'
            'CapabilityBoundingSet=\nRestrictSUIDSGID=yes\n'
            '[Install]\nWantedBy=multi-user.target\n')


def read_bundle(path):
    """Returns the bundle's files after checking every one against its manifest."""
    with zipfile.ZipFile(path) as archive:
        names = set(archive.namelist())
        if names != {*FILES, 'manifest.json'}:
            raise ValueError(f'Unexpected bundle members: {sorted(names)}')
        manifest = json.loads(archive.read('manifest.json'))
        if set(manifest) != {'product_version', 'files'} or set(manifest['files']) != set(FILES):
            raise ValueError('Invalid bundle manifest')
        files = {}
        for name in FILES:
            data = archive.read(name)
            if hashlib.sha256(data).hexdigest() != manifest['files'][name]:
                raise ValueError(f'Bundle checksum mismatch: {name}')
            if name.endswith(('_recorder', '.so')) and data[:4] != b'\x7fELF':
                raise ValueError(f'Not a Linux program: {name}')
            files[name] = data
        return manifest, files


def place(bundle):
    """Checks the bundle, makes sure its recorder runs here, then replaces the
    programs in bin/ and lib/ one rename each. A running recorder keeps the
    files it started with until it restarts. Returns the bundle's files."""
    manifest, files = read_bundle(bundle)
    with tempfile.TemporaryDirectory(dir=ROOT) as folder:
        staging = Path(folder)
        for name in PROGRAMS:
            (staging / Path(name).name).write_bytes(files[name])
            (staging / Path(name).name).chmod(0o755)
        version = run(str(staging / 'asterion_recorder'), '--version', capture_output=True, text=True).stdout
        if 'Asterion Recorder' not in version:
            raise ValueError('The bundled recorder does not run on this host')
        for name in PROGRAMS:
            os.replace(staging / Path(name).name, ROOT / name)
    return manifest, files


def account():
    try:
        return pwd.getpwnam(ACCOUNT)
    except KeyError:
        run('/usr/sbin/useradd', '--system', '--no-create-home', '--home-dir', str(ROOT / 'data'),
            '--shell', '/usr/sbin/nologin', '--user-group', ACCOUNT)
        return pwd.getpwnam(ACCOUNT)


def write(path, text, mode, group=0):
    temporary = path.with_name('.' + path.name + '.new')
    temporary.write_text(text)
    os.chown(temporary, 0, group)
    temporary.chmod(mode)
    temporary.rename(path)


def install(args):
    if not Path('/run/systemd/system').is_dir():
        raise ValueError('A booted systemd host is required')
    for tool in ('/usr/bin/systemd-creds', '/usr/sbin/useradd'):
        if not os.access(tool, os.X_OK):
            raise ValueError(f'Install the prerequisite first: {tool}')
    if (ROOT / 'bin/asterion_recorder').exists():
        raise ValueError('The recorder is installed already; use upgrade')
    user = account()
    for folder, owner, mode in (('bin', 0, 0o755), ('lib', 0, 0o755), ('config', 0, 0o750),
                                ('logs', user.pw_uid, 0o700), ('data', user.pw_uid, 0o700)):
        (ROOT / folder).mkdir(parents=True, exist_ok=True)
        os.chown(ROOT / folder, owner, user.pw_gid)
        (ROOT / folder).chmod(mode)
    manifest, files = place(args.bundle)
    # Copied once: later installs and upgrades never overwrite your edits.
    if not SETTINGS.exists():
        write(SETTINGS, files['config/recorder.json'].decode(), 0o640, user.pw_gid)
    # Where configure.sh keeps the encrypted password and authentication code.
    CREDENTIALS.mkdir(mode=0o700, exist_ok=True)
    write(UNITS / UNIT, unit_text(), 0o644)
    run('/usr/bin/systemctl', 'daemon-reload')
    run('/usr/bin/systemctl', 'enable', UNIT)
    print(f"Installed {manifest['product_version']} in {ROOT}. Next: sudo {ROOT}/bin/configure.sh")
    port = json.loads(SETTINGS.read_text())['port']
    print(f'Settings: {SETTINGS}. It runs as {ACCOUNT} and listens on TCP {port}, unencrypted: allow the '
          f'port only from your own machines, e.g. ufw allow from <address> to any port {port} proto tcp')


def trading_hours(args):
    """For configure.sh: refuses, before any question, what would restart a
    running recorder during trading hours."""
    outside_trading('Configuring')


def upgrade(args):
    outside_trading('An upgrade')
    manifest, _ = place(args.bundle)
    if active():
        run('/usr/bin/systemctl', 'restart', UNIT)
    print(f"Running {manifest['product_version']}.")


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('install')
    command.add_argument('bundle')
    command.set_defaults(action=install)
    commands.add_parser('trading-hours').set_defaults(action=trading_hours)
    command = commands.add_parser('upgrade')
    command.add_argument('bundle')
    command.set_defaults(action=upgrade)
    args = parser.parse_args(arguments)
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise ValueError('Run on the recording host with sudo')
    os.umask(0o022)
    args.action(args)


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except Exception as error:
        print(f'recording-host: {error}', file=sys.stderr)
        sys.exit(1)
