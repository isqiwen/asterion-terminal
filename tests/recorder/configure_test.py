"""configure.sh asks for the recorder's settings with the current values as
defaults and the secrets without, writes recorder.json and hands the secrets
to systemd-creds only. It runs as root in a user namespace, with stand-ins for
systemctl, systemd-creds and chown that record what they were asked.

    configure_test.py <scripts/recorder> <config/recorder.json>
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

with tempfile.TemporaryDirectory() as folder:
    root = Path(folder) / 'asterion-recorder'
    (root / 'bin').mkdir(parents=True)
    (root / 'config').mkdir()
    shutil.copy2(scripts / 'configure.sh', root / 'bin/configure.sh')
    shutil.copy2(scripts / 'recording-host.py', root / 'bin/recording-host.py')
    shutil.copy2(default, root / 'config/recorder.json')
    fakes = Path(folder) / 'fakes'
    fakes.mkdir()
    for name in ('systemctl', 'systemd-creds', 'chown'):
        (fakes / name).write_text(STAND_IN)
        (fakes / name).chmod(0o755)
    calls, secrets = Path(folder) / 'calls', Path(folder) / 'secrets'
    env = dict(os.environ, PATH=f'{fakes}:{os.environ["PATH"]}', CALLS=str(calls), SECRETS=str(secrets))
    # Enter keeps a default; a bad port is asked again.
    answers = '\n'.join(['', 'abc', '7451', '', '', '', '273005', '', 'fixture-password', 'fixture-auth']) + '\n'
    result = subprocess.run(['unshare', '-r', 'bash', str(root / 'bin/configure.sh')], input=answers,
                            capture_output=True, text=True, env=env, timeout=60)
    assert result.returncode == 0, result.stderr
    assert '格式不对' in result.stderr, result.stderr

    settings = json.loads((root / 'config/recorder.json').read_text())
    shipped = json.loads(default.read_text())
    assert settings == {'bind': shipped['bind'], 'port': 7451,
                        'counter': {**shipped['counter'], 'user': '273005'}}, settings
    text = (root / 'config/recorder.json').read_text()
    assert 'fixture-password' not in text and 'fixture-auth' not in text
    assert oct((root / 'config/recorder.json').stat().st_mode & 0o777) == '0o640'

    assert secrets.read_text().splitlines() == [
        '--name=asterion.ctp.password=fixture-password', '--name=asterion.ctp.auth_code=fixture-auth']
    lines = calls.read_text().splitlines()
    assert any(line.startswith('chown root:asterion-recorder ') for line in lines), lines
    assert lines[-1] == 'systemctl restart asterion-recorder.service', lines
    assert 'fixture-password' not in result.stdout + result.stderr
    print('configured', settings)
