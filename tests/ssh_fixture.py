#!/usr/bin/env python3
"""Test-only SSH/SFTP double; never registers host OS services or connects remotely."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
root = Path(os.environ['ASTERION_SSH_FIXTURE'])
args = sys.argv[1:]
assert 'StrictHostKeyChecking=yes' in args and 'BatchMode=yes' in args
assert 'GlobalKnownHostsFile=none' in args and 'ForwardAgent=no' in args
assert 'IdentityAgent=none' in args and 'IdentitiesOnly=yes' in args
assert 'PreferredAuthentications=publickey' in args and 'PasswordAuthentication=no' in args
identity=Path(args[args.index('-i')+1])
assert identity.stat().st_mode & 0o777 == 0o600
assert 'PRIVATE KEY' in identity.read_text()
with (root/'identity-paths').open('a') as out:
    out.write(str(identity)+'\n')
assert not any('PRIVATE KEY' in arg for arg in args)
if (root/'reject').exists():
    sys.exit(255)
if Path(sys.argv[0]).name == 'sftp':
    for line in Path(args[args.index('-b')+1]).read_text().splitlines():
        operation, source, target = shlex.split(line)
        assert operation == 'put' and re.fullmatch(r'\.asterion-install-[a-f0-9]{32}/[a-z.]+',target)
        shutil.copyfile(source,root/target)
    sys.exit(0)
command=args[-1]
if command.startswith("sh -c "):
    script=shlex.split(command)[2]
    if 'uname -m' in script and '"arch"' in script:
        import platform
        target_os=(root/'target-os').read_text() if (root/'target-os').exists() else 'linux'
        arch={'aarch64':'arm64','arm64':'arm64','x86_64':'x86_64'}.get(platform.machine(),'x86_64')
        print(json.dumps(dict(os=target_os,arch=arch)))
    elif '"backend"' in script or '\"backend\"' in script:
        state='permission_required' if (root/'no-firewall-permission').exists() else 'active'
        print(json.dumps(dict(source='192.0.2.10',backend='ufw',state=state,os='linux')))
    elif 'ufw' in script:
        with (root/'firewall-changes').open('a') as out: out.write(script+'\n')
        print(json.dumps(dict(changed=True)))
    else: raise AssertionError('unexpected firewall script')
    sys.exit(0)
if command.startswith('test "$(uname -s)"'):
    assert 'id -u' in command
elif command.startswith('umask 077; mkdir '):
    stage=shlex.split(command)[-1]
    assert re.fullmatch(r'\.asterion-install-[a-f0-9]{32}',stage)
    (root/stage).mkdir(mode=0o700)
elif command.startswith('if test ') or command.startswith("sh '.asterion-install-"):
    install=root/re.search(r"'(\.asterion-install-[a-f0-9]{32}/install)'",command)[1]
    stage=install.parent
    script=install.read_text(); service=(stage/'service').read_text() if (stage/'service').exists() else ''
    digest=hashlib.sha256((stage/'agent').read_bytes()).hexdigest()
    assert digest in script and 'cmp' in script and 'chmod 600' in script
    assert 'asterion-host --manage install' in script or 'launchctl bootstrap system' in script
    if not service:
        assert command.startswith('sh ') and 'sudo -n sh' not in command
        assert 'systemctl' not in script
    if (root/'fail_install').exists():
        sys.exit(1)
    port=int(re.search(r'(?:--port</string><string>|--port )(\d+)',service)[1]) if service else int(re.search(r"--manage install '[^']+' (\d+)",script)[1])
    deployed=root/'deployed'; deployed.mkdir()
    for name in ('agent','ca.crt','server.crt','server.key'):
        shutil.copyfile(stage/name,deployed/name)
    (deployed/'state').mkdir(); (deployed/'agent').chmod(0o700)
    process=subprocess.Popen([str(deployed/'agent'),'--bind','127.0.0.1','--port',str(port),'--directory',str(deployed/'state'),'--tls-ca',str(deployed/'ca.crt'),'--tls-cert',str(deployed/'server.crt'),'--tls-key',str(deployed/'server.key')],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
    (root/'pid').write_text(str(process.pid))
elif command.startswith('rm -rf -- '):
    stage=shlex.split(command)[-1]; assert re.fullmatch(r'\.asterion-install-[a-f0-9]{32}',stage)
    shutil.rmtree(root/stage,ignore_errors=True)
else:
    raise AssertionError('unexpected command')
