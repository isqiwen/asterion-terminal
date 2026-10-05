"""Bootstrap orchestration double plus real generated-identity/mTLS Agent handshake."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from bundle_fixture import make_bundle
with tempfile.TemporaryDirectory(prefix='asterion-ssh-', ignore_cleanup_errors=True) as temporary:
    root=Path(temporary); tools=root/'tools'; tools.mkdir(); remote=root/'remote'; remote.mkdir()
    for tool in ('ssh','sftp'):
        shutil.copyfile(Path(__file__).with_name('ssh_fixture.py'),tools/tool); (tools/tool).chmod(0o700)
    known=root/'known_hosts'; known.write_text('test fixture, never passed to real ssh\n')
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0)); port=listener.getsockname()[1]
    resources=make_bundle(root/'resources',Path(sys.argv[2]).resolve().parent)
    env=dict(os.environ,ASTERION_REMOTE_RESOURCES=str(resources),ASTERION_NODE_DIRECTORY=str(root/'local'),ASTERION_SSH_TOOL_DIRECTORY=str(tools),ASTERION_SSH_FIXTURE=str(remote))
    host=subprocess.Popen([sys.argv[1]],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,env=env)
    def call(method,params):
        host.stdin.write(json.dumps(dict(version=1,method=method,params=params))+'\n'); host.stdin.flush(); return json.loads(host.stdout.readline())
    subprocess.run(['/usr/bin/ssh-keygen','-q','-t','ed25519','-N','','-f',str(root/'test-key')],check=True)
    private_key=(root/'test-key').read_text()
    request=dict(id='bootstrap-test',host='localhost',ssh_port='22',username='asterion',key_source='provided',private_key=private_key,known_hosts=str(known),agent_port=str(port))
    try:
        if sys.platform != 'linux':
            (remote/'target-os').write_text('macos')
            rejected=call('node.bootstrap',request)
            assert 'error' in rejected and 'Linux' in str(rejected['error'])
            assert not list(remote.glob('.asterion-install-*')), 'unsupported target reached upload'
            print('Non-Linux SSH target rejected before upload')
            raise SystemExit(0)
        for invalid in ('', 'ssh-ed25519 public-key', '/path/to/key'):
            assert 'error' in call('node.bootstrap',dict(request,private_key=invalid))
        assert 'error' in call('node.bootstrap',dict(request,auth='agent'))
        assert 'error' in call('node.bootstrap',dict(request,key_file='/path/to/key'))
        header=bytearray(64); header[:6]=b'\x7fELF\x02\x01'; header[18]=62
        linux_artifact=root/'linux-inspection-only'; linux_artifact.write_bytes(header)
        inspection=dict(request,username='tester',firewall_port=str(port),firewall_action='allow')
        assert 'error' in call('node.firewall.apply',dict(token='not-confirmed',private_key=private_key))
        readonly=call('node.firewall.inspect',dict(inspection,username='asterion'))['result']['firewall_plan']
        assert readonly['state']=='read_only' and not readonly['can_apply']
        (remote/'no-firewall-permission').touch()
        denied=call('node.firewall.inspect',inspection)['result']['firewall_plan']
        assert not denied['can_apply'] and denied['state']=='permission_required'
        assert 'error' in call('node.firewall.apply',dict(token=denied['token'],private_key=private_key))
        assert not (remote/'firewall-changes').exists()
        (remote/'no-firewall-permission').unlink()
        preview=call('node.firewall.inspect',inspection)['result']['firewall_plan']
        assert preview['source']=='192.0.2.10' and preview['can_apply']
        assert not (remote/'firewall-changes').exists(), 'inspection mutated firewall'
        applied=call('node.firewall.apply',dict(token=preview['token'],private_key=private_key))['result']['firewall_plan']
        assert applied['state']=='applied' and applied['verification']=='pending_install'
        assert 'error' in call('node.firewall.apply',dict(token=preview['token'],private_key=private_key))
        removal=call('node.firewall.inspect',dict(inspection,firewall_action='remove'))['result']['firewall_plan']
        assert removal['owned'] and removal['rule']==preview['rule']
        assert call('node.firewall.apply',dict(token=removal['token'],private_key=private_key))['result']['firewall_plan']['state']=='removed'
        (remote/'old-helper').touch()
        assert 'error' in call('node.bootstrap',request)
        assert not (root/'local/enrollments/bootstrap-test').exists(), 'obsolete helper reached identity creation'
        assert not list(remote.glob('.asterion-install-*')), 'obsolete helper reached upload'
        (remote/'old-helper').unlink()
        (remote/'reject').touch()
        assert 'error' in call('node.bootstrap',request)
        assert not (root/'local/enrollments/bootstrap-test').exists()
        (remote/'reject').unlink(); (remote/'fail_install').touch()
        assert 'error' in call('node.bootstrap',request)
        enrollment=root/'local/enrollments/bootstrap-test'
        fingerprint=(enrollment/'client.key').read_bytes()
        assert not list(remote.glob('.asterion-install-*'))
        (remote/'fail_install').unlink(); (remote/'fail_file_sync').touch()
        assert 'error' in call('node.bootstrap',request)
        deployed=remote/'deployed'
        assert not (deployed/'asterion-node-agent').exists(), 'failed file flush published an executable'
        assert not (deployed/'install.json').exists(), 'failed prerequisites committed an installation'
        assert not list(deployed.glob('.asterion-publish-*'))
        (remote/'fail_file_sync').unlink(); (remote/'fail_publish_sync').touch()
        assert 'error' in call('node.bootstrap',request)
        executable=deployed/'asterion-node-agent'
        assert executable.is_file() and not (deployed/'install.json').exists()
        before=executable.stat()
        (remote/'fail_publish_sync').unlink()
        result=call('node.bootstrap',request)
        assert result['result']['nodes'][0]['state']=='online',result
        after=executable.stat()
        assert (before.st_ino,before.st_mtime_ns)==(after.st_ino,after.st_mtime_ns), 'retry rewrote the published program'
        assert (deployed/'install.json').is_file()
        assert (enrollment/'client.key').read_bytes()==fingerprint
        assert not (enrollment/'ca.key').exists()
        assert not list(remote.glob('.asterion-install-*'))
        assert call('node.connect',{'id':'bootstrap-test'})['result']['nodes'][0]['state']=='online'
        assert 'error' in call('node.connect',{'id':'bootstrap-test','host':'localhost','port':str(port),'ca_file':'x','certificate_file':'x','private_key_file':'x'})
        assert 'error' in call('node.bootstrap',dict(request,agent_port=str(port+1)))
        assert 'error' in call('node.bootstrap',dict(request,id='../../escape'))
        assert 'error' in call('node.bootstrap',dict(request,host='host; touch BAD'))
        assert not (remote/'BAD').exists()
        for name in (remote/'identity-paths').read_text().splitlines():
            assert not Path(name).exists(), 'temporary SSH key survived operation'
        for file in enrollment.rglob('*'):
            if file.is_file():
                assert private_key.encode() not in file.read_bytes(), 'SSH key persisted in enrollment'
        assert private_key not in json.dumps(result)
    finally:
        host.kill(); host.communicate(timeout=10)
        if (remote/'pid').exists():
            os.kill(int((remote/'pid').read_text()),signal.SIGTERM); time.sleep(2)
print('SSH orchestration, strict option contract, failed-install retry, immutable identity, real mTLS enrollment and injection rejection verified; remote system installer is simulated')
