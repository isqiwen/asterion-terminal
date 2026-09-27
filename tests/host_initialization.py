"""Permission-boundary tests; never alter host accounts, sudoers or services."""
import base64
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('initialize_host', Path(__file__).resolve().parents[1] / 'scripts/node/initialize-linux.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


class HostInitialization(unittest.TestCase):
    def test_only_public_key_without_options(self):
        kind = b'ssh-ed25519'
        encoded = base64.b64encode(len(kind).to_bytes(4, 'big') + kind + b'\0' * 36).decode()
        key = 'ssh-ed25519 ' + encoded
        self.assertEqual(host.public_key(key + ' user@example\n'), key + '\n')
        for invalid in ('-----BEGIN OPENSSH PRIVATE KEY-----', 'command="sh" ' + key, key + '\n' + key, 'ssh-rsa ' + encoded, 'ssh-ed25519 !!!!'):
            with self.subTest(invalid=invalid[:20]), self.assertRaises(ValueError):
                host.public_key(invalid)

    def test_default_prompt_accepts_public_key_before_any_mutation(self):
        kind = b'ssh-ed25519'
        key = 'ssh-ed25519 ' + base64.b64encode(len(kind).to_bytes(4, 'big') + kind + b'\0' * 36).decode()
        with patch('builtins.input', return_value=key) as prompt, patch.object(host.Path, 'is_dir', return_value=False), patch.object(host, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'systemd'):
                host.initialize([])
            prompt.assert_called_once()
            run.assert_not_called()

    def test_unprivileged_fixed_service(self):
        unit = host.service_text('node-1', 7442, '0.0.0.0')
        self.assertIn('User=asterion\nGroup=asterion\n', unit)
        self.assertIn('NoNewPrivileges=yes\n', unit)
        self.assertIn('CapabilityBoundingSet=\n', unit)
        self.assertIn('/var/lib/asterion/nodes/node-1/asterion-node-agent', unit)
        for identity, port, bind in [('../escape', 7442, '::'), ('x\nUser=root', 7442, '::'), ('x', 22, '::'), ('x', 65536, '::'), ('x', 7442, '::\nUser=root')]:
            with self.assertRaises(ValueError):
                host.service_text(identity, port, bind)

    def test_helper_rejects_arbitrary_commands_before_writing(self):
        with patch.object(host, 'account'), patch.object(host, 'put_once') as write, patch.object(host, 'run') as run:
            for args in (['sh', '/tmp/upload'], ['install', 'x', '7442', '::', '--root'], ['install', '../../other', '7442', '::'], ['install', 'x', '0', '::']):
                with self.assertRaises(ValueError):
                    host.manage(args)
            write.assert_not_called()
            run.assert_not_called()
            host.manage(['install', 'node-1', '7442', '::'])
            self.assertEqual(write.call_args.args[0], Path('/etc/systemd/system/asterion-node-agent-node-1.service'))
            self.assertEqual(run.call_args.args, ('/usr/bin/systemctl', 'enable', '--now', 'asterion-node-agent-node-1.service'))

    def test_existing_files_and_symlinks_not_overwritten(self):
        with tempfile.TemporaryDirectory() as root, patch.object(host, 'protected_directory'):
            existing = Path(root) / 'existing'
            existing.write_text('unrelated configuration')
            link = Path(root) / 'link'
            link.symlink_to(existing)
            for path in (existing, link):
                with self.assertRaises(ValueError):
                    host.put_once(path, 'replacement', 0o644)
            self.assertEqual(existing.read_text(), 'unrelated configuration')

    def test_writable_administrative_parent_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root)
            path.chmod(0o777)
            with self.assertRaises(ValueError):
                host.protected_directory(path)


if __name__ == '__main__':
    unittest.main()
