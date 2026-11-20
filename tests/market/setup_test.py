"""setup.sh installs, upgrades and configures Market. It runs as root in a
user namespace against a temporary root, with stand-ins for the system
commands (chown, systemctl, systemd-creds, systemd-run, date) that
record what they were asked.

    setup_test.py <build>/apps/services/market/setup.sh
"""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tarfile
import tempfile

setup = Path(sys.argv[1])
logging = Path(__file__).parents[2] / 'apps/services/market/config/logging.json'
namespace = [] if os.geteuid() == 0 else ['unshare', '-r']
if namespace and subprocess.run(namespace + ['true'], capture_output=True).returncode != 0:
    print('SKIP: unprivileged user namespaces are unavailable')
    sys.exit(77)

STAND_IN = r'''#!/bin/bash
name=$(basename "$0")
{ printf '%s' "$name"; printf ' %s' "$@"; printf '\n'; } >> "$CALLS"
case $name in
    systemctl)
        case $1 in
            is-active)
                unit=${@: -1}
                if [[ $unit == asterion-market-restart.timer ]]; then
                    [[ -f $TIMERS/asterion-market-restart ]] || exit 3
                else
                    exit "${ACTIVE:-3}"
                fi ;;
            stop)
                shift
                for unit in "$@"; do
                    [[ $unit != *.timer ]] || rm -f "$TIMERS/${unit%.timer}"
                done ;;
            enable) touch "$BOOT/${@: -1}" ;;
            disable) rm -f "$BOOT/${@: -1}" ;;
            is-enabled)
                if [[ -f $BOOT/$2 ]]; then echo enabled;
                else echo disabled; exit 1; fi ;;
        esac ;;
    systemd-run)
        for arg in "$@"; do
            if [[ $arg == --unit=* ]]; then touch "$TIMERS/${arg#--unit=}"; fi
        done ;;
    systemd-creds) printf '%s=' "$2" >> "$SECRETS"; cat >> "$SECRETS"; echo >> "$SECRETS" ;;
    timedatectl) echo "${TIME_SYNC:-yes}" ;;
    date) case "$*" in *%u*) echo "$NOW" ;; *tomorrow*) echo 2026-10-30 ;; *) echo 2026-10-29 ;; esac ;;
esac
'''


def release(folder, version):
    top = Path(folder) / f'stage-{version}' / 'asterion-market'
    for part in ('bin', 'lib'):
        (top / part).mkdir(parents=True)
    (top / 'bin/asterion_market').write_text(f'#!/bin/sh\necho "Asterion Market {version}"\n')
    (top / 'bin/asterion_market').chmod(0o755)
    shutil.copy2(setup, top / 'bin/setup.sh')
    (top / 'bin/cold_backup.py').write_text('#!/usr/bin/env python3\n')
    (top / 'bin/cold_backup.py').chmod(0o755)
    (top / 'bin/asterion_crash_reporter').write_text('#!/bin/sh\nexit 0\n')
    (top / 'bin/asterion_crash_reporter').chmod(0o755)
    for library in ('ctp-md.so', 'ctp-trader.so'):
        (top / 'lib' / library).write_text(version)
    (top / 'config').mkdir()
    shutil.copy2(logging, top / 'config/logging.json')
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:gz') as archive:
        archive.add(top, 'asterion-market')
    script = Path(folder) / f'Asterion-Market-{version}-linux-x86_64.sh'
    script.write_bytes(setup.read_bytes() + b'__ARCHIVE__\n' + payload.getvalue())
    return script


class Host:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.root = self.folder / 'opt/asterion-market'
        self.units = self.folder / 'units'
        self.credentials = self.folder / 'credstore'
        self.shortcuts = self.folder / 'shortcuts'
        self.timers = self.folder / 'timers'
        self.boot = self.folder / 'boot'
        for path in (self.units, self.shortcuts, self.timers, self.boot):
            path.mkdir()
        fakes = self.folder / 'fakes'
        fakes.mkdir()
        for name in ('chown', 'systemctl', 'systemd-creds', 'systemd-run', 'date', 'timedatectl'):
            (fakes / name).write_text(STAND_IN)
            (fakes / name).chmod(0o755)
        self.calls, self.secrets = self.folder / 'calls', self.folder / 'secrets'
        self.env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(self.calls),
                        SECRETS=str(self.secrets), TIMERS=str(self.timers), BOOT=str(self.boot),
                        SUDO_USER='tester')

    def run(self, script, answers, active=False, now='4 1700', args=()):
        for record in (self.calls, self.secrets):
            record.unlink(missing_ok=True)
        command = (
            'source "$6"; SELF=$(readlink -f "$1"); ROOT=$2; UNITS=$3; '
            'CREDENTIALS=$4; SHORTCUT_DIR=$5; main "${@:7}"'
        )
        env = dict(self.env, ACTIVE='0' if active else '3', NOW=now)
        result = subprocess.run(
            namespace + ['bash', '-c', command, 'setup', str(script), str(self.root), str(self.units),
             str(self.credentials), str(self.shortcuts), str(setup), *args],
            input='\n'.join(answers) + '\n', capture_output=True, text=True, env=env, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return calls, result

    def settings(self):
        return json.loads((self.root / 'config/market.json').read_text())


client_pin = 'a' * 64
shipped = {'bind': '0.0.0.0', 'port': 7451, 'clients': [dict(fingerprint=client_pin)],
           'counter': {'market_front': 'tcp://182.254.243.31:40011', 'trade_front': 'tcp://182.254.243.31:40001',
                       'broker': '9999', 'user': '', 'app_id': 'simnow_client_test'}}
with tempfile.TemporaryDirectory() as folder:
    host = Host(folder)

    # Install: layout, unit, shortcut, settings, Terminals, first start.
    calls, result = host.run(release(folder, '1'),
                             ['', 'abc', '7461', '', '', '', '', '273005', '', client_pin, '', 'fixture-password', 'fixture-auth'])
    assert '输入格式无效' in result.stderr, result.stderr
    assert sorted(p.name for p in host.root.iterdir()) == ['.program.lock', 'bin', 'config', 'data', 'lib', 'logs']
    assert (host.root / 'bin/asterion_market').read_text().endswith('1"\n')
    assert (host.root / 'bin/setup.sh').read_bytes() == setup.read_bytes()
    assert os.access(host.root / 'bin/cold_backup.py', os.X_OK)
    assert (host.shortcuts / 'asterion-market').resolve() == (host.root / 'bin/setup.sh').resolve()
    assert (host.root / 'config/logging.json').read_bytes() == logging.read_bytes()
    assert host.settings() == {**shipped, 'port': 7461, 'data': str(host.root / 'data'),
                               'counter': {**shipped['counter'], 'user': '273005'}}
    text = (host.root / 'config/market.json').read_text()
    assert 'fixture-password' not in text and 'fixture-auth' not in text
    assert oct((host.root / 'config/market.json').stat().st_mode & 0o777) == '0o600'
    assert host.secrets.read_text().splitlines() == [
        '--name=asterion.ctp.password=fixture-password', '--name=asterion.ctp.auth_code=fixture-auth']
    unit = (host.units / 'asterion-market.service').read_text()
    for line in ('User=tester', 'Restart=always', 'StartLimitIntervalSec=0',
                 'Wants=network-online.target time-sync.target',
                 'After=network-online.target time-sync.target',
                 'ImportCredential=asterion.ctp.*', f'ExecStart={host.root}/bin/asterion_market',
                 f'ReadWritePaths={host.root}/logs {host.root}/data'):
        assert line + '\n' in unit, line
    assert any(call.startswith('chown tester: ') for call in calls), calls
    assert 'systemctl enable asterion-market.service' in calls, calls
    assert calls[-1] == 'systemctl restart asterion-market.service', calls
    assert 'fixture-password' not in result.stdout + result.stderr
    assert (host.root / 'config/tls/server.key').stat().st_mode & 0o777 == 0o600

    # Upgrade: new programs, settings untouched when already in current format.
    (host.root / 'config/market.json').write_text(json.dumps({**host.settings(), 'port': 7999}))
    (host.root / 'config/logging.json').write_text('{"level": "debug", "retention_days": 7}')
    before = (host.root / 'config/market.json').read_text()
    preserved = {
        'data/ctp-catalog.pb': b'fixture-catalog',
        'logs/asterion_market_2026-10-29.log': b'fixture-log\n',
    }
    for name, content in preserved.items():
        path = host.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    preserved.update({str(path.relative_to(host.root)): path.read_bytes()
                      for path in (host.root / 'config/tls').iterdir()})
    second = release(folder, '2')
    calls, result = host.run(second, [''])
    assert (host.root / 'bin/asterion_market').read_text().endswith('2"\n')
    assert (host.root / 'lib/ctp-md.so').read_text() == '2'
    assert (host.root / 'config/market.json').read_text() == before
    assert json.loads((host.root / 'config/logging.json').read_text())['level'] == 'debug'
    assert not host.secrets.exists()
    assert calls[-1] == 'systemctl restart asterion-market.service', calls
    assert all((host.root / name).read_bytes() == content for name, content in preserved.items())

    # Recording during trading hours: the upgrade is placed, the restart waits.
    for now, at in (('4 1000', '2026-10-29 15:30'), ('4 2200', '2026-10-30 02:45'),
                    ('5 0100', '2026-10-29 02:45')):
        calls, result = host.run(second, [''], active=True, now=now)
        assert 'systemctl restart asterion-market.service' not in calls, (now, calls)
        assert calls[-1] == ('systemd-run --quiet --collect --unit=asterion-market-restart '
                             f'--on-calendar={at}:00 Asia/Shanghai systemctl restart '
                             'asterion-market.service'), (now, calls)
        assert at in result.stdout, result.stdout
    for now in ('4 1530', '4 0245', '6 1000', '1 0100'):
        calls, _ = host.run(second, [''], active=True, now=now)
        assert calls[-1] == 'systemctl restart asterion-market.service', (now, calls)

    # Terminal management (5) adds a Terminal; it takes effect after the trading window.
    calls, result = host.run(host.shortcuts / 'asterion-market', ['5', '1', 'b' * 64, '', '0', '0'],
                             active=True, now='4 1000')
    assert host.settings()['clients'] == [dict(fingerprint=client_pin), dict(fingerprint='b' * 64)]
    assert calls[-1].startswith('systemd-run --quiet --collect --unit=asterion-market-restart'), calls
    # Immediate restart (1) during trading hours asks confirmation first.
    calls, _ = host.run(host.shortcuts / 'asterion-market', ['1', 'n', '0'], active=True, now='4 1000')
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls), calls
    calls, _ = host.run(host.shortcuts / 'asterion-market', ['1', 'y', '0'], active=True, now='4 1000')
    assert 'systemctl stop asterion-market-restart.timer asterion-market-restart.service' in calls, calls
    assert 'systemctl restart asterion-market.service' in calls, calls
    assert not any(call.startswith('systemd-run') for call in calls), calls

    # Changing settings via full wizard (2 -> 6) updates data directory and AppID.
    disk = host.folder / 'disk/market-data'
    calls, result = host.run(host.root / 'bin/setup.sh',
                             ['2', '6', '', '', str(disk), '', '', '', '', '-', 'fixture-password', '', '0', '0'])
    assert 'CTP AppID: simnow_client_test -> 无' in result.stdout, result.stdout
    assert host.settings()['port'] == 7999 and host.settings()['counter']['app_id'] == ''
    assert host.settings()['data'] == str(disk)
    assert disk.is_dir() and any(call.startswith('chown ') and call.endswith(f': {disk}')
                                 for call in calls), calls
    unit = (host.units / 'asterion-market.service').read_text()
    assert f'ReadWritePaths={host.root}/logs {disk}\n' in unit, unit
    assert f'原有数据仍保留在 {host.root}/data' in result.stdout, result.stdout
    assert (host.root / 'data/ctp-catalog.pb').read_bytes() == b'fixture-catalog'
    assert host.secrets.read_text().splitlines() == ['--name=asterion.ctp.password=fixture-password']
    assert not (host.credentials / 'asterion.ctp.auth_code').exists()
    assert calls[-1] == 'systemctl restart asterion-market.service', calls

    # The same counter account keeps its saved password; nothing changed is not saved or restarted.
    calls, result = host.run(host.root / 'bin/setup.sh', ['2', '6'] + [''] * 9 + ['0', '0'])
    assert '未作任何修改。' in result.stdout, result.stdout
    assert not host.secrets.exists()
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls), calls

    def staged_files():
        return sorted(str(path) for folder in (host.root, host.units, host.credentials)
                      for path in folder.rglob('.*') if path.is_file() and path.name != '.program.lock')
    assert staged_files() == [], staged_files()
    (host.units / 'asterion-market.service').write_text('[Service]\n')
    calls, _ = host.run(second, [''])
    assert 'ImportCredential=asterion.ctp.*' in (host.units / 'asterion-market.service').read_text()
    assert 'systemctl daemon-reload' in calls, calls

    # Input that ends before the credentials writes no settings.
    before = (host.root / 'config/market.json').read_bytes()
    result = subprocess.run(
        namespace + ['bash', '-c', 'source "$5"; SELF=$1; ROOT=$2; UNITS=$3; CREDENTIALS=$4; main',
         'setup', str(host.root / 'bin/setup.sh'), str(host.root), str(host.units), str(host.credentials),
         str(setup)], input='2\n6\n' + '\n' * 8, capture_output=True, text=True,
        env=dict(host.env, ACTIVE='3', NOW='4 1700'), timeout=60)
    assert result.returncode != 0, result.stdout + result.stderr
    assert (host.root / 'config/market.json').read_bytes() == before
    assert staged_files() == [], staged_files()

    # Fingerprint via shortcut restarts nothing.
    calls, result = host.run(host.shortcuts / 'asterion-market', [], args=['fingerprint'])
    der = ssl.PEM_cert_to_DER_cert((host.root / 'config/tls/server.crt').read_text())
    value = ':'.join(f'{b:02X}' for b in hashlib.sha256(der).digest())
    assert result.stdout.splitlines() == ['本机服务端证书指纹（请填入 Terminal）：', '  ' + value], result.stdout
    assert calls == [], calls

    result = subprocess.run(namespace + ['bash', '-c', 'source "$1"; SELF=$1; ROOT=$2; main', 'setup',
                             str(setup), str(host.root)], capture_output=True, text=True, env=host.env, timeout=60)
    assert result.returncode != 0 and 'Asterion-Market-VERSION-linux-x86_64.sh' in result.stderr, result.stderr
print('installed, upgraded, deferred and configured from a self-extracting script')
