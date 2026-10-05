"""Permission-boundary tests; never alter host accounts, sudoers or services."""
import base64
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('initialize_host', Path(__file__).resolve().parents[1] / 'scripts/node/initialize-linux.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


class HostInitialization(unittest.TestCase):
    def test_check_reports_the_current_durable_publication_contract(self):
        output = io.StringIO()
        with patch.object(host, 'account'), contextlib.redirect_stdout(output), patch.object(host, 'run') as run:
            host.manage(['check'])
        self.assertEqual(output.getvalue(), 'asterion-host-v2\n')
        run.assert_not_called()

    def test_failed_file_flush_does_not_publish_partial_administrative_configuration(self):
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as root, patch.object(host, 'protected_directory'):
            path = Path(root) / 'configuration'
            with patch.object(host.os, 'fsync', side_effect=OSError('injected file flush failure')):
                with self.assertRaisesRegex(OSError, 'injected'):
                    host.put_once(path, 'complete configuration', 0o644)
            self.assertFalse(path.exists())
            self.assertEqual(list(Path(root).iterdir()), [])

    def test_matching_configuration_retry_requires_directory_sync_without_rewriting(self):
        # Emulate root ownership only; all I/O is confined to the disposable folder.
        original_stat, original_lstat, original_fstat, original_sync = os.stat, os.lstat, os.fstat, os.fsync
        def root_owned(info):
            values = list(info); values[4] = 0
            return os.stat_result(values)
        def fail_directory(fd):
            if stat.S_ISDIR(original_fstat(fd).st_mode):
                raise OSError('injected directory flush failure')
            original_sync(fd)
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as root, patch.object(host, 'protected_directory'):
            path = Path(root) / 'configuration'
            path.write_text('complete configuration'); path.chmod(0o644)
            before = original_stat(path)
            with patch.object(host.os, 'stat', side_effect=lambda *a, **kw: root_owned(original_stat(*a, **kw))), patch.object(host.os, 'lstat', side_effect=lambda *a, **kw: root_owned(original_lstat(*a, **kw))), patch.object(host.os, 'fstat', side_effect=lambda fd: root_owned(original_fstat(fd))):
                with patch.object(host.os, 'fsync', side_effect=fail_directory):
                    with self.assertRaisesRegex(OSError, 'directory flush'):
                        host.put_once(path, 'complete configuration', 0o644)
                host.put_once(path, 'complete configuration', 0o644)
            after = original_stat(path)
            self.assertEqual((before.st_ino, before.st_mtime_ns, before.st_mode),
                             (after.st_ino, after.st_mtime_ns, after.st_mode))
            self.assertEqual(path.read_text(), 'complete configuration')

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
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as root, patch.object(host, 'protected_directory'):
            existing = Path(root) / 'existing'
            existing.write_text('unrelated configuration')
            link = Path(root) / 'link'
            link.symlink_to(existing)
            for path in (existing, link):
                with self.assertRaises(ValueError):
                    host.put_once(path, 'replacement', 0o644)
            self.assertEqual(existing.read_text(), 'unrelated configuration')

    def test_writable_administrative_parent_rejected(self):
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as root:
            path = Path(root)
            path.chmod(0o777)
            with self.assertRaises(ValueError):
                host.protected_directory(path)


if __name__ == '__main__':
    unittest.main()
