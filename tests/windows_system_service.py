"""Opt-in native Windows SCM/mTLS acceptance on a disposable elevated host.

This validates SCM service execution and recovery, not SSH installation or reboot.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--allow-system-service', required=True, action='store_true')
    args = parser.parse_args()
    if os.name != 'nt' or not ctypes.windll.shell32.IsUserAnAdmin():
        parser.error('requires an elevated Windows test host')
    build = args.build.resolve()
    for name in ('asterion-node-agent', 'asterion_terminal_dev_bridge', 'asterion_test_certificates'):
        if not (build / (name + '.exe')).is_file():
            parser.error('build all native executables first')
    name = 'AsterionAcceptance-' + uuid.uuid4().hex[:12]
    root = Path(os.environ['ProgramData']) / name
    root.mkdir()  # Refuse reuse of any existing directory.
    state = root / 'state'; state.mkdir()
    terminal = None
    created = False

    def sc(*arguments, check=True):
        return subprocess.run(['sc.exe', *arguments], check=check, capture_output=True, timeout=30)

    def wait(check):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if check():
                return
            time.sleep(0.5)
        raise AssertionError('Windows service did not reach expected state')

    try:
        subprocess.run([str(build / 'asterion_test_certificates.exe'), str(root)], check=True, timeout=15)
        # The same service account used by the product installer.
        subprocess.run(['icacls.exe', str(root), '/grant', '*S-1-5-19:(OI)(CI)F', '/T'], check=True, capture_output=True, timeout=15)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
        binary = subprocess.list2cmdline([str(build / 'asterion-node-agent.exe'), '--windows-service', name, '--directory', str(state), '--bind', '127.0.0.1', '--port', str(port), '--tls-ca', str(root / 'ca.crt'), '--tls-cert', str(root / 'server.crt'), '--tls-key', str(root / 'server.key')])
        # Copy the executable into the fixture directory with inherited LocalService access.
        import shutil
        executable = root / 'asterion-node-agent.exe'
        shutil.copyfile(build / 'asterion-node-agent.exe', executable)
        binary = binary.replace(str(build / 'asterion-node-agent.exe'), str(executable), 1)
        sc('create', name, 'binPath=', binary, 'start=', 'demand', 'obj=', 'NT AUTHORITY\\LocalService')
        created = True
        sc('failure', name, 'reset=', '3600', 'actions=', 'restart/1000/restart/1000/none/0')
        sc('start', name)
        wait(lambda: (state / 'agent.pid').is_file())
        with tempfile.TemporaryDirectory(prefix='asterion-scm-client-') as directory:
            enrollment = Path(directory) / 'enrollments' / 'acceptance'; enrollment.mkdir(parents=True)
            for file in ('ca.crt', 'client.crt', 'client.key'):
                shutil.copyfile(root / file, enrollment / file)
            (enrollment / 'enrollment.json').write_text(json.dumps(dict(version=1, id='acceptance', host='localhost', port=port)))
            terminal = subprocess.Popen([str(build / 'asterion_terminal_dev_bridge.exe')], env=dict(os.environ, ASTERION_NODE_DIRECTORY=directory), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

            def call(method, params):
                terminal.stdin.write(json.dumps(dict(version=1, method=method, params=params)) + '\n'); terminal.stdin.flush()
                result = json.loads(terminal.stdout.readline())
                assert 'error' not in result, result
                return result['result']

            assert call('node.connect', dict(id='acceptance'))['nodes'][0]['state'] == 'online'
            pid = int((state / 'agent.pid').read_text())
            subprocess.run(['taskkill.exe', '/PID', str(pid), '/F'], check=True, capture_output=True, timeout=15)
            wait(lambda: int((state / 'agent.pid').read_text() or '0') not in (0, pid))
            call('node.disconnect', dict(id='acceptance'))
            assert call('node.connect', dict(id='acceptance'))['nodes'][0]['state'] == 'online'
            sc('stop', name)
            wait(lambda: b'STOPPED' in sc('query', name).stdout)
            print('PASS: native Windows SCM, LocalService identity, mTLS, crash restart and explicit stop')
    finally:
        if terminal is not None:
            terminal.kill(); terminal.communicate(timeout=10)
        if created:
            sc('stop', name, check=False)
            wait(lambda: b'STOPPED' in sc('query', name).stdout)
            sc('delete', name)
        import shutil
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
