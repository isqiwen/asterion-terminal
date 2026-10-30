"""Real Manager, mTLS Agent and publication, with SSH/systemd isolated by a transport double."""
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
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from installation import install  # noqa: E402
from bundle_fixture import make_bundle

rpc, manager, agent = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
if sys.platform != 'linux':
    print('Remote runtime acceptance requires Linux x86_64')
    raise SystemExit(0)
source = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ast-rem-') as directory:
    root = Path(directory)
    tools = root/'tools'; tools.mkdir()
    remote = root/'remote'; remote.mkdir(mode=0o700)
    local = root/'local'; (local/'manager').mkdir(parents=True)
    shutil.copytree(source/'config', local/'config')
    for name in ('ssh', 'sftp'):
        shutil.copyfile(Path(__file__).with_name('ssh_fixture.py'), tools/name)
        (tools/name).chmod(0o700)
    resources = make_bundle(root/'resources', Path(agent).parent, strip_debug=True)
    known = root/'known_hosts'; known.write_text('isolated fixture identity\n')
    subprocess.run(['/usr/bin/ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(root/'identity')], check=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
    env = dict(os.environ, ASTERION_REMOTE_RESOURCES=str(resources), ASTERION_SSH_TOOL_DIRECTORY=str(tools),
               ASTERION_SSH_FIXTURE=str(remote), ASTERION_TEST_MANAGER_RPC=rpc,
               ASTERION_TEST_OWNER_PID=str(os.getpid()))
    endpoint = str(root/'manager.sock')
    def launch():
        child = subprocess.Popen([str(install(local, 'manager', manager)), '--endpoint', endpoint,
            '--enrollment-directory', str(local/'manager/data/enrollments'), '--isolated'], env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 15
        while not Path(endpoint).exists():
            assert child.poll() is None, child.stderr.read().decode()
            assert time.monotonic() < deadline
            time.sleep(.05)
        return child
    process = launch()
    client = subprocess.Popen([rpc, endpoint], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
    def call(request, fail=False):
        client.stdin.write(json.dumps(request)+'\n'); client.stdin.flush()
        response = json.loads(client.stdout.readline())
        assert ('error' in response) == fail, response
        return response
    params = dict(id='bootstrap-test', host='localhost', sshPort='22', username='asterion',
                  keySource='provided', privateKey=(root/'identity').read_text(), knownHosts=str(known), agentPort=str(port))
    def maintain(action, fail=False, **fields):
        return call(dict(maintenance=dict(action=action, resources=str(resources), targetVersion='0.1.0',
                                          connections=[params], **fields)), fail)
    try:
        call(dict(ssh=dict(params, operation='ENROLL')))
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); market_port = listener.getsockname()[1]
        call(dict(node=dict(id='bootstrap-test', deploy=dict(service='market', kind='MARKET', port=market_port))))
        service_config = remote/'deployed/agent/data/services/market/service.json'
        maintain('PREPARE', True)
        call(dict(node=dict(id='bootstrap-test', action=dict(service='market', action='stop'))))
        before_service = json.loads(service_config.read_text())
        assert before_service['port'] == market_port and not before_service['desired']
        view = maintain('INSPECT')
        assert view['nodes'][0]['installed'] and view['nodes'][0]['ssh_configured']
        profile = local/'manager/data/enrollments/bootstrap-test/ssh.json'
        assert 'PRIVATE KEY' not in profile.read_text() and 'private_key' not in profile.read_text()
        ledger = remote/'deployed/market/data/market/retained-ledger'; ledger.write_text('business fixture')
        config = remote/'deployed/agent/config/research.json'; config.write_text('{"task_concurrency":5}')
        (remote/'reject').touch()
        maintain('PREPARE', True)
        assert not (local/'manager/data/enrollments/.maintenance.json').exists()
        (remote/'reject').unlink()
        (remote/'fail_maintenance').touch()
        maintain('PREPARE', True)
        assert (remote/'deployed/agent/data/maintenance.json').is_file()
        assert ledger.read_text() == 'business fixture'
        assert maintain('INSPECT')['pending']['nodes'][0]['phase'] == 'stopping'
        call(dict(node=dict(id='bootstrap-test', connect={})), True)
        process.terminate(); process.wait(timeout=10); process = launch()
        (remote/'fail_maintenance').unlink()
        view = maintain('PREPARE')
        assert view['pending']['nodes'][0]['phase'] == 'prepared', view
        assert not (remote/'pid').exists()
        assert json.loads(config.read_text())['task_concurrency'] == 5
        assert json.loads(service_config.read_text())['port'] == market_port
        assert not json.loads(service_config.read_text())['desired']
        view = maintain('FINISH')
        assert view['pending'] is None
        assert not (remote/'deployed/agent/data/maintenance.json').exists()
        assert ledger.read_text() == 'business fixture'
        maintain('UNINSTALL', nodes=['bootstrap-test'])
        assert ledger.exists() and config.exists()
        assert not (remote/'deployed/agent/bin/asterion_node_agent').exists()
        assert not maintain('INSPECT')['nodes'][0]['installed']
        call(dict(ssh=dict(params, operation='ENROLL')))
        maintain('UNINSTALL', nodes=['bootstrap-test'], removeData=True)
        assert not ledger.exists() and not config.exists()
        assert (remote/'deployed/agent/logs').is_dir()
        print('remote installation, offline preflight, interrupted upgrade, Manager restart, explicit completion, uninstall retention/removal passed')
    finally:
        client.stdin.close(); client.wait(timeout=10)
        process.terminate(); process.wait(timeout=10)
        if (remote/'pid').exists():
            try: os.kill(int((remote/'pid').read_text()), signal.SIGTERM)
            except ProcessLookupError: pass
