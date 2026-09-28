"""Real OpenSSH generation, persistence, managed authentication and leak checks."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='asterion-managed-key-', ignore_cleanup_errors=True) as folder:
    root=Path(folder)
    tools=root/'tools'; tools.mkdir()
    remote=root/'remote'; remote.mkdir()
    for name in ('ssh','sftp'):
        shutil.copyfile(Path(__file__).with_name('ssh_fixture.py'),tools/name)
        (tools/name).chmod(0o700)
    (tools/'ssh-keygen').symlink_to('/usr/bin/ssh-keygen')
    env=dict(os.environ,ASTERION_NODE_DIRECTORY=str(root/'state'),ASTERION_SSH_TOOL_DIRECTORY=str(tools),ASTERION_SSH_FIXTURE=str(remote))
    def launch():
        return subprocess.Popen([sys.argv[1]],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,env=env)
    app=launch()
    def call(method,params):
        app.stdin.write(json.dumps(dict(version=1,method=method,params=params))+'\n'); app.stdin.flush()
        response=json.loads(app.stdout.readline())
        assert 'PRIVATE KEY' not in json.dumps(response), 'private key leaked to response'
        return response
    try:
        assert 'error' in call('node.key.prepare',dict(id='../escape'))
        enrolled=root/'state/enrollments/lost-key'; enrolled.mkdir(parents=True)
        (enrolled/'enrollment.json').write_text('{}')
        assert 'error' in call('node.key.prepare',dict(id='lost-key'))
        assert not (enrolled.parent/'.ssh-keys/lost-key').exists()
        prepared=call('node.key.prepare',dict(id='research'))['result']['ssh_key']
        assert prepared['id']=='research' and prepared['public_key'].startswith('ssh-ed25519 ')
        key=root/'state/enrollments/.ssh-keys/research/identity'
        assert key.stat().st_mode & 0o777 == 0o600
        assert key.parent.stat().st_mode & 0o777 == 0o700
        original=key.read_bytes()
        public=subprocess.run(['/usr/bin/ssh-keygen','-y','-f',str(key)],check=True,capture_output=True,text=True).stdout
        assert public.split()[:2]==prepared['public_key'].split()[:2]
        assert call('node.key.prepare',dict(id='research'))['result']['ssh_key']==prepared
        app.terminate(); app.communicate(timeout=10); app=launch()
        assert call('node.key.prepare',dict(id='research'))['result']['ssh_key']==prepared
        assert key.read_bytes()==original
        public_file=key.with_name('identity.pub'); saved_public=public_file.read_text()
        public_file.write_text('ssh-ed25519 broken\n')
        assert 'error' in call('node.key.prepare',dict(id='research'))
        assert key.read_bytes()==original
        public_file.write_text(saved_public)
        artifact=root/'linux'; header=bytearray(64); header[:6]=b'\x7fELF\x02\x01'; header[18]=62; artifact.write_bytes(header)
        known=root/'known'; known.write_text('test double only\n')
        request=dict(id='research',host='localhost',ssh_port='22',username='asterion',key_source='managed',private_key='',known_hosts=str(known),agent_port='7442',firewall_port='7442',firewall_action='allow')
        assert call('node.firewall.inspect',request)['result']['firewall_plan']['state']=='read_only'
        assert 'error' in call('node.firewall.inspect',dict(request,private_key=original.decode()))
        assert 'error' in call('node.firewall.inspect',dict(request,id='missing'))
        key.chmod(0o644)
        assert 'error' in call('node.firewall.inspect',request)
        key.chmod(0o600)
        key.unlink(); key.symlink_to(root/'outside')
        assert 'error' in call('node.key.prepare',dict(id='research'))
        for saved in (root/'state').rglob('*'):
            if saved.is_file() and not saved.is_symlink():
                assert original not in saved.read_bytes(), 'private material leaked outside the managed identity'
        assert all(not Path(line).exists() for line in (remote/'identity-paths').read_text().splitlines())
    finally:
        app.terminate(); app.communicate(timeout=10)
print('Managed SSH key: real generation, matching public key, restart reuse, automatic authentication, path/permission rejection and no response/config leaks passed')
