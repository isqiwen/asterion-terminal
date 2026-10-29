"""setup.sh installs, upgrades and configures the recorder. It runs as root in a
user namespace against a temporary root, with stand-ins for the system
commands (chown, systemctl, systemd-creds, systemd-run, date) that
record what they were asked.

    setup_test.py <apps/services/recorder/scripts/setup.sh> <config/recorder.json>
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

setup, default = Path(sys.argv[1]), Path(sys.argv[2])
if subprocess.run(['unshare', '-r', 'true'], capture_output=True).returncode != 0:
    print('SKIP: unprivileged user namespaces are unavailable')
    sys.exit(77)

# systemctl is-active answers from $ACTIVE; date answers $NOW for the weekday
# and time, and fixed dates otherwise.
STAND_IN = r'''#!/bin/bash
name=$(basename "$0")
{ printf '%s' "$name"; printf ' %s' "$@"; printf '\n'; } >> "$CALLS"
case $name in
    systemctl) [[ $1 != is-active ]] || exit "${ACTIVE:-3}" ;;
    systemd-creds) printf '%s=' "$2" >> "$SECRETS"; cat >> "$SECRETS"; echo >> "$SECRETS" ;;
    date) case "$*" in *%u*) echo "$NOW" ;; *tomorrow*) echo 2026-10-30 ;; *) echo 2026-10-29 ;; esac ;;
esac
'''


def release(folder, version):
    """A release script like asterion-recorder-setup.sh: setup.sh, the line
    __ARCHIVE__, then the installation as a tar.gz. Its recorder only answers
    --version."""
    top = Path(folder) / f'stage-{version}' / 'asterion-recorder'
    for part in ('bin', 'lib', 'config'):
        (top / part).mkdir(parents=True)
    (top / 'bin/asterion_recorder').write_text(f'#!/bin/sh\necho "Asterion Recorder {version}"\n')
    (top / 'bin/asterion_recorder').chmod(0o755)
    shutil.copy2(setup, top / 'bin/setup.sh')
    for library in ('ctp-md.so', 'ctp-trader.so'):
        (top / 'lib' / library).write_text(version)
    shutil.copy2(default, top / 'config/recorder.json')
    shutil.copy2(default.parent / 'recorder-logging.json', top / 'config/recorder-logging.json')
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:gz') as archive:
        archive.add(top, 'asterion-recorder')
    script = Path(folder) / f'asterion-recorder-setup-{version}.sh'
    script.write_bytes(setup.read_bytes() + b'__ARCHIVE__\n' + payload.getvalue())
    return script


class Host:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.root = self.folder / 'opt/asterion-recorder'
        self.units = self.folder / 'units'
        self.credentials = self.folder / 'credstore'
        self.units.mkdir()
        fakes = self.folder / 'fakes'
        fakes.mkdir()
        for name in ('chown', 'systemctl', 'systemd-creds', 'systemd-run', 'date'):
            (fakes / name).write_text(STAND_IN)
            (fakes / name).chmod(0o755)
        self.calls, self.secrets = self.folder / 'calls', self.folder / 'secrets'
        self.env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(self.calls),
                        SECRETS=str(self.secrets), SUDO_USER='tester')

    def run(self, script, answers, active=False, now='4 1700'):
        """Runs `script`'s main with the host's paths in place of the real ones."""
        for record in (self.calls, self.secrets):
            record.unlink(missing_ok=True)
        # Bash does not source a file with an archive in it: source the plain
        # script and point it at `script`, as running `script` would.
        command = ('source "$5"; SELF=$1; ROOT=$2; UNITS=$3; CREDENTIALS=$4; main')
        env = dict(self.env, ACTIVE='0' if active else '3', NOW=now)
        result = subprocess.run(
            ['unshare', '-r', 'bash', '-c', command, 'setup', str(script), str(self.root), str(self.units),
             str(self.credentials), str(setup)], input='\n'.join(answers) + '\n', capture_output=True, text=True,
            env=env, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return calls, result

    def settings(self):
        return json.loads((self.root / 'config/recorder.json').read_text())


shipped = json.loads(default.read_text())
with tempfile.TemporaryDirectory() as folder:
    host = Host(folder)

    # Install: the layout, the unit, then the settings (Enter keeps a default,
    # a bad port is asked again), then the first start.
    calls, result = host.run(release(folder, '1'),
                             ['', 'abc', '7451', '', '', '', '273005', '', 'fixture-password', 'fixture-auth'])
    assert 'Invalid value' in result.stderr, result.stderr
    assert sorted(p.name for p in host.root.iterdir()) == ['bin', 'config', 'data', 'lib', 'logs']
    assert (host.root / 'bin/asterion_recorder').read_text().endswith('1"\n')
    assert (host.root / 'bin/setup.sh').read_bytes() == setup.read_bytes()
    assert (host.root / 'config/recorder-logging.json').read_bytes() == (
        default.parent / 'recorder-logging.json').read_bytes()
    assert host.settings() == {'bind': shipped['bind'], 'port': 7451,
                               'counter': {**shipped['counter'], 'user': '273005'}}
    text = (host.root / 'config/recorder.json').read_text()
    assert 'fixture-password' not in text and 'fixture-auth' not in text
    assert oct((host.root / 'config/recorder.json').stat().st_mode & 0o777) == '0o600'
    assert host.secrets.read_text().splitlines() == [
        '--name=asterion.ctp.password=fixture-password', '--name=asterion.ctp.auth_code=fixture-auth']
    unit = (host.units / 'asterion-recorder.service').read_text()
    for line in ('User=tester', 'Restart=always', 'StartLimitIntervalSec=0',
                 'ImportCredential=asterion.ctp.*', f'ExecStart={host.root}/bin/asterion_recorder',
                 f'ReadWritePaths={host.root}/logs {host.root}/data'):
        assert line + '\n' in unit, line
    assert any(call.startswith('chown tester: ') for call in calls), calls
    assert 'systemctl enable asterion-recorder.service' in calls, calls
    assert calls[-1] == 'systemctl restart asterion-recorder.service', calls
    assert 'fixture-password' not in result.stdout + result.stderr

    # Upgrade: new programs, settings untouched when not asked to change them.
    (host.root / 'config/recorder.json').write_text(json.dumps({**host.settings(), 'port': 7999}))
    (host.root / 'config/recorder-logging.json').write_text('{"level": "debug", "retention_days": 7}')
    before = (host.root / 'config/recorder.json').read_text()
    second = release(folder, '2')
    calls, result = host.run(second, ['n'])
    assert (host.root / 'bin/asterion_recorder').read_text().endswith('2"\n')
    assert (host.root / 'lib/ctp-md.so').read_text() == '2'
    assert (host.root / 'config/recorder.json').read_text() == before
    assert json.loads((host.root / 'config/recorder-logging.json').read_text())['level'] == 'debug'
    assert not host.secrets.exists()
    assert calls[-1] == 'systemctl restart asterion-recorder.service', calls

    # Recording during trading hours: the upgrade is placed, the restart waits.
    for now, at in (('4 1000', '2026-10-29 15:30'), ('4 2200', '2026-10-30 02:45'),
                    ('5 0100', '2026-10-29 02:45')):
        calls, result = host.run(second, ['n'], active=True, now=now)
        assert 'systemctl restart asterion-recorder.service' not in calls, (now, calls)
        assert calls[-1] == ('systemd-run --quiet --unit=asterion-recorder-restart '
                             f'--on-calendar={at}:00 Asia/Shanghai systemctl restart '
                             'asterion-recorder.service'), (now, calls)
        assert at in result.stdout, result.stdout
    # Outside trading hours, and at a weekend, it restarts at once.
    for now in ('4 1530', '4 0245', '6 1000', '1 0100'):
        calls, _ = host.run(second, ['n'], active=True, now=now)
        assert calls[-1] == 'systemctl restart asterion-recorder.service', (now, calls)

    # The installed copy only changes the settings.
    calls, result = host.run(host.root / 'bin/setup.sh',
                             ['', '', '', '', '', '', '-', 'fixture-password'])
    assert host.settings()['port'] == 7999 and host.settings()['counter']['app_id'] == ''
    assert host.secrets.read_text().splitlines() == ['--name=asterion.ctp.password=fixture-password']
    assert 'systemctl enable asterion-recorder.service' not in calls, calls
    assert calls[-1] == 'systemctl restart asterion-recorder.service', calls

    # The plain script elsewhere carries no programs.
    result = subprocess.run(['unshare', '-r', 'bash', '-c', 'source "$1"; SELF=$1; ROOT=$2; main', 'setup',
                             str(setup), str(host.root)], capture_output=True, text=True, env=host.env, timeout=60)
    assert result.returncode != 0 and 'asterion-recorder-setup.sh' in result.stderr, result.stderr
print('installed, upgraded, deferred and configured from a self-extracting script')
