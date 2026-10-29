#!/usr/bin/python3 -I
"""Install, configure and upgrade asterion_recorder on a recording host.

Run locally on the host as root (sudo). The recorder runs as the system
service asterion-recorder.service under its own system account
asterion-recorder, which cannot log in; nothing else on the host manages it. Its settings are in /etc/asterion-recorder/recorder.json,
copied once from the bundle. Clients read it over plain TCP (port 7450 by
default); open the port in the firewall only to your own machines.

  install BUNDLE                program, account and unit
  login                         counter addresses and account; the password and
                                authentication code are typed here and encrypted
                                with systemd-creds, bound to this host
  upgrade BUNDLE                a new program, outside trading hours only

Anything that restarts a running recorder is refused during trading hours
(Beijing time, weekdays 08:45-15:30 and 20:45-02:45, holidays included).
"""
import argparse
from datetime import datetime, timedelta, timezone
import getpass
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

UNIT = 'asterion-recorder.service'
PROGRAMS = Path('/opt/asterion-recorder')
CONFIG = Path('/etc/asterion-recorder')
STATE = Path('/var/lib/asterion-recorder')
ACCOUNT = 'asterion-recorder'
CREDENTIALS = Path('/etc/credstore.encrypted')
UNITS = Path('/etc/systemd/system')
SETTINGS = CONFIG / 'recorder.json'
FILES = ('asterion_recorder', 'ctp-md.so', 'ctp-trader.so', 'recording-host.py', 'recorder.json')
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
    current = PROGRAMS / 'current'
    return ('[Unit]\nDescription=Asterion market recorder\n'
            'Wants=network-online.target\nAfter=network-online.target\n'
            # Never give up restarting: a recorder that stays down loses the market.
            'StartLimitIntervalSec=0\n'
            f'[Service]\nUser={ACCOUNT}\nGroup={ACCOUNT}\nUMask=0077\n'
            # Everything else the recorder runs with is in its configuration file.
            f'ExecStart={current}/asterion_recorder --config {SETTINGS}\n'
            f'Environment=ASTERION_LOG_DIRECTORY={STATE}/logs\n'
            'StateDirectory=asterion-recorder\nStateDirectoryMode=0700\n'
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
            if name not in ('recording-host.py', 'recorder.json') and data[:4] != b'\x7fELF':
                raise ValueError(f'Not a Linux program: {name}')
            files[name] = data
        return manifest, files


def install_release(bundle):
    """Unpacks a bundle into its own release directory; returns it."""
    manifest, files = read_bundle(bundle)
    identity = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:12]
    release = PROGRAMS / 'releases' / f"{manifest['product_version']}-{identity}"
    if not release.exists():
        staging = Path(tempfile.mkdtemp(prefix='.staging-', dir=PROGRAMS / 'releases'))
        for name, data in files.items():
            (staging / name).write_bytes(data)
            (staging / name).chmod(0o755)
        version = run(str(staging / 'asterion_recorder'), '--version', capture_output=True, text=True).stdout
        if 'Asterion Recorder' not in version:
            raise ValueError('The bundled recorder does not run on this host')
        staging.chmod(0o755)
        staging.rename(release)
    return release


def switch(release):
    """Points `current` at the release in one rename and keeps one older release."""
    link = PROGRAMS / 'current'
    previous = link.resolve() if link.is_symlink() else None
    temporary = PROGRAMS / '.current'
    if temporary.is_symlink():
        temporary.unlink()
    temporary.symlink_to(release)
    temporary.rename(link)
    for old in (PROGRAMS / 'releases').iterdir():
        if old not in (release, previous) and not old.name.startswith('.'):
            shutil.rmtree(old)


def account():
    try:
        return pwd.getpwnam(ACCOUNT)
    except KeyError:
        run('/usr/sbin/useradd', '--system', '--no-create-home', '--home-dir', str(STATE),
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
    account()
    (PROGRAMS / 'releases').mkdir(parents=True, exist_ok=True)
    CONFIG.mkdir(mode=0o755, exist_ok=True)
    release = install_release(args.bundle)
    if (PROGRAMS / 'current').exists():
        raise ValueError('The recorder is installed already; use upgrade')
    switch(release)
    # Copied once: later installs and upgrades never overwrite your edits.
    if not SETTINGS.exists():
        write(SETTINGS, (release / 'recorder.json').read_text(), 0o640, account().pw_gid)
    write(UNITS / UNIT, unit_text(), 0o644)
    run('/usr/bin/systemctl', 'daemon-reload')
    run('/usr/bin/systemctl', 'enable', UNIT)
    print(f'Installed {release.name}. Next: sudo {sys.argv[0]} login')
    port = json.loads(SETTINGS.read_text())['port']
    print(f'Settings: {SETTINGS}. It runs as {ACCOUNT} and listens on TCP {port}, unencrypted: allow the '
          f'port only from your own machines, e.g. ufw allow from <address> to any port {port} proto tcp')


def login(args):
    outside_trading('Changing the login')
    fields = {}
    for key, prompt in (('market_front', '行情前置（tcp://地址:端口）'), ('trade_front', '交易前置（tcp://地址:端口）'),
                        ('broker', '经纪公司代码'), ('user', '投资者代码'), ('app_id', 'AppID（没有则留空）')):
        fields[key] = input(f'{prompt}: ').strip()
        if key != 'app_id' and not fields[key]:
            raise ValueError(f'{key} is required')
    for key in ('market_front', 'trade_front'):
        if not re.fullmatch(r'tcp://[A-Za-z0-9.-]+:[0-9]{1,5}', fields[key]):
            raise ValueError(f'{key} must look like tcp://host:port')
    secrets = [('asterion.ctp.password', getpass.getpass('密码（不显示）: '))]
    if fields['app_id']:
        secrets.append(('asterion.ctp.auth_code', getpass.getpass('授权码（不显示）: ')))
    CREDENTIALS.mkdir(mode=0o700, exist_ok=True)
    for name, value in secrets:
        if not value:
            raise ValueError(f'{name} is required')
        run('/usr/bin/systemd-creds', 'encrypt', f'--name={name}', '-', str(CREDENTIALS / name),
            input=value.encode(), stdout=subprocess.DEVNULL)
    # The account is not a secret, but only the recorder needs to read it.
    settings = json.loads(SETTINGS.read_text())
    settings['counter'] = fields
    write(SETTINGS, json.dumps(settings, ensure_ascii=False, indent=2) + '\n', 0o640, account().pw_gid)
    run('/usr/bin/systemctl', 'restart', UNIT)
    print('The recorder restarted with this login. Check it: systemctl status asterion-recorder; '
          f'logs in {STATE}/logs')


def upgrade(args):
    release = install_release(args.bundle)
    outside_trading('An upgrade')
    switch(release)
    if active():
        run('/usr/bin/systemctl', 'restart', UNIT)
    print(f'Running {release.name}.')


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('install')
    command.add_argument('bundle')
    command.set_defaults(action=install)
    commands.add_parser('login').set_defaults(action=login)
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
