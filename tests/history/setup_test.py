"""setup.sh installs, upgrades and configures the market. It runs as root in a
user namespace against a temporary root, with stand-ins for the system
commands (chown, systemctl, systemd-creds, systemd-run, date) that
record what they were asked.

    setup_test.py <apps/services/history/scripts/setup.sh>
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
namespace = [] if os.geteuid() == 0 else ['unshare', '-r']
if namespace and subprocess.run(namespace + ['true'], capture_output=True).returncode != 0:
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
    """A release script like Asterion-History-VERSION-linux-x86_64.sh: setup.sh, the line
    __ARCHIVE__, then the installation as a tar.gz. Its market only answers
    --version."""
    top = Path(folder) / f'stage-{version}' / 'asterion-history'
    (top / 'bin').mkdir(parents=True)
    (top / 'bin/asterion_history').write_text(f'#!/bin/sh\necho "Asterion History {version}"\n')
    (top / 'bin/asterion_history').chmod(0o755)
    shutil.copy2(setup, top / 'bin/setup.sh')
    (top / 'bin/asterion_crash_reporter').write_text('#!/bin/sh\nexit 0\n')
    (top / 'bin/asterion_crash_reporter').chmod(0o755)
    (top / 'config').mkdir()
    shutil.copy2(setup.parent.parent / 'config/logging.json', top / 'config/logging.json')
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w:gz') as archive:
        archive.add(top, 'asterion-history')
    script = Path(folder) / f'Asterion-History-{version}-linux-x86_64.sh'
    script.write_bytes(setup.read_bytes() + b'__ARCHIVE__\n' + payload.getvalue())
    return script


class Host:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.root = self.folder / 'opt/asterion-history'
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
            namespace + ['bash', '-c', command, 'setup', str(script), str(self.root), str(self.units),
             str(self.credentials), str(setup)], input='\n'.join(answers) + '\n', capture_output=True, text=True,
            env=env, timeout=60)
        assert result.returncode == 0, result.stdout + result.stderr
        calls = self.calls.read_text().splitlines() if self.calls.exists() else []
        return calls, result

    def settings(self):
        return json.loads((self.root / 'config/history.json').read_text())


client_pin = 'a' * 64
shipped = {'bind': '0.0.0.0', 'port': 7450,
           'clients': [dict(fingerprint=client_pin, role='manage')],
           'acquisition': {'exchanges': ['SHFE', 'INE', 'CZCE', 'DCE', 'CFFEX', 'GFEX'], 'tushare': {'requests_per_minute': 100}}}
with tempfile.TemporaryDirectory() as folder:
    host = Host(folder)

    # Install: the layout, the unit, then the settings (Enter keeps a default,
    # a bad port is asked again), then the first start.
    calls, result = host.run(release(folder, '1'),
                             ['', 'abc', '7451', '', client_pin + '=manage', '', '100', 'fixture-token'])
    assert 'Invalid value' in result.stderr, result.stderr
    assert sorted(p.name for p in host.root.iterdir()) == ['bin', 'config', 'data', 'logs']
    assert (host.root / 'bin/asterion_history').read_text().endswith('1"\n')
    assert (host.root / 'bin/setup.sh').read_bytes() == setup.read_bytes()
    assert (host.root / 'config/logging.json').read_bytes() == (setup.parent.parent / 'config/logging.json').read_bytes()
    assert host.settings() == {**shipped, 'port': 7451, 'data': str(host.root / 'data'),
                               }
    text = (host.root / 'config/history.json').read_text()
    assert 'fixture-token' not in text
    assert oct((host.root / 'config/history.json').stat().st_mode & 0o777) == '0o600'
    assert host.secrets.read_text().splitlines() == [
        '--name=asterion.history.tushare.token=fixture-token']
    unit = (host.units / 'asterion-history.service').read_text()
    for line in ('User=tester', 'Restart=always', 'StartLimitIntervalSec=0',
                 'ImportCredential=asterion.history.*', f'ExecStart={host.root}/bin/asterion_history',
                 f'ReadWritePaths={host.root}/logs {host.root}/data'):
        assert line + '\n' in unit, line
    assert any(call.startswith('chown tester: ') for call in calls), calls
    assert 'systemctl enable asterion-history.service' in calls, calls
    assert calls[-1] == 'systemctl restart asterion-history.service', calls
    assert 'fixture-token' not in result.stdout + result.stderr
    assert (host.root / 'config/tls/server.key').stat().st_mode & 0o777 == 0o600

    # Upgrade: new programs, settings untouched when not asked to change them.
    (host.root / 'config/history.json').write_text(json.dumps({**host.settings(), 'port': 7999}))
    (host.root / 'config/logging.json').write_text('{"level": "debug", "retention_days": 7}')
    before = (host.root / 'config/history.json').read_text()
    # Service-owned records and TLS identities survive upgrades byte for byte.
    preserved = {
        'data/sources/sina/warehouse/current': b'published-version',
        'data/sources/sina/warehouse/versions/published-version.json': b'fixture-manifest',
        'data/acquisition/published.pb': b'fixture-task',
        'logs/market.log': b'fixture-log',
    }
    for name, content in preserved.items():
        path = host.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    preserved.update({str(path.relative_to(host.root)): path.read_bytes()
                      for path in (host.root / 'config/tls').iterdir()})
    second = release(folder, '2')
    calls, result = host.run(second, ['n'])
    assert (host.root / 'bin/asterion_history').read_text().endswith('2"\n')
    assert (host.root / 'config/history.json').read_text() == before
    assert json.loads((host.root / 'config/logging.json').read_text())['level'] == 'debug'
    assert not host.secrets.exists()
    assert calls[-1] == 'systemctl restart asterion-history.service', calls
    assert all((host.root / name).read_bytes() == content for name, content in preserved.items())

    # The installed copy only changes the settings. Another data directory is
    # made for the market and becomes writable; the old data is not moved.
    disk = host.folder / 'disk/market-data'
    calls, result = host.run(host.root / 'bin/setup.sh',
                             ['', '', str(disk), '', '', '0'])
    assert host.settings()['port'] == 7999
    assert host.settings()['data'] == str(disk)
    assert disk.is_dir() and any(call.startswith('chown ') and call.endswith(f': {disk}')
                                 for call in calls), calls
    unit = (host.units / 'asterion-history.service').read_text()
    assert f'ReadWritePaths={host.root}/logs {disk}\n' in unit, unit
    assert f'The existing data stays in {host.root}/data' in result.stdout, result.stdout
    assert (host.root / 'data/acquisition/published.pb').read_bytes() == b'fixture-task'
    assert not host.secrets.exists()
    assert 'systemctl enable asterion-history.service' not in calls, calls
    assert calls[-1] == 'systemctl restart asterion-history.service', calls

    # Nothing a run stages is left behind, and an upgrade writes its unit again.
    def staged_files():
        return sorted(str(path) for folder in (host.root, host.units, host.credentials)
                      for path in folder.rglob('.*') if path.is_file())
    assert staged_files() == [], staged_files()
    (host.units / 'asterion-history.service').write_text('[Service]\n')
    calls, _ = host.run(second, ['n'])
    assert 'ImportCredential=asterion.history.*' in (host.units / 'asterion-history.service').read_text()
    assert 'systemctl daemon-reload' in calls, calls
    # Input that ends early writes no settings.
    before = (host.root / 'config/history.json').read_bytes()
    result = subprocess.run(
        namespace + ['bash', '-c', 'source "$5"; SELF=$1; ROOT=$2; UNITS=$3; CREDENTIALS=$4; main',
         'setup', str(host.root / 'bin/setup.sh'), str(host.root), str(host.units), str(host.credentials),
         str(setup)], input='\n' * 3, capture_output=True, text=True, env=dict(host.env, ACTIVE='3', NOW='4 1700'),
        timeout=60)
    assert result.returncode != 0, result.stdout + result.stderr
    assert (host.root / 'config/history.json').read_bytes() == before
    assert staged_files() == [], staged_files()

    # The plain script elsewhere carries no programs.
    result = subprocess.run(namespace + ['bash', '-c', 'source "$1"; SELF=$1; ROOT=$2; main', 'setup',
                             str(setup), str(host.root)], capture_output=True, text=True, env=host.env, timeout=60)
    assert result.returncode != 0 and 'Asterion-History-VERSION-linux-x86_64.sh' in result.stderr, result.stderr
print('installed, upgraded and configured from a self-extracting script')
