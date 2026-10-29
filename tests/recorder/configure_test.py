"""configure.sh asks for the recorder's settings with the current values as
defaults and the secrets without, writes recorder.json and hands the secrets
to systemd-creds only. It runs as root in a user namespace, with stand-ins for
systemctl, systemd-creds and chown that record what they were asked.

    configure_test.py <apps/services/recorder/scripts> <config/recorder.json>
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

scripts, default = Path(sys.argv[1]), Path(sys.argv[2])
if subprocess.run(['unshare', '-r', 'true'], capture_output=True).returncode != 0:
    print('SKIP: unprivileged user namespaces are unavailable')
    sys.exit(77)

STAND_IN = '''#!/bin/bash
{ printf '%s' "$(basename "$0")"; printf ' %s' "$@"; printf '\\n'; } >> "$CALLS"
if [[ $(basename "$0") == systemd-creds ]]; then printf '%s=' "$2" >> "$SECRETS"; cat >> "$SECRETS"; echo >> "$SECRETS"; fi
'''



def configure(folder, answers, restart_at=None):
    """Runs configure.sh in a fresh installation; returns it, the calls and the result.
    With `restart_at`, recording-host.py is a stand-in saying the recorder is
    recording during trading hours until then."""
    root = Path(folder) / 'asterion-recorder'
    (root / 'bin').mkdir(parents=True)
    (root / 'config').mkdir()
    shutil.copy2(scripts / 'configure.sh', root / 'bin/configure.sh')
    if restart_at:
        (root / 'bin/recording-host.py').write_text(f'print("{restart_at}")\n')
    else:
        shutil.copy2(scripts / 'recording-host.py', root / 'bin/recording-host.py')
    shutil.copy2(default, root / 'config/recorder.json')
    fakes = Path(folder) / 'fakes'
    fakes.mkdir()
    for name in ('systemctl', 'systemd-creds', 'systemd-run', 'chown'):
        (fakes / name).write_text(STAND_IN)
        (fakes / name).chmod(0o755)
    calls, secrets = Path(folder) / 'calls', Path(folder) / 'secrets'
    env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(calls), SECRETS=str(secrets))
    result = subprocess.run(['unshare', '-r', 'bash', str(root / 'bin/configure.sh')],
                            input='\n'.join(answers) + '\n', capture_output=True, text=True, env=env, timeout=60)
    assert result.returncode == 0, result.stderr
    return root, calls.read_text().splitlines(), secrets, result


shipped = json.loads(default.read_text())
with tempfile.TemporaryDirectory() as folder:
    # Enter keeps a default; a bad port is asked again.
    root, calls, secrets, result = configure(
        folder, ['', 'abc', '7451', '', '', '', '273005', '', 'fixture-password', 'fixture-auth'])
    assert '格式不对' in result.stderr, result.stderr
    settings = json.loads((root / 'config/recorder.json').read_text())
    assert settings == {'bind': shipped['bind'], 'port': 7451,
                        'counter': {**shipped['counter'], 'user': '273005'}}, settings
    text = (root / 'config/recorder.json').read_text()
    assert 'fixture-password' not in text and 'fixture-auth' not in text
    assert oct((root / 'config/recorder.json').stat().st_mode & 0o777) == '0o640'
    assert secrets.read_text().splitlines() == [
        '--name=asterion.ctp.password=fixture-password', '--name=asterion.ctp.auth_code=fixture-auth']
    assert any(line.startswith('chown root:asterion-recorder ') for line in calls), calls
    assert calls[-1] == 'systemctl restart asterion-recorder.service', calls
    assert 'fixture-password' not in result.stdout + result.stderr

with tempfile.TemporaryDirectory() as folder:
    # Recording during trading hours: saved now, restarted when the window ends.
    root, calls, _, result = configure(
        folder, ['', '', '', '', '', '273005', '-', 'fixture-password'], restart_at='2026-10-29 15:30')
    assert '2026-10-29 15:30' in result.stdout, result.stdout
    assert json.loads((root / 'config/recorder.json').read_text())['counter']['app_id'] == ''
    assert 'systemctl restart asterion-recorder.service' not in calls, calls
    assert calls[-1] == ('systemd-run --quiet --unit=asterion-recorder-restart '
                         '--on-calendar=2026-10-29 15:30:00 Asia/Shanghai systemctl restart '
                         'asterion-recorder.service'), calls
print('configured now and after trading hours')
