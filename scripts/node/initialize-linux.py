#!/usr/bin/python3 -I
"""Initialize a dedicated Linux SSH account, or manage its fixed systemd units.

Run initialization locally as an administrator. Only a PUBLIC SSH key is accepted.
The installed copy exposes a narrow sudo entry point; uploaded code never runs as root.
"""
import argparse
import base64
import grp
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys
import tempfile

ACCOUNT = 'asterion'
HOME = '/var/lib/asterion'
HELPER = Path('/usr/local/sbin/asterion-host')
ENV = {'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LC_ALL': 'C'}


def run(*args, **kwargs):
    return subprocess.run(args, check=True, timeout=60, env=ENV, **kwargs)


def public_key(value):
    parts = value.strip().split()
    if len(parts) < 2 or parts[0] not in ('ssh-ed25519', 'ssh-rsa', 'ecdsa-sha2-nistp256', 'ecdsa-sha2-nistp384', 'ecdsa-sha2-nistp521'):
        raise ValueError('Provide one OpenSSH public key, without authorized_keys options')
    if '\n' in value.strip() or len(value) > 16384:
        raise ValueError('Provide exactly one public key')
    blob = base64.b64decode(parts[1], validate=True)
    size = int.from_bytes(blob[:4], 'big')
    if size > 128 or blob[4:4 + size] != parts[0].encode():
        raise ValueError('Public key type does not match its contents')
    return parts[0] + ' ' + parts[1] + '\n'


def protected_directory(path):
    """Refuse writable/symlink ancestors before making root-owned files."""
    for item in [*reversed(path.parents), path]:
        info = item.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError(f'Unsafe administrative directory: {item}')


def put_once(path, content, mode):
    protected_directory(path.parent)
    data = content.encode() if isinstance(content, str) else content
    if path.exists() or path.is_symlink():
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or stat.S_IMODE(info.st_mode) != mode or path.read_bytes() != data:
            raise ValueError(f'Existing configuration differs; review it before retrying: {path}')
        return
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, 'wb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def account():
    user = pwd.getpwnam(ACCOUNT)
    if user.pw_uid == 0 or user.pw_dir != HOME or user.pw_shell != '/bin/sh' or grp.getgrgid(user.pw_gid).gr_name != ACCOUNT:
        raise ValueError('Existing asterion account does not match the dedicated account contract')
    if set(os.getgrouplist(ACCOUNT, user.pw_gid)) != {user.pw_gid}:
        raise ValueError('asterion must not belong to supplementary groups')
    return user


def service_text(identity, port, bind):
    if not re.fullmatch('[a-zA-Z0-9][a-zA-Z0-9_-]{0,40}', identity):
        raise ValueError('Invalid node ID')
    if not 1024 <= port <= 65535 or bind not in ('0.0.0.0', '::'):
        raise ValueError('Invalid unprivileged listener')
    root = f'{HOME}/nodes/{identity}'
    return (f'[Unit]\nDescription=Asterion Node Agent\nAfter=network.target\nStartLimitIntervalSec=60\nStartLimitBurst=3\n'
            f'[Service]\nUser={ACCOUNT}\nGroup={ACCOUNT}\nUMask=0077\n'
            f'ExecStart={root}/asterion-node-agent --directory {root}/state --bind {bind} --port {port} '
            f'--tls-ca {root}/ca.crt --tls-cert {root}/server.crt --tls-key {root}/server.key\n'
            'NoNewPrivileges=yes\nProtectSystem=strict\nProtectHome=yes\nPrivateTmp=yes\n'
            f'ReadWritePaths={HOME}\nCapabilityBoundingSet=\nRestrictSUIDSGID=yes\n'
            'Restart=on-failure\nRestartSec=5\n[Install]\nWantedBy=multi-user.target\n')


def manage(arguments):
    account()
    if arguments == ['check']:
        print('asterion-host-v1')
        return
    if len(arguments) != 4 or arguments[0] != 'install':
        raise ValueError('Only check and install NODE PORT BIND are supported')
    _, identity, port, bind = arguments
    definition = service_text(identity, int(port), bind)
    unit = f'asterion-node-agent-{identity}.service'
    put_once(Path('/etc/systemd/system') / unit, definition, 0o644)
    run('/usr/bin/systemctl', 'daemon-reload')
    run('/usr/bin/systemctl', 'enable', '--now', unit)


def initialize(arguments):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public-key', help='Optional PUBLIC key file; otherwise paste the public key when prompted')
    args = parser.parse_args(arguments)
    key = public_key(Path(args.public_key).read_text() if args.public_key else input('粘贴 Terminal 显示的 SSH 公钥（单行，不是私钥）: '))
    if not Path('/run/systemd/system').is_dir():
        raise ValueError('A booted systemd Linux host is required')
    for executable in ('/usr/sbin/sshd', '/usr/sbin/useradd', '/usr/sbin/visudo', '/usr/bin/ssh-keygen', '/usr/bin/systemctl'):
        if not os.access(executable, os.X_OK):
            raise ValueError(f'Install the prerequisite before initialization: {executable}')
    protected_directory(Path('/etc/ssh/sshd_config.d'))
    protected_directory(HELPER.parent)
    protected_directory(Path('/etc/sudoers.d'))
    # Validate the full key with OpenSSH before touching accounts or configuration.
    with tempfile.NamedTemporaryFile(mode='w', suffix='.pub') as sample:
        sample.write(key); sample.flush()
        run('/usr/bin/ssh-keygen', '-lf', sample.name, stdout=subprocess.DEVNULL)
    ssh_config = ('Match User asterion\n    AuthenticationMethods publickey\n'
                  '    PubkeyAuthentication yes\n    PasswordAuthentication no\n'
                  '    KbdInteractiveAuthentication no\n    DisableForwarding yes\n'
                  '    PermitTTY no\n    PermitUserRC no\n'
                  '    AuthorizedKeysFile /etc/ssh/asterion_authorized_keys\nMatch all\n')
    policy = (f'asterion ALL=(root) NOPASSWD: {HELPER} --manage *\n'
              'asterion ALL=(root) NOPASSWD: /usr/sbin/ufw status verbose\n')
    with tempfile.NamedTemporaryFile(mode='w') as sample:
        sample.write(policy); sample.flush()
        run('/usr/sbin/visudo', '-cf', sample.name, stdout=subprocess.DEVNULL)
    try:
        account()
    except KeyError:
        if Path(HOME).exists():
            raise ValueError('Refusing to take over an existing /var/lib/asterion directory')
        run('/usr/sbin/useradd', '--create-home', '--home-dir', HOME, '--shell', '/bin/sh', '--user-group', ACCOUNT)
        account()
    put_once(HELPER, Path(__file__).read_bytes(), 0o755)
    put_once(Path('/etc/ssh/asterion_authorized_keys'), 'restrict ' + key, 0o644)
    ssh_file = Path('/etc/ssh/sshd_config.d/00-asterion.conf')
    existed = ssh_file.exists()
    put_once(ssh_file, ssh_config, 0o644)
    try:
        run('/usr/sbin/sshd', '-t')
        effective = run('/usr/sbin/sshd', '-T', '-C', 'user=asterion,host=localhost,addr=127.0.0.1', capture_output=True, text=True).stdout
        required = ['pubkeyauthentication yes', 'authenticationmethods publickey', 'passwordauthentication no', 'kbdinteractiveauthentication no',
                    'authorizedkeysfile /etc/ssh/asterion_authorized_keys', 'disableforwarding yes', 'permittty no', 'permituserrc no', 'usepam yes']
        if any(line not in effective.splitlines() for line in required):
            raise ValueError('Existing SSH policy overrides initialization or does not include sshd_config.d; review sshd -T')
    except Exception:
        if not existed:
            ssh_file.unlink()
        raise
    put_once(Path('/etc/sudoers.d/asterion-host'), policy, 0o440)
    run('/usr/sbin/visudo', '-c', stdout=subprocess.DEVNULL)
    # Verify the active unit before reloading it; do not start or enable global services.
    for unit in ('ssh.service', 'sshd.service'):
        if subprocess.run(['/usr/bin/systemctl', 'is-active', '--quiet', unit], env=ENV).returncode == 0:
            run('/usr/bin/systemctl', 'reload', unit)
            break
    else:
        raise ValueError('SSH service is not active; initialization files remain for inspection')
    print('Initialized asterion: public-key SSH, restricted Agent registration, read-only firewall inspection.')
    print('No Agent installed and no firewall port opened. Connect from Terminal using the matching private key.')
    run('/usr/bin/ssh-keygen', '-lf', '/etc/ssh/ssh_host_ed25519_key.pub')


def main():
    if '--help' in sys.argv[1:]:
        initialize(sys.argv[1:]); return
    if sys.platform != 'linux' or os.geteuid() != 0:
        raise ValueError('Run locally on the target Linux host with sudo / as root')
    os.umask(0o022)
    if sys.argv[1:2] == ['--manage']:
        manage(sys.argv[2:])
    else:
        initialize(sys.argv[1:])


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'Initialization/management failed: {error}', file=sys.stderr)
        sys.exit(1)
