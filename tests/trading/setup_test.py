"""setup.sh installs and upgrades the trading service and adds, changes and
disables account instances. It runs as root in a user namespace against a
temporary root, with stand-ins for the system commands (chown, systemctl,
systemd-run, date) that record what they were asked; openssl is real.

    setup_test.py <apps/services/trading/scripts/setup.sh>
"""
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

setup = Path(sys.argv[1])
if not shutil.which('unshare') or subprocess.run(['unshare', '-r', 'true'], capture_output=True).returncode != 0:
    print('SKIP: unprivileged user namespaces are unavailable')
    sys.exit(77)

# systemctl is-active answers from $ACTIVE; date answers $NOW for the weekday
# and time, and fixed dates otherwise.
STAND_IN = r'''#!/bin/bash
name=$(basename "$0")
{ printf '%s' "$name"; printf ' %s' "$@"; printf '\n'; } >> "$CALLS"
case $name in
    systemctl)
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
    date) case "$*" in *%u*) echo "$NOW" ;; *tomorrow*) echo 2026-10-30 ;; *) echo 2026-10-29 ;; esac ;;
esac
'''
TERMINAL = ':'.join(['AB'] * 32)
OTHER = 'cd' * 32
IDENTITY = dict(environment='simulation', broker=dict(broker_id='9999', user_id='000001', app_id='client_app', front='tcp://127.0.0.1:41205'))


def release(folder, version):
    """A release like asterion-trading-setup.sh whose program only answers --version."""
    top = Path(folder) / f'stage-{version}' / 'asterion-trading'
    for part in ('bin', 'lib'):
        (top / part).mkdir(parents=True)
    (top / 'bin/asterion_trading').write_text(f'#!/bin/sh\necho "Asterion Trading {version}"\n')
    (top / 'bin/asterion_trading').chmod(0o755)
    shutil.copy2(setup, top / 'bin/setup.sh')
    (top / 'lib/ctp-trader.so').write_text(version)
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:gz') as archive:
        archive.add(top, 'asterion-trading')
    script = Path(folder) / f'asterion-trading-setup-{version}.sh'
    script.write_bytes(setup.read_bytes() + b'__ARCHIVE__\n' + payload.getvalue())
    return script


class Host:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.root = self.folder / 'opt/asterion/trading'
        self.units = self.folder / 'units'
        self.units.mkdir()
        fakes = self.folder / 'fakes'
        fakes.mkdir()
        for name in ('chown', 'systemctl', 'systemd-run', 'date', 'systemd-creds'):
            (fakes / name).write_text(STAND_IN)
            (fakes / name).chmod(0o755)
        self.timers = self.folder / 'timers'
        self.timers.mkdir()
        self.boot = self.folder / 'boot'
        self.boot.mkdir()
        self.calls = self.folder / 'calls'
        self.env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(self.calls),
                        SUDO_USER='tester', TIMERS=str(self.timers), BOOT=str(self.boot))

    def run(self, script, answers, active=False, now='4 1700', ok=True, running=None, tail=True):
        self.calls.unlink(missing_ok=True)
        # Bash does not source a file with an archive in it: source the plain
        # script and point it at `script`, as running `script` would.
        command = 'source "$4"; SELF=$1; ROOT=$2; UNITS=$3; CREDENTIALS=$3/credstore; main'
        result = subprocess.run(
            ['unshare', '-r', 'bash', '-c', command, 'setup', str(script), str(self.root),
             str(self.units), str(setup)], input='\n'.join(answers + (['simulation', '9999', '000001', 'client_app', 'tcp://127.0.0.1:41205', 'test-only-password', 'test-only-auth'] if tail else [])) + '\n', capture_output=True,
            text=True, env=dict(self.env, RUNNING=running if running is not None else ('*' if active else ''), NOW=now), timeout=60)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return calls, result

    def account(self, name):
        return json.loads((self.root / f'config/accounts/{name}.json').read_text())


with tempfile.TemporaryDirectory() as folder:
    host = Host(folder)

    # Install: the layout, this host's certificate, the unit template, then the
    # first account (Enter keeps a default, a bad value is asked again).
    calls, result = host.run(release(folder, '1'), ['bad name', 'one', '', '', 'xyz', TERMINAL])
    assert result.stderr.count('Invalid value') == 2, result.stderr
    assert sorted(p.name for p in host.root.iterdir()) == ['bin', 'config', 'data', 'lib', 'logs']
    assert (host.root / 'bin/asterion_trading').read_text().endswith('1"\n')
    assert json.loads((host.root / 'config/logging.json').read_text())['file']['retention_days'] == 30
    assert not (host.root / 'config/trading.json').exists()
    assert host.account('one') == {'bind': '0.0.0.0', 'port': 7460, 'terminals': ['ab' * 32], **IDENTITY}
    assert oct((host.root / 'config/accounts/one.json').stat().st_mode & 0o777) == '0o600'
    assert oct((host.root / 'config/tls/server.key').stat().st_mode & 0o777) == '0o600'
    fingerprint = subprocess.run(
        ['openssl', 'x509', '-noout', '-fingerprint', '-sha256', '-in',
         str(host.root / 'config/tls/server.crt')], capture_output=True, text=True,
        check=True).stdout.split('=', 1)[1].strip()
    assert result.stdout.count(fingerprint) == 2, result.stdout
    unit = (host.units / 'asterion-trading@.service').read_text()
    for line in ('User=tester', 'Restart=always', 'StartLimitIntervalSec=0',
                 f'ExecStart={host.root}/bin/asterion_trading --account %i',
                 f'ReadWritePaths={host.root}/logs {host.root}/data'):
        assert line + '\n' in unit, line
    assert f'LoadCredentialEncrypted=password:{host.units}/credstore/asterion-trading-%i/password' in unit
    assert f'LoadCredentialEncrypted=auth_code:{host.units}/credstore/asterion-trading-%i/auth_code' in unit
    for name, secret in [('password', 'test-only-password'), ('auth_code', 'test-only-auth')]:
        assert (host.units / f'credstore/asterion-trading-one/{name}').read_text() == secret
        assert secret not in result.stdout + result.stderr + '\n'.join(calls)
        assert secret not in (host.root / 'config/accounts/one.json').read_text()
    assert 'systemctl enable asterion-trading@one.service' in calls, calls
    assert calls[-1] == 'systemctl restart asterion-trading@one.service', calls

    # The installed copy adds a second account on the next free port.
    calls, result = host.run(host.root / 'bin/setup.sh', ['1', 'two', '127.0.0.1', '', f'{OTHER} {TERMINAL}', 'simulation', '9999', '000001', 'client_app', 'tcp://127.0.0.1:41205', 'second-only-password', 'second-only-auth'])
    assert (host.units / 'credstore/asterion-trading-two/password').read_text() == 'second-only-password'
    assert (host.units / 'credstore/asterion-trading-one/password').read_text() == 'test-only-password'
    assert 'second-only-password' not in result.stdout + result.stderr + '\n'.join(calls)
    assert host.account('two') == {'bind': '127.0.0.1', 'port': 7461, 'terminals': [OTHER, 'ab' * 32], **IDENTITY}
    assert calls[-1] == 'systemctl restart asterion-trading@two.service', calls
    # Changing it offers its current values.
    host.run(host.root / 'bin/setup.sh', ['1', 'two', '', '7470', ''])
    assert host.account('two') == {'bind': '127.0.0.1', 'port': 7470, 'terminals': [OTHER, 'ab' * 32], **IDENTITY}

    # Configuring another counter must never replace a recorded account's identity.
    journal = host.root / 'data/two/journal.sqlite'
    journal.parent.mkdir(parents=True, exist_ok=True)
    journal.touch()
    previous = host.account('two')
    host.run(host.root / 'bin/setup.sh', ['1', 'two', '', '', '', 'simulation', '9999', '000009', 'client_app', 'tcp://127.0.0.1:41205'], ok=False)
    assert host.account('two') == previous

    # Upgrade: new programs, configuration untouched, running instances restarted;
    # during trading hours the restart waits for the end of the window.
    second = release(folder, '2')
    before = (host.root / 'config/accounts/one.json').read_bytes()
    calls, _ = host.run(second, [], active=True)
    assert (host.root / 'bin/asterion_trading').read_text().endswith('2"\n')
    assert (host.root / 'config/accounts/one.json').read_bytes() == before
    assert [call for call in calls if call.startswith('systemctl restart')] == [
        'systemctl restart asterion-trading@one.service', 'systemctl restart asterion-trading@two.service'], calls
    for now, at in (('4 1000', '2026-10-29 15:30'), ('4 2200', '2026-10-30 02:45')):
        calls, result = host.run(second, [], active=True, now=now)
        assert not any(call.startswith('systemctl restart') for call in calls), (now, calls)
        assert [call for call in calls if call.startswith('systemd-run')] == [
            f'systemd-run --quiet --collect --unit=asterion-trading-restart-{name} '
            f'--on-calendar={at}:00 Asia/Shanghai systemctl restart '
            f'asterion-trading@{name}.service' for name in ('one', 'two')], (now, calls)
        assert at in result.stdout
    # Nothing running: nothing to restart.
    calls, _ = host.run(second, [], active=False)
    assert not any(call.startswith('systemctl restart') for call in calls), calls

    # Repeated edits of separate running instances must retain both timers.
    host.run(host.root / 'bin/setup.sh', ['1', 'one', '', '', ''], active=True, now='4 1000')
    host.run(host.root / 'bin/setup.sh', ['1', 'two', '', '', ''], active=True, now='4 1000')
    assert sorted(p.name for p in host.timers.iterdir()) == [
        'asterion-trading-restart-one', 'asterion-trading-restart-two']
    # Re-editing one only replaces its own pending restart.
    calls, _ = host.run(host.root / 'bin/setup.sh', ['1', 'one', '', '', ''], active=True, now='4 1000')
    assert not any(call.startswith('systemctl stop') and 'restart-two' in call for call in calls)
    # Status includes both listeners, active state and the pending deadline.
    _, status = host.run(host.root / 'bin/setup.sh', ['4'], active=True)
    for field in ('ACCOUNT', 'AUTOSTART', 'RESTART', 'one', 'two', '0.0.0.0:7460',
                  '127.0.0.1:7470', 'active', 'enabled', '2026-10-29 15:30:00'):
        assert field in status.stdout, status.stdout
    # A port conflict fails before replacing configuration or scheduling work.
    previous = host.account('two')
    calls, result = host.run(host.root / 'bin/setup.sh', ['1', 'two', '', '07460'], ok=False)
    assert 'already configured for account one' in result.stderr
    assert host.account('two') == previous
    assert not any(call.startswith(('systemctl restart', 'systemd-run')) for call in calls)
    # A running instance does not defer starting a different, stopped instance.
    calls, _ = host.run(second, [], now='4 1000', running='asterion-trading@one.service')
    assert len([call for call in calls if call.startswith('systemd-run')]) == 1
    assert not any(call.startswith('systemctl restart') for call in calls)
    calls, _ = host.run(host.root / 'bin/setup.sh', ['1', 'two', '', '', ''],
                       now='4 1000', running='asterion-trading@one.service')
    assert 'systemctl restart asterion-trading@two.service' in calls
    assert (host.timers / 'asterion-trading-restart-one').exists()
    # Recreate two's timer, then disable it: it must not start again at the close.
    host.run(host.root / 'bin/setup.sh', ['1', 'two', '', '', ''], active=True, now='4 1000')

    # Disabling stops the instance and keeps its configuration and record.
    (host.root / 'data/two').mkdir(exist_ok=True)
    calls, _ = host.run(host.root / 'bin/setup.sh', ['2', 'two', 'y'])
    assert 'systemctl disable --now asterion-trading@two.service' in calls, calls
    assert not (host.timers / 'asterion-trading-restart-two').exists()
    assert (host.timers / 'asterion-trading-restart-one').exists()
    assert (host.root / 'config/accounts/two.json').exists() and (host.root / 'data/two').exists()
    _, status = host.run(host.root / 'bin/setup.sh', ['4'])
    row = next(line.split() for line in status.stdout.splitlines() if line.startswith('two '))
    assert row == ['two', '127.0.0.1:7470', 'inactive', 'disabled', 'none'], row
    _, result = host.run(host.root / 'bin/setup.sh', ['3'])
    assert fingerprint in result.stdout

    # New accounts use script defaults without any installed default configuration file.
    host.run(host.root / 'bin/setup.sh', ['1', 'three', '', '', TERMINAL,
        '', '', '000003', '', '', 'third-only-password', 'third-only-auth'])
    assert host.account('three') == {'bind': '0.0.0.0', 'port': 7461,
        'terminals': ['ab' * 32], 'environment': 'simulation',
        'broker': {'broker_id': '9999', 'user_id': '000003',
                   'app_id': 'simnow_client_test', 'front': 'tcp://182.254.243.31:40001'}}
    # Enter on every existing-account field keeps its own values, not the new defaults.
    previous = host.account('one')
    host.run(host.root / 'bin/setup.sh', ['1', 'one'] + [''] * 8 +
        ['updated-only-password', 'updated-only-auth'])
    assert host.account('one') == previous

    # Nothing a run stages is left behind, however it ends.
    def staged_files():
        return sorted(str(path) for folder in (host.root, host.units)
                      for path in folder.rglob('.*') if path.is_file())
    assert staged_files() == [], staged_files()
    # The password may stay when the counter account does not change.
    saved = (host.units / 'credstore/asterion-trading-one/password').read_text()
    host.run(host.root / 'bin/setup.sh', ['1', 'one'] + [''] * 8 + ['y'], tail=False)
    assert (host.units / 'credstore/asterion-trading-one/password').read_text() == saved
    # Input that ends before the credentials writes nothing.
    calls, result = host.run(host.root / 'bin/setup.sh', ['1', 'four', '', '', TERMINAL,
        '', '', '000004', '', ''], tail=False, ok=False)
    assert 'Credential input ended' in result.stderr, result.stderr
    assert not (host.root / 'config/accounts/four.json').exists()
    assert not (host.units / 'credstore/asterion-trading-four').exists()
    assert staged_files() == [], staged_files()
    # An upgrade writes the unit template of its release again.
    (host.units / 'asterion-trading@.service').write_text('[Service]\n')
    calls, _ = host.run(second, [])
    assert 'LoadCredentialEncrypted=password:' in (host.units / 'asterion-trading@.service').read_text()
    assert 'systemctl daemon-reload' in calls, calls
    assert staged_files() == [], staged_files()

    # The plain script elsewhere carries no programs.
    result = subprocess.run(['unshare', '-r', 'bash', '-c', 'source "$1"; SELF=$1; ROOT=$2; main', 'setup',
                             str(setup), str(host.root)], capture_output=True, text=True, env=host.env, timeout=60)
    assert result.returncode != 0 and 'asterion-trading-setup.sh' in result.stderr, result.stderr
print('installed, upgraded, deferred, added, changed and disabled accounts from a self-extracting script')
