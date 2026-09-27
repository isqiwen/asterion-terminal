"""Opt-in real loopback SSH/SFTP + native system-service acceptance on POSIX.

Run only on a disposable machine with passwordless sudo. Uses generated test keys,
unique service IDs, real bootstrap code, and removes only its own installation.
This exercises service restart, not an OS reboot or a second physical machine.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import pwd
import socket
import subprocess
import tempfile
import time
import uuid
from bundle_fixture import make_bundle


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=30, **kwargs)


def port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def wait(check, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.5)
    raise AssertionError('native service did not reach expected state')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--allow-system-service', action='store_true', required=True)
    args = parser.parse_args()
    if platform.system() != 'Linux':
        parser.error('remote SSH system-service acceptance requires Linux')
    build = args.build.resolve()
    for executable in ('asterion_terminal_dev_bridge', 'asterion-node-agent', 'asterion-trading'):
        if not (build / executable).is_file():
            parser.error('build all native executables first')
    sudo = [] if os.getuid() == 0 else ['sudo', '-n']
    if subprocess.run([*sudo, 'true'], timeout=10, capture_output=True).returncode:
        parser.error('system-service acceptance needs passwordless sudo on a disposable test host; no installation was attempted')
    linux = platform.system() == 'Linux'
    if linux:
        run(['systemctl', '--version'], stdout=subprocess.DEVNULL)
        if not Path('/run/systemd/system').is_dir():
            parser.error('a booted systemd host is required, not an ordinary container')
    identity = 'acceptance-' + uuid.uuid4().hex[:12]
    label = ('asterion-node-agent-' + identity + '.service') if linux else ('me.asterion.node-agent.' + identity)
    installation = Path('/var/lib/asterion/nodes' if linux else '/Library/Application Support/Asterion/nodes') / identity
    definition = Path('/etc/systemd/system') / label if linux else Path('/Library/LaunchDaemons') / (label + '.plist')
    # No existing names or directories may be reused or removed by this test.
    run([*sudo, 'test', '!', '-e', str(installation)])
    run([*sudo, 'test', '!', '-e', str(definition)])
    terminal = sshd = None
    with tempfile.TemporaryDirectory(prefix='asterion-native-ssh-') as temporary:
        root = Path(temporary)
        for name in ('host',):
            run(['/usr/bin/ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(root / name)])
        ssh_port, agent_port, trading_port = port(), port(), port()
        while len({ssh_port, agent_port, trading_port}) != 3:
            ssh_port, agent_port, trading_port = port(), port(), port()
        config = root / 'sshd.conf'
        config.write_text(f'Port {ssh_port}\nListenAddress 127.0.0.1\nHostKey {root}/host\nPidFile {root}/sshd.pid\nAuthorizedKeysFile {"/etc/ssh/asterion_authorized_keys" if linux else str(root / "client.pub")}\nStrictModes yes\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM {"yes" if linux else "no"}\nPermitRootLogin prohibit-password\nSubsystem sftp internal-sftp\nLogLevel ERROR\n')
        known = root / 'known_hosts'
        known.write_text(f'[127.0.0.1]:{ssh_port} ' + (root / 'host.pub').read_text())
        resources = make_bundle(root / "resources", build)
        env = dict(os.environ, ASTERION_REMOTE_RESOURCES=str(resources), ASTERION_NODE_DIRECTORY=str(root / 'local'))
        env.pop('ASTERION_SSH_TOOL_DIRECTORY', None)
        env.pop('ASTERION_SSH_FIXTURE', None)

        def launch():
            return subprocess.Popen([str(build / 'asterion_terminal_dev_bridge')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

        def call(method, params):
            terminal.stdin.write(json.dumps(dict(version=1, method=method, params=params)) + '\n')
            terminal.stdin.flush()
            response = json.loads(terminal.stdout.readline())
            # Do not print request parameters: bootstrap includes a test private key.
            assert 'error' not in response, response
            return response['result']

        def health():
            return call('runtime.snapshot', {})['nodes'][0]

        def service_running():
            current = health()
            return current['state'] == 'online' and any(s['id'] == 'paper-acceptance' and s['state'] == 'running' and s['health'] in ('ready', 'awaiting_input') for s in current['health']['services'])

        def service_command(action):
            if linux:
                return [*sudo, 'systemctl', action, label]
            return [*sudo, 'launchctl', 'kickstart', '-k', 'system/' + label]

        if linux:
            # This opt-in fixture must never adopt a pre-existing account or policy.
            try:
                pwd.getpwnam('asterion')
            except KeyError:
                pass
            else:
                raise RuntimeError('dedicated asterion account already exists; use a disposable host')
            for item in ('/var/lib/asterion', '/usr/local/sbin/asterion-host', '/etc/ssh/asterion_authorized_keys', '/etc/ssh/sshd_config.d/00-asterion.conf', '/etc/sudoers.d/asterion-host'):
                run([*sudo, 'test', '!', '-e', item])
        try:
            terminal = launch()
            generated = call('node.key.prepare', dict(id=identity))['ssh_key']
            (root / 'client.pub').write_text(generated['public_key'])
            if linux:
                run([*sudo, '/usr/bin/python3', '-I', str(Path(__file__).resolve().parents[1] / 'scripts/node/initialize-linux.py'), '--public-key', str(root / 'client.pub')])
            with (root / 'sshd.log').open('w') as log:
                sshd = subprocess.Popen([*sudo, '/usr/sbin/sshd', '-D', '-e', '-f', str(config)], stdout=log, stderr=log)
            time.sleep(1)
            assert sshd.poll() is None, 'isolated sshd could not start'
            result = call('node.bootstrap', dict(id=identity, host='127.0.0.1', ssh_port=str(ssh_port), username='asterion' if linux else pwd.getpwuid(os.getuid()).pw_name, key_source='managed', private_key='', known_hosts=str(known), agent_port=str(agent_port)))
            assert result['nodes'][0]['state'] == 'online'
            if linux:
                service_user = run(['systemctl', 'show', label, '--property=User', '--value'], capture_output=True, text=True).stdout.strip()
                assert service_user == 'asterion'
            native = result['nodes'][0]['health']
            call('node.deploy', dict(kind='paper', id=identity, service='paper-acceptance', port=str(trading_port)))
            wait(service_running)
            # Closing Terminal must not terminate either system service or trading.
            terminal.terminate(); terminal.communicate(timeout=15)
            terminal = launch()
            call('node.connect', dict(id=identity))
            wait(service_running)
            old_pid = health()['health']['services'][0]['pid']
            run(service_command('restart'))
            wait(lambda: service_running() and health()['health']['services'][0]['pid'] != old_pid)
            call('node.action', dict(kind='paper', id=identity, service='paper-acceptance', action='stop'))
            run(service_command('restart'))
            # Reattach after restart: explicit stopped state must survive.
            time.sleep(2)
            call('node.disconnect', dict(id=identity))
            call('node.connect', dict(id=identity))
            assert health()['health']['services'][0]['state'] == 'stopped'
            print('PASS: real SSH/SFTP install, native service, mTLS, deployment, Terminal exit, Agent restart and durable stop')
        finally:
            if terminal is not None and terminal.poll() is None:
                terminal.kill(); terminal.communicate(timeout=15)
            if sshd is not None:
                if (root / 'sshd.pid').exists():
                    pid = (root / 'sshd.pid').read_text().strip()
                    if pid.isdecimal():
                        subprocess.run([*sudo, 'kill', '-TERM', pid], timeout=10)
                if sshd.poll() is None:
                    sshd.terminate()
                sshd.wait(timeout=10)
            # Stop only the uniquely named service installed by this fixture.
            cleanup = [*sudo, 'systemctl', 'disable', '--now', label] if linux else [*sudo, 'launchctl', 'bootout', 'system/' + label]
            subprocess.run(cleanup, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            query = [*sudo, 'systemctl', 'is-active', '--quiet', label] if linux else [*sudo, 'launchctl', 'print', 'system/' + label]
            if subprocess.run(query, timeout=15, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                raise RuntimeError('test service remains active; preserved installation for cleanup')
            run([*sudo, 'rm', '-f', str(definition)])
            run([*sudo, 'rm', '-rf', str(installation)])
            if linux:
                run([*sudo, 'systemctl', 'daemon-reload'])
                run([*sudo, 'rm', '-f', '/usr/local/sbin/asterion-host', '/etc/ssh/asterion_authorized_keys', '/etc/ssh/sshd_config.d/00-asterion.conf', '/etc/sudoers.d/asterion-host'])
                subprocess.run([*sudo, 'userdel', '--remove', 'asterion'], timeout=30, check=True)
                run([*sudo, 'systemctl', 'reload', 'ssh.service'])



if __name__ == '__main__':
    main()
