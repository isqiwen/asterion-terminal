"""setup.sh installs, upgrades and configures Warehouse. It runs as root in a
user namespace against a temporary root, with stand-ins for the system
commands (chown, systemctl, systemd-creds, systemd-run, date) that
record what they were asked.

    setup_test.py <build>/apps/services/warehouse/setup.sh
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
logging = Path(__file__).parents[2] / 'apps/services/warehouse/config/logging.json'
exchange_rules = Path(__file__).parents[2] / 'apps/services/warehouse/config/exchange_rules.json'
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
            is-active) exit "${ACTIVE:-3}" ;;
            enable) touch "$BOOT/${@: -1}" ;;
            disable) rm -f "$BOOT/${@: -1}" ;;
            is-enabled)
                if [[ -f $BOOT/$2 ]]; then echo enabled;
                else echo disabled; exit 1; fi ;;
        esac ;;
    systemd-creds) printf '%s=' "$2" >> "$SECRETS"; cat >> "$SECRETS"; echo >> "$SECRETS" ;;
    timedatectl) echo "${TIME_SYNC:-yes}" ;;
    date) case "$*" in *%u*) echo "$NOW" ;; *tomorrow*) echo 2026-10-30 ;; *) echo 2026-10-29 ;; esac ;;
esac
'''


def release(folder, version):
    top = Path(folder) / f'stage-{version}' / 'asterion-warehouse'
    (top / 'bin').mkdir(parents=True)
    (top / 'bin/asterion_warehouse').write_text(f'#!/bin/sh\necho "Asterion Warehouse {version}"\n')
    (top / 'bin/asterion_warehouse').chmod(0o755)
    shutil.copy2(setup, top / 'bin/setup.sh')
    (top / 'bin/cold_backup.py').write_text('#!/usr/bin/env python3\n')
    (top / 'bin/cold_backup.py').chmod(0o755)
    (top / 'bin/asterion_crash_reporter').write_text('#!/bin/sh\nexit 0\n')
    (top / 'bin/asterion_crash_reporter').chmod(0o755)
    (top / 'config').mkdir()
    shutil.copy2(logging, top / 'config/logging.json')
    shutil.copy2(exchange_rules, top / 'config/exchange_rules.json')
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:gz') as archive:
        archive.add(top, 'asterion-warehouse')
    script = Path(folder) / f'Asterion-Warehouse-{version}-linux-x86_64.sh'
    script.write_bytes(setup.read_bytes() + b'__ARCHIVE__\n' + payload.getvalue())
    return script


class Host:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.root = self.folder / 'opt/asterion-warehouse'
        self.units = self.folder / 'units'
        self.credentials = self.folder / 'credstore'
        self.shortcuts = self.folder / 'shortcuts'
        self.boot = self.folder / 'boot'
        for path in (self.units, self.shortcuts, self.boot):
            path.mkdir()
        fakes = self.folder / 'fakes'
        fakes.mkdir()
        for name in ('chown', 'systemctl', 'systemd-creds', 'systemd-run', 'date', 'timedatectl'):
            (fakes / name).write_text(STAND_IN)
            (fakes / name).chmod(0o755)
        self.calls, self.secrets = self.folder / 'calls', self.folder / 'secrets'
        self.env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(self.calls),
                        SECRETS=str(self.secrets), BOOT=str(self.boot), SUDO_USER='tester')

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
        return json.loads((self.root / 'config/warehouse.json').read_text())


client_pin = 'a' * 64
shipped = {'bind': '0.0.0.0', 'port': 7450,
           'clients': [dict(fingerprint=client_pin)],
           'acquisition': {'exchanges': ['SHFE', 'INE', 'CZCE', 'DCE', 'CFFEX', 'GFEX'], 'tushare': {'requests_per_minute': 500}, 'update_at': '06:00'}}
with tempfile.TemporaryDirectory() as folder:
    host = Host(folder)

    # Install: layout, unit, shortcut symlink, settings, Terminals, first start.
    calls, result = host.run(release(folder, '1'),
                             ['', 'abc', '7451', '', '', '', '', client_pin, '', 'fixture-token'])
    assert '输入格式无效' in result.stderr, result.stderr
    assert sorted(p.name for p in host.root.iterdir()) == ['.program.lock', 'bin', 'config', 'data', 'logs']
    assert (host.root / 'bin/asterion_warehouse').read_text().endswith('1"\n')
    assert (host.root / 'bin/setup.sh').read_bytes() == setup.read_bytes()
    assert os.access(host.root / 'bin/cold_backup.py', os.X_OK)
    assert (host.shortcuts / 'asterion-warehouse').resolve() == (host.root / 'bin/setup.sh').resolve()
    assert (host.root / 'config/logging.json').read_bytes() == logging.read_bytes()
    assert json.loads((host.root / 'config/exchange_rules.json').read_text()) == json.loads(exchange_rules.read_text())
    assert host.settings() == {**shipped, 'port': 7451, 'data': str(host.root / 'data')}
    text = (host.root / 'config/warehouse.json').read_text()
    assert 'fixture-token' not in text
    assert oct((host.root / 'config/warehouse.json').stat().st_mode & 0o777) == '0o600'
    assert host.secrets.read_text().splitlines() == [
        '--name=asterion.warehouse.tushare.token=fixture-token']
    unit = (host.units / 'asterion-warehouse.service').read_text()
    for line in ('User=tester', 'Restart=always', 'StartLimitIntervalSec=0',
                 'Wants=network-online.target time-sync.target',
                 'After=network-online.target time-sync.target',
                 'ImportCredential=asterion.warehouse.*', f'ExecStart={host.root}/bin/asterion_warehouse',
                 f'ReadWritePaths={host.root}/logs {host.root}/data'):
        assert line + '\n' in unit, line
    assert any(call.startswith('chown tester: ') for call in calls), calls
    assert 'systemctl enable asterion-warehouse.service' in calls, calls
    assert calls[-1] == 'systemctl restart asterion-warehouse.service', calls
    assert 'fixture-token' not in result.stdout + result.stderr
    assert (host.root / 'config/tls/server.key').stat().st_mode & 0o777 == 0o600

    # Upgrade: new programs, settings refreshed to current release format, records kept.
    earlier = {**host.settings(), 'port': 7999, 'clients': [dict(fingerprint=client_pin, role='manage')]}
    earlier['acquisition'] = {k: v for k, v in earlier['acquisition'].items() if k != 'update_at'}
    (host.root / 'config/warehouse.json').write_text(json.dumps(earlier))
    (host.root / 'config/logging.json').write_text('{"level": "debug", "retention_days": 7}')
    advanced_rules = json.loads((host.root / 'config/exchange_rules.json').read_text())
    advanced_rules['dce_settlement_fees']['verified_through'] = '2027-06-30'
    (host.root / 'config/exchange_rules.json').write_text(json.dumps(advanced_rules))
    preserved = {
        'data/sources/sina/warehouse/current': b'published-version',
        'data/sources/sina/warehouse/versions/published-version.json': b'fixture-manifest',
        'data/acquisition/published.pb': b'fixture-task',
        'logs/asterion_warehouse_2026-10-29.log': b'fixture-log\n',
    }
    for name, content in preserved.items():
        path = host.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    preserved.update({str(path.relative_to(host.root)): path.read_bytes()
                      for path in (host.root / 'config/tls').iterdir()})
    second = release(folder, '2')
    calls, result = host.run(second, [''])
    assert (host.root / 'bin/asterion_warehouse').read_text().endswith('2"\n')
    assert host.settings() == {**shipped, 'port': 7999, 'data': str(host.root / 'data')}, host.settings()
    assert 'acquisition.update_at: 新增默认值 "06:00"' in result.stdout, result.stdout
    assert 'clients: [{"fingerprint"' in result.stdout, result.stdout
    assert oct((host.root / 'config/warehouse.json').stat().st_mode & 0o777) == '0o600'
    assert json.loads((host.root / 'config/logging.json').read_text())['level'] == 'debug'
    assert json.loads((host.root / 'config/exchange_rules.json').read_text())['dce_settlement_fees']['verified_through'] == '2027-06-30'
    assert not host.secrets.exists()
    assert calls[-1] == 'systemctl restart asterion-warehouse.service', calls
    assert all((host.root / name).read_bytes() == content for name, content in preserved.items())

    # Shortcut opens the panel: Terminal fingerprint sub-menu (5) adds/removes pins and restarts once.
    other_pin = 'b' * 64
    calls, result = host.run(host.shortcuts / 'asterion-warehouse',
                             ['5', '1', 'xyz', ':'.join(['BB'] * 32), client_pin, '', '2', '1 2', '1', '0', '0'])
    assert '64 位十六进制' in result.stderr and '已登记' in result.stderr, result.stderr
    assert '至少须保留一台' in result.stderr, result.stderr
    assert host.settings()['clients'] == [dict(fingerprint=other_pin)]
    assert calls.count('systemctl restart asterion-warehouse.service') == 1, calls

    # Granular single-item edit (2 -> 5: update_at only) without stepping through all prompts.
    calls, result = host.run(host.shortcuts / 'asterion-warehouse', ['2', '5', '07:30', '', '0', '0'])
    assert '每日自动更新: 06:00 -> 07:30' in result.stdout, result.stdout
    assert host.settings()['acquisition']['update_at'] == '07:30'
    assert calls[-1] == 'systemctl restart asterion-warehouse.service', calls

    # Service lifecycle controls: autostart toggle (4), logs view (6), stop service (3).
    calls, result = host.run(host.shortcuts / 'asterion-warehouse', ['4', '4', '6', '3', 'y', '0'])
    assert 'fixture-log' in result.stdout, result.stdout
    assert 'systemctl disable asterion-warehouse.service' in calls, calls
    assert 'systemctl enable asterion-warehouse.service' in calls, calls
    assert 'systemctl stop asterion-warehouse.service' in calls, calls

    # Full wizard (2 -> 7): changes data directory and token, leaving old data untouched.
    disk = host.folder / 'disk/market-data'
    calls, result = host.run(host.root / 'bin/setup.sh',
                             ['2', '7', '', '', str(disk), '', '', '-', 'n', 'second-token', '', '0', '0'])
    assert f'数据存储目录: {host.root}/data -> {disk}' in result.stdout, result.stdout
    assert '每日自动更新: 07:30 -> -' in result.stdout, result.stdout
    assert host.settings()['acquisition']['update_at'] is None
    assert host.settings()['port'] == 7999
    assert host.settings()['data'] == str(disk)
    assert disk.is_dir() and any(call.startswith('chown ') and call.endswith(f': {disk}')
                                 for call in calls), calls
    unit = (host.units / 'asterion-warehouse.service').read_text()
    assert f'ReadWritePaths={host.root}/logs {disk}\n' in unit, unit
    assert f'原有数据仍保留在 {host.root}/data' in result.stdout, result.stdout
    assert (host.root / 'data/acquisition/published.pb').read_bytes() == b'fixture-task'
    assert host.secrets.read_text().splitlines() == ['--name=asterion.warehouse.tushare.token=second-token']
    assert calls[-1] == 'systemctl restart asterion-warehouse.service', calls

    # Enter keeps every value and the saved token: nothing is saved or restarted.
    before = (host.root / 'config/warehouse.json').read_bytes()
    calls, result = host.run(host.root / 'bin/setup.sh', ['2', '7'] + [''] * 7 + ['0', '0'])
    assert '未作任何修改。' in result.stdout, result.stdout
    assert not host.secrets.exists()
    assert (host.root / 'config/warehouse.json').read_bytes() == before
    assert not any(call.startswith('systemctl restart') for call in calls), calls

    # Nothing a run stages is left behind, and an upgrade writes its unit again.
    def staged_files():
        return sorted(str(path) for folder in (host.root, host.units, host.credentials)
                      for path in folder.rglob('.*') if path.is_file() and path.name != '.program.lock')
    assert staged_files() == [], staged_files()
    (host.units / 'asterion-warehouse.service').write_text('[Service]\n')
    before = (host.root / 'config/warehouse.json').read_bytes()
    calls, result = host.run(second, [''])
    assert (host.root / 'config/warehouse.json').read_bytes() == before
    assert '升级为当前版本格式' not in result.stdout, result.stdout
    assert 'ImportCredential=asterion.warehouse.*' in (host.units / 'asterion-warehouse.service').read_text()
    assert 'systemctl daemon-reload' in calls, calls

    # Input that ends early writes no settings.
    before = (host.root / 'config/warehouse.json').read_bytes()
    result = subprocess.run(
        namespace + ['bash', '-c', 'source "$5"; SELF=$1; ROOT=$2; UNITS=$3; CREDENTIALS=$4; main',
         'setup', str(host.root / 'bin/setup.sh'), str(host.root), str(host.units), str(host.credentials),
         str(setup)], input='2\n7\n' + '\n' * 3, capture_output=True, text=True,
        env=dict(host.env, ACTIVE='3', NOW='4 1700'), timeout=60)
    assert result.returncode != 0, result.stdout + result.stderr
    assert (host.root / 'config/warehouse.json').read_bytes() == before
    assert staged_files() == [], staged_files()

    # Fingerprint command via shortcut shows the certificate fingerprint and restarts nothing.
    calls, result = host.run(host.shortcuts / 'asterion-warehouse', [], args=['fingerprint'])
    der = ssl.PEM_cert_to_DER_cert((host.root / 'config/tls/server.crt').read_text())
    value = ':'.join(f'{b:02X}' for b in hashlib.sha256(der).digest())
    assert result.stdout.splitlines() == ['本机服务端证书指纹（请填入 Terminal）：', '  ' + value], result.stdout
    assert calls == [], calls

    # The plain script elsewhere carries no programs.
    result = subprocess.run(namespace + ['bash', '-c', 'source "$1"; SELF=$1; ROOT=$2; main', 'setup',
                             str(setup), str(host.root)], capture_output=True, text=True, env=host.env, timeout=60)
    assert result.returncode != 0 and 'Asterion-Warehouse-VERSION-linux-x86_64.sh' in result.stderr, result.stderr
print('installed, upgraded and configured from a self-extracting script')
