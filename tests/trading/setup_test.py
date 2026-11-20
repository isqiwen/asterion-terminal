"""setup.sh installs and upgrades the trading service and adds, changes and
disables account instances. It runs as root in a user namespace against a
temporary root, with stand-ins for the system commands (chown, systemctl,
systemd-run, date) that record what they were asked; openssl is real.

    setup_test.py <build>/apps/services/trading/setup.sh
"""
import io
import fcntl
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tarfile
import tempfile

setup = Path(sys.argv[1])
logging = Path(__file__).parents[2] / 'apps/services/trading/config/logging.json'
if not shutil.which('unshare') or subprocess.run(['unshare', '-r', 'true'], capture_output=True).returncode != 0:
    print('SKIP: unprivileged user namespaces are unavailable')
    sys.exit(77)

STAND_IN = r'''#!/bin/bash
name=$(basename "$0")
{ printf '%s' "$name"; printf ' %s' "$@"; printf '\n'; } >> "$CALLS"
case $name in
    systemctl)
        if [[ ${FAIL_RELOAD:-} == 1 && $1 == daemon-reload ]]; then exit 1; fi
        case $1 in
            is-active)
                unit=${@: -1}
                if [[ $unit == asterion-trading-restart-*.timer ]]; then
                    [[ -f $TIMERS/${unit%.timer} ]] || exit 3
                else
                    [[ $RUNNING == '*' || " $RUNNING " == *" $unit "* ]] || exit 3
                fi ;;
            stop)
                shift
                for unit in "$@"; do
                    [[ $unit != *.timer ]] || rm -f "$TIMERS/${unit%.timer}"
                done ;;
            show)
                if [[ $* == *NextElapseUSecRealtime* ]]; then echo '2026-10-29 15:30:00';
                elif [[ $RUNNING == '*' || " $RUNNING " == *" $2 "* ]]; then echo active;
                else echo inactive; fi ;;
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
    systemd-creds) cat > "${@: -1}" ;;
    timedatectl) echo "${TIME_SYNC:-yes}" ;;
    date) case "$*" in *%u*) echo "$NOW" ;; *tomorrow*) echo 2026-10-30 ;; *) echo 2026-10-29 ;; esac ;;
esac
'''
TERMINAL = ':'.join(['AB'] * 32)
OTHER = 'cd' * 32
IDENTITY = dict(market_front='tcp://127.0.0.1:41213', environment='simulation', broker=dict(broker_id='9999', user_id='000001', app_id='client_app', front='tcp://127.0.0.1:41205'))


def release(folder, version):
    top = Path(folder) / f'stage-{version}' / 'asterion-trading'
    for part in ('bin', 'lib'):
        (top / part).mkdir(parents=True)
    (top / 'bin/asterion_trading').write_text(f'#!/bin/sh\necho "Asterion Trading {version}"\n')
    (top / 'bin/asterion_trading').chmod(0o755)
    shutil.copy2(setup, top / 'bin/setup.sh')
    (top / 'bin/cold_backup.py').write_text('#!/usr/bin/env python3\n')
    (top / 'bin/cold_backup.py').chmod(0o755)
    runtime = top / 'runtime/python/bin/python3'
    runtime.parent.mkdir(parents=True)
    runtime.write_text('#!/bin/sh\n[ "$2" != -B ] || exit 0\nexec ' + shlex.quote(sys.executable) + ' "$@"\n')
    runtime.chmod(0o755)
    (top / 'runtime/release').write_text(version)
    (top / 'lib/ctp-trader.so').write_text(version)
    (top / 'lib/ctp-md.so').write_text(version)
    for name in ('strategy_runner.py', 'asterion_strategy.py', 'asterion/strategy_pb2.py',
                 'asterion/common_pb2.py'):
        path = top / 'lib/strategy' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('# ' + version)
    (top / 'config').mkdir()
    shutil.copy2(logging, top / 'config/logging.json')
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:gz') as archive:
        archive.add(top, 'asterion-trading')
    script = Path(folder) / f'Asterion-Trading-{version}-linux-x86_64.sh'
    script.write_bytes(setup.read_bytes() + b'__ARCHIVE__\n' + payload.getvalue())
    return script


class Host:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.root = self.folder / 'opt/asterion/trading'
        self.units = self.folder / 'units'
        self.shortcuts = self.folder / 'shortcuts'
        self.timers = self.folder / 'timers'
        self.boot = self.folder / 'boot'
        for path in (self.units, self.shortcuts, self.timers, self.boot):
            path.mkdir()
        fakes = self.folder / 'fakes'
        fakes.mkdir()
        for name in ('chown', 'systemctl', 'systemd-run', 'date', 'systemd-creds', 'timedatectl'):
            (fakes / name).write_text(STAND_IN)
            (fakes / name).chmod(0o755)
        self.calls = self.folder / 'calls'
        self.env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(self.calls),
                        SUDO_USER='tester', TIMERS=str(self.timers), BOOT=str(self.boot))

    def run(self, script, answers, active=False, now='4 1700', ok=True, running=None, args=()):
        self.calls.unlink(missing_ok=True)
        command = (
            'source "$5"; SELF=$(readlink -f "$1"); ROOT=$2; UNITS=$3; '
            'CREDENTIALS=$3/credstore; SHORTCUT_DIR=$4; main "${@:6}"'
        )
        result = subprocess.run(
            ['unshare', '-r', 'bash', '-c', command, 'setup', str(script), str(self.root),
             str(self.units), str(self.shortcuts), str(setup), *args],
            input='\n'.join(answers) + '\n', capture_output=True, text=True,
            env=dict(self.env, RUNNING=running if running is not None else ('*' if active else ''), NOW=now), timeout=60)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return calls, result

    def account(self, name):
        return json.loads((self.root / f'config/accounts/{name}.json').read_text())


COUNTER = ['simulation', '9999', '000001', 'client_app', 'tcp://127.0.0.1:41205', 'tcp://127.0.0.1:41213']
CREDENTIALS = ['test-only-password', 'test-only-auth', '']
UNCHANGED = [''] * 8 + ['', '']

with tempfile.TemporaryDirectory() as folder:
    host = Host(folder)

    # A host whose clock is not synchronized gets no installation.
    host.env['TIME_SYNC'] = 'no'
    calls, result = host.run(release(folder, '0'), [], ok=False)
    assert '主机时钟未同步' in result.stderr and not host.root.exists(), result.stderr
    del host.env['TIME_SYNC']

    # Install: layout, shortcut, certificate, unit template, then first account.
    calls, result = host.run(release(folder, '1'), ['bad name', 'one', '', ''] + COUNTER + ['xyz', TERMINAL, ''] + CREDENTIALS)
    assert result.stderr.count('输入格式无效') == 1 and '64 位十六进制' in result.stderr, result.stderr
    assert sorted(p.name for p in host.root.iterdir()) == ['.program.lock', 'bin', 'config', 'data', 'lib', 'logs', 'runtime']
    assert (host.root / 'bin/asterion_trading').read_text().endswith('1"\n')
    assert os.access(host.root / 'bin/setup.sh', os.X_OK)
    assert os.access(host.root / 'bin/cold_backup.py', os.X_OK)
    assert (host.shortcuts / 'asterion-trading').resolve() == (host.root / 'bin/setup.sh').resolve()
    assert (host.root / 'config/logging.json').read_bytes() == logging.read_bytes()
    assert not (host.root / 'config/trading.json').exists()
    assert host.account('one') == {'bind': '0.0.0.0', 'port': 7460, 'terminals': ['ab' * 32], **IDENTITY}
    assert (host.root / 'data/one').is_dir() and (host.root / 'data/owners').is_dir()
    assert oct((host.root / 'config/accounts/one.json').stat().st_mode & 0o777) == '0o600'
    assert oct((host.root / 'config/tls/server.key').stat().st_mode & 0o777) == '0o600'
    fingerprint = subprocess.run(
        ['openssl', 'x509', '-noout', '-fingerprint', '-sha256', '-in',
         str(host.root / 'config/tls/server.crt')], capture_output=True, text=True,
        check=True).stdout.split('=', 1)[1].strip()
    assert fingerprint in result.stdout, result.stdout
    unit = (host.units / 'asterion-trading@.service').read_text()
    for line in ('User=tester', 'Restart=always', 'StartLimitIntervalSec=0', 'WatchdogSec=60',
                 'NotifyAccess=main',
                 'Wants=network-online.target time-sync.target',
                 'After=network-online.target time-sync.target',
                 f'ExecStart={host.root}/bin/asterion_trading --account %i',
                 f'ReadWritePaths={host.root}/logs {host.root}/data/%i {host.root}/data/owners'):
        assert line + '\n' in unit, line
    assert f'LoadCredentialEncrypted=password:{host.units}/credstore/asterion-trading-%i/password' in unit
    assert f'LoadCredentialEncrypted=auth_code:{host.units}/credstore/asterion-trading-%i/auth_code' in unit
    assert 'ImportCredential=asterion.trading.%i.*' in unit
    for name, secret in [('password', 'test-only-password'), ('auth_code', 'test-only-auth')]:
        assert (host.units / f'credstore/asterion-trading-one/{name}').read_text() == secret
        assert secret not in result.stdout + result.stderr + '\n'.join(calls)
        assert secret not in (host.root / 'config/accounts/one.json').read_text()
    assert 'systemctl enable asterion-trading@one.service' in calls, calls
    assert 'systemctl restart asterion-trading@one.service' in calls, calls

    # Adding a second account using pure number choice (2 = add_index when 1 account exists).
    calls, result = host.run(host.shortcuts / 'asterion-trading', ['2', 'two', '127.0.0.1', ''] + COUNTER + [OTHER, TERMINAL, ''] + ['second-only-password', 'second-only-auth', '', '0'])
    assert (host.units / 'credstore/asterion-trading-two/password').read_text() == 'second-only-password'
    assert (host.units / 'credstore/asterion-trading-one/password').read_text() == 'test-only-password'
    assert 'second-only-password' not in result.stdout + result.stderr + '\n'.join(calls)
    assert host.account('two') == {'bind': '127.0.0.1', 'port': 7461, 'terminals': [OTHER, 'ab' * 32], **IDENTITY}
    assert 'systemctl restart asterion-trading@two.service' in calls, calls

    # Granular listen address & port edit (account 2 -> 2 配置与凭据设置 -> 1 监听地址与端口).
    calls, result = host.run(host.shortcuts / 'asterion-trading', ['2', '2', '1', '', '7470', '', '0', '0', '0'])
    assert '监听端口: 7461 -> 7470' in result.stdout, result.stdout
    assert host.account('two') == {'bind': '127.0.0.1', 'port': 7470, 'terminals': [OTHER, 'ab' * 32], **IDENTITY}
    assert 'systemctl restart asterion-trading@two.service' in calls, calls

    # Configuring another counter must never replace a recorded account's identity.
    journal = host.root / 'data/two/journal.sqlite'
    journal.parent.mkdir(parents=True, exist_ok=True)
    journal.touch()
    previous = host.account('two')
    calls, result = host.run(host.root / 'bin/setup.sh', ['2', '2', '5', '', '', 'simulation', '9999', '000009', 'client_app', 'tcp://127.0.0.1:41205', 'tcp://127.0.0.1:41213', '0', '0', '0'])
    assert '已永久绑定原柜台账户身份' in result.stderr, result.stderr
    assert host.account('two') == previous
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls), calls
    # Granular front & AppID edit (2 -> 2) works cleanly on a recorded account.
    calls, _ = host.run(host.root / 'bin/setup.sh', ['2', '2', '2', 'updated_app', 'tcp://127.0.0.1:41206', '', '', '0', '0', '0'])
    assert host.account('two')['broker']['app_id'] == 'updated_app'
    assert host.account('two')['broker']['front'] == 'tcp://127.0.0.1:41206'

    # Shared lock forbids every program/runtime mutation while any account runs.
    second = release(folder, '2')
    before = (host.root / 'config/accounts/one.json').read_bytes()
    with (host.root / '.program.lock').open() as held:
        fcntl.flock(held, fcntl.LOCK_SH | fcntl.LOCK_NB)
        for now in ('4 1700', '4 1000', '4 2200'):
            calls, result = host.run(second, [], active=True, now=now, ok=False)
            assert '请先停止本机所有 Trading 账户实例后再升级' in result.stderr
            assert (host.root / 'bin/asterion_trading').read_text().endswith('1"\n')
            assert (host.root / 'runtime/release').read_text() == '1'
            assert not any(call.startswith(('systemctl restart', 'systemctl stop', 'systemd-run')) for call in calls)

    # Active systemd unit also blocks upgrade even when .program.lock is not yet held.
    calls, result = host.run(second, [], active=True, ok=False)
    assert '请先停止本机所有 Trading 账户实例后再升级' in result.stderr
    assert (host.root / 'bin/asterion_trading').read_text().endswith('1"\n')

    # Offline upgrade replaces program trees, refreshes account files, never starts inactive accounts.
    (host.root / 'runtime/obsolete.py').touch()
    (host.root / '.program-incomplete').touch()
    (host.root / 'config/accounts/one.json').write_text(json.dumps({**json.loads(before), 'retired': 1}))
    calls, result = host.run(second, [])
    assert 'retired: 已移除' in result.stdout, result.stdout
    assert (host.root / 'bin/asterion_trading').read_text().endswith('2"\n')
    assert (host.root / 'runtime/release').read_text() == '2'
    assert not (host.root / 'runtime/obsolete.py').exists()
    assert not (host.root / '.program-incomplete').exists()
    assert (host.root / 'config/accounts/one.json').read_bytes() == before
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls), calls

    host.env['FAIL_RELOAD'] = '1'
    host.run(second, [], ok=False)
    assert (host.root / '.program-incomplete').exists()
    del host.env['FAIL_RELOAD']
    host.run(second, [])
    assert not (host.root / '.program-incomplete').exists()
    assert (host.root / 'config/accounts/one.json').read_bytes() == before

    # Terminal edits (5) of separate running instances retain both timers and restart once on exit.
    third = 'ef' * 32
    calls, _ = host.run(host.root / 'bin/setup.sh', ['1', '5', '1', OTHER, '', '1', third, '', '0', '0', '0'], active=True, now='4 1000')
    assert host.account('one')['terminals'] == ['ab' * 32, OTHER, third]
    assert sum(call.startswith('systemd-run') for call in calls) == 1, calls
    host.run(host.root / 'bin/setup.sh', ['2', '5', '1', third, '', '0', '0', '0'], active=True, now='4 1000')
    assert sorted(p.name for p in host.timers.iterdir()) == [
        'asterion-trading-restart-one', 'asterion-trading-restart-two']
    calls, result = host.run(host.root / 'bin/setup.sh', ['1', '5', '2', '2 3', '2', '1', '', '0', '0', '0'], active=True, now='4 1000')
    assert host.account('one')['terminals'] == ['ab' * 32]
    assert '至少须保留一台' in result.stderr, result.stderr
    assert not any(call.startswith('systemctl stop') and 'restart-two' in call for call in calls)

    # Status panel includes listeners, Chinese status labels and pending deadline.
    _, status = host.run(host.root / 'bin/setup.sh', ['0'], active=True)
    for field in ('账户', '开机自启', '待执行重启', 'one', 'two', '0.0.0.0:7460',
                  '127.0.0.1:7470', '运行中', '已开启', '2026-10-29 15:30:00', fingerprint):
        assert field in status.stdout, (field, status.stdout)

    # Port in use is asked again; Enter everywhere else saves and restarts nothing.
    previous = host.account('two')
    calls, result = host.run(host.root / 'bin/setup.sh', ['2', '2', '5', '', '07460', '7470'] + UNCHANGED + ['0', '0', '0'])
    assert '已被账户 one 占用' in result.stderr
    assert '未作任何修改。' in result.stdout, result.stdout
    assert host.account('two') == previous
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls)

    # A running instance does not defer starting a different, stopped instance.
    calls, _ = host.run(host.root / 'bin/setup.sh', ['2', '5', '2', '3', '0', '0', '0'],
                       now='4 1000', running='asterion-trading@one.service')
    assert 'systemctl restart asterion-trading@two.service' in calls
    assert (host.timers / 'asterion-trading-restart-one').exists()
    host.run(host.root / 'bin/setup.sh', ['2', '5', '1', third, '', '0', '0', '0'], active=True, now='4 1000')

    # Disabling (3) stops the instance and keeps its configuration and record.
    (host.root / 'data/two').mkdir(exist_ok=True)
    calls, _ = host.run(host.root / 'bin/setup.sh', ['2', '3', 'y', '0'])
    assert 'systemctl disable --now asterion-trading@two.service' in calls, calls
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls), calls
    assert not (host.timers / 'asterion-trading-restart-two').exists()
    assert (host.timers / 'asterion-trading-one').exists() is False and (host.timers / 'asterion-trading-restart-one').exists()
    assert (host.root / 'config/accounts/two.json').exists() and (host.root / 'data/two').exists()
    _, status = host.run(host.root / 'bin/setup.sh', ['0'])
    row = next(line.split() for line in status.stdout.splitlines() if ') two ' in line)
    assert row == ['2)', 'two', '127.0.0.1:7470', '未运行', '未开启', '无'], row

    # Stopped account starts again from option 1 without confirmation; running account asks confirmation.
    calls, _ = host.run(host.root / 'bin/setup.sh', ['2', '1', '0', '0'])
    assert 'systemctl enable --now asterion-trading@two.service' in calls, calls
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls), calls
    calls, _ = host.run(host.root / 'bin/setup.sh', ['1', '1', 'n', '0', '0'], active=True, now='4 1000')
    assert not any(call.startswith('systemctl restart') for call in calls), calls
    calls, _ = host.run(host.root / 'bin/setup.sh', ['1', '1', 'y', '0', '0'], active=True, now='4 1000')
    assert 'systemctl restart asterion-trading@one.service' in calls, calls
    assert not (host.timers / 'asterion-trading-restart-one').exists()

    _, result = host.run(host.shortcuts / 'asterion-trading', [], args=['fingerprint'])
    assert result.stdout.splitlines() == ['本机服务端证书指纹（请填入 Terminal）：', '  ' + fingerprint], result.stdout

    # New accounts use script defaults; granular credential & webhook options (3 & 4) encrypt secrets cleanly.
    host.run(host.root / 'bin/setup.sh', ['a', 'three', '', '', '', '', '000003', '', '', 'tcp://127.0.0.1:41213',
        TERMINAL, '', 'third-only-password', 'third-only-auth', '', '0'])
    assert host.account('three') == {'bind': '0.0.0.0', 'port': 7461,
        'terminals': ['ab' * 32], 'environment': 'simulation',
        'market_front': 'tcp://127.0.0.1:41213',
        'broker': {'broker_id': '9999', 'user_id': '000003',
                   'app_id': 'simnow_client_test', 'front': 'tcp://182.254.243.31:40001'}}

    calls, result = host.run(host.root / 'bin/setup.sh', ['1', '2', '3', 'updated-only-password', 'updated-only-auth', '', '0', '0', '0'])
    assert 'CTP 密码与授权码: 已更新' in result.stdout, result.stdout
    assert (host.units / 'credstore/asterion-trading-one/password').read_text() == 'updated-only-password'
    assert 'systemctl restart asterion-trading@one.service' in calls, calls

    secret_hook = 'https://hooks.example.invalid/secret-token-123'
    calls, result = host.run(host.root / 'bin/setup.sh', ['1', '2', '4', 'bad-url', secret_hook, '', '0', '0', '0'])
    assert '脱机告警 Webhook URL: 已设置' in result.stdout, result.stdout
    assert (host.units / 'credstore/asterion.trading.one.webhook').read_text() == secret_hook
    assert secret_hook not in result.stdout + result.stderr + '\n'.join(calls)
    assert secret_hook not in (host.root / 'config/accounts/one.json').read_text()
    calls, result = host.run(host.root / 'bin/setup.sh', ['1', '2', '4', '-', '', '0', '0', '0'])
    assert '脱机告警 Webhook URL: 已清除' in result.stdout, result.stdout
    assert not (host.units / 'credstore/asterion.trading.one.webhook').exists()

    def staged_files():
        return sorted(str(path) for folder in (host.root, host.units)
                      for path in folder.rglob('.*') if path.is_file() and path.name != '.program.lock')
    assert staged_files() == [], staged_files()
    calls, result = host.run(host.root / 'bin/setup.sh', ['a', 'four', '', '', '', '', '000004', '', '',
        'tcp://127.0.0.1:41213', TERMINAL, ''], ok=False)
    assert '凭据输入意外结束' in result.stderr, result.stderr
    assert not (host.root / 'config/accounts/four.json').exists()
    assert not (host.units / 'credstore/asterion-trading-four').exists()
    assert staged_files() == [], staged_files()

    (host.units / 'asterion-trading@.service').write_text('[Service]\n')
    calls, _ = host.run(second, [])
    assert 'LoadCredentialEncrypted=password:' in (host.units / 'asterion-trading@.service').read_text()
    assert 'systemctl daemon-reload' in calls, calls
    assert staged_files() == [], staged_files()

    result = subprocess.run(['unshare', '-r', 'bash', '-c', 'source "$1"; SELF=$1; ROOT=$2; main', 'setup',
                             str(setup), str(host.root)], capture_output=True, text=True, env=host.env, timeout=60)
    assert result.returncode != 0 and 'Asterion-Trading-VERSION-linux-x86_64.sh' in result.stderr, result.stderr
print('installed, upgraded, deferred, added, changed and disabled accounts from a self-extracting script')
