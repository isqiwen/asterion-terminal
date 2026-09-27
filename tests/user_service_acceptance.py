"""Opt-in native user supervisor acceptance without administrator privileges.

Creates one unique transient systemd user service or launchd user agent and cleans
it up. Validates OS supervision; does not claim SSH installation or reboot coverage.
"""
import argparse
import html
import json
import os
from pathlib import Path
import platform
import signal
import socket
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--allow-user-service', required=True, action='store_true')
    args = parser.parse_args()
    if platform.system() not in ('Linux', 'Darwin'):
        parser.error('requires systemd user manager or macOS login session')
    build = args.build.resolve()
    linux = platform.system() == 'Linux'
    name = 'me.asterion.acceptance.' + uuid.uuid4().hex[:12]
    target = name if linux else f'gui/{os.getuid()}/{name}'

    def command(arguments, check=True):
        return subprocess.run(arguments, check=check, capture_output=True, timeout=30)

    def wait(check):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if check():
                return
            time.sleep(0.5)
        raise AssertionError('user supervisor state timeout')

    terminal = None
    registered = False
    with tempfile.TemporaryDirectory(prefix='asterion-user-supervisor-') as temporary:
        root = Path(temporary); state = root / 'state'; state.mkdir()
        command([str(build / 'asterion_test_certificates'), str(root)])
        sockets = [socket.socket(), socket.socket()]
        for s in sockets:
            s.bind(('127.0.0.1', 0))
        management, trading = [s.getsockname()[1] for s in sockets]
        for s in sockets:
            s.close()
        arguments = [str(build / 'asterion-node-agent'), '--directory', str(state), '--bind', '127.0.0.1', '--port', str(management), '--tls-ca', str(root / 'ca.crt'), '--tls-cert', str(root / 'server.crt'), '--tls-key', str(root / 'server.key')]
        enrollment = root / 'local/enrollments/acceptance'; enrollment.mkdir(parents=True)
        import shutil
        for file in ('ca.crt', 'client.crt', 'client.key'):
            shutil.copyfile(root / file, enrollment / file)
        (enrollment / 'enrollment.json').write_text(json.dumps(dict(version=1, id='acceptance', host='localhost', port=management)))

        def launch():
            return subprocess.Popen([str(build / 'asterion_terminal_dev_bridge')], env=dict(os.environ, ASTERION_NODE_DIRECTORY=str(root / 'local')), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

        def call(method, params=None):
            terminal.stdin.write(json.dumps(dict(version=1, method=method, params=params or {})) + '\n'); terminal.stdin.flush()
            response = json.loads(terminal.stdout.readline())
            assert 'error' not in response, response
            return response['result']

        def health():
            return call('runtime.snapshot')['nodes'][0]

        def ready():
            node = health()
            return node['state'] == 'online' and node['health']['services'] and node['health']['services'][0]['state'] == 'running' and node['health']['services'][0]['health'] in ('ready', 'awaiting_input')

        def new_pid(previous):
            try:
                pid = int((state / 'agent.pid').read_text() or '0')
                return pid > 0 and pid != previous
            except FileNotFoundError:
                return False

        try:
            if linux:
                command(['systemd-run', '--user', '--unit=' + name, '--property=Restart=on-failure', '--property=RestartSec=1', *arguments])
            else:
                plist = root / 'agent.plist'
                plist.write_text('<?xml version="1.0"?><plist version="1.0"><dict><key>Label</key><string>' + name + '</string><key>ProgramArguments</key><array>' + ''.join('<string>' + html.escape(a) + '</string>' for a in arguments) + '</array><key>RunAtLoad</key><true/><key>KeepAlive</key><true/><key>ThrottleInterval</key><integer>1</integer></dict></plist>')
                command(['/bin/launchctl', 'bootstrap', f'gui/{os.getuid()}', str(plist)])
            registered = True
            wait(lambda: new_pid(0))
            terminal = launch()
            native = call('node.connect', dict(id='acceptance'))['nodes'][0]['health']
            call('node.deploy', dict(id='acceptance', executable=str(build / 'asterion-trading'), os=native['os'], arch=native['arch'], service='paper-user-test', port=str(trading)))
            wait(ready)
            call('node.attach', dict(id='acceptance', service='paper-user-test'))
            assert call('runtime.snapshot')['connection']['state'] == 'connected'
            call('paper.close')
            trading_pid = health()['health']['services'][0]['pid']
            terminal.terminate(); terminal.communicate(timeout=15)
            terminal = launch(); call('node.connect', dict(id='acceptance'))
            assert health()['health']['services'][0]['pid'] == trading_pid
            pid = int((state / 'agent.pid').read_text()); os.kill(pid, signal.SIGKILL)
            wait(lambda: new_pid(pid))
            wait(lambda: ready() and health()['health']['services'][0]['pid'] != trading_pid)
            call('node.action', dict(id='acceptance', service='paper-user-test', action='stop'))
            pid = int((state / 'agent.pid').read_text()); os.kill(pid, signal.SIGKILL)
            wait(lambda: new_pid(pid))
            call('node.disconnect', dict(id='acceptance')); call('node.connect', dict(id='acceptance'))
            assert health()['health']['services'][0]['state'] == 'stopped'
            print('PASS:', platform.system(), 'native user supervisor, mTLS, deployment, trading connection, Terminal exit, crash restart, durable explicit stop')
        finally:
            if terminal is not None and terminal.poll() is None:
                terminal.kill(); terminal.communicate(timeout=15)
            if registered:
                if linux:
                    command(['systemctl', '--user', 'stop', name])
                    command(['systemctl', '--user', 'reset-failed', name], check=False)
                else:
                    command(['/bin/launchctl', 'bootout', target])


if __name__ == '__main__':
    main()
