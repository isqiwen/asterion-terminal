"""Real loopback OpenSSH host verification; no privileged installation is performed."""
import os
from pathlib import Path
import pwd
import socket
import subprocess
import sys
import tempfile
import time
if os.name == 'nt': raise SystemExit('POSIX sshd fixture')
with tempfile.TemporaryDirectory(prefix='asterion-sshd-') as folder:
    root=Path(folder)
    for name in ('host','client','other'):
        subprocess.run(['/usr/bin/ssh-keygen','-q','-t','ed25519','-N','','-f',str(root/name)],check=True)
    with socket.socket() as s: s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
    config=root/'sshd.conf'
    config.write_text(f'Port {port}\nListenAddress 127.0.0.1\nHostKey {root}/host\nPidFile {root}/sshd.pid\nAuthorizedKeysFile {root}/client.pub\nStrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nLogLevel ERROR\n')
    log=(root/'log').open('w+')
    server=subprocess.Popen(['/usr/sbin/sshd','-D','-e','-f',str(config)],stdout=log,stderr=log)
    options=['/usr/bin/ssh','-F','none','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','GlobalKnownHostsFile=none','-o','UpdateHostKeys=no','-o',f'UserKnownHostsFile={root}/known','-o','IdentitiesOnly=yes','-o','IdentityAgent=none','-o','ConnectTimeout=3','-i',str(root/'client'),'-p',str(port),'-l',pwd.getpwuid(os.getuid()).pw_name,'127.0.0.1','true']
    try:
        time.sleep(0.5)
        if server.poll() is not None:
            log.seek(0); raise RuntimeError('local sshd unavailable: '+log.read())
        (root/'known').write_text('')
        assert subprocess.run(options,capture_output=True).returncode!=0
        (root/'known').write_text(f'[127.0.0.1]:{port} '+(root/'other.pub').read_text())
        assert subprocess.run(options,capture_output=True).returncode!=0
        (root/'known').write_text(f'[127.0.0.1]:{port} '+(root/'host.pub').read_text())
        result=subprocess.run(options,capture_output=True)
        assert result.returncode==0,result.stderr.decode()
    finally:
        server.terminate(); server.wait(timeout=10); log.close()
print('Real OpenSSH rejected unknown/changed host keys and authenticated only the trusted loopback host')
