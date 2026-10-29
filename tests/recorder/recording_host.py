"""The recording host script: trading hours, the unit, bundle checks and the
refusal to restart a running recorder during trading. No root, no systemd:
what changes the host is patched out.

    recording_host.py <scripts/recorder/recording-host.py>
"""
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

spec = importlib.util.spec_from_file_location('recording_host', sys.argv.pop(1))
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
BEIJING = timezone(timedelta(hours=8))


def at(day, hour, minute):
    return datetime(2026, 10, day, hour, minute, tzinfo=BEIJING)  # 2026-10-26 is a Monday


def bundle(files, manifest=None):
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w') as archive:
        for name, content in files.items():
            archive.writestr(name, content)
        archive.writestr('manifest.json', json.dumps(manifest or {
            'product_version': '0.1.0',
            'files': {name: hashlib.sha256(content).hexdigest() for name, content in files.items()}}))
    data.seek(0)
    return data


PROGRAMS = {'asterion_recorder': b'\x7fELF-recorder', 'ctp-md.so': b'\x7fELF-md',
            'ctp-trader.so': b'\x7fELF-td', 'recording-host.py': b'print()'}


class Hours(unittest.TestCase):
    def test_sessions_and_their_margins_are_trading(self):
        for moment in (at(26, 8, 45), at(26, 11, 40), at(26, 15, 29), at(26, 20, 45), at(26, 23, 59),
                       at(27, 2, 44), at(31, 1, 0)):  # Saturday 01:00 ends Friday's night session
            self.assertTrue(host.trading(moment), moment)

    def test_outside_sessions_an_upgrade_may_restart(self):
        for moment in (at(26, 8, 44), at(26, 15, 30), at(26, 20, 44), at(27, 2, 45), at(26, 3, 0),
                       at(31, 9, 0), at(31, 22, 0), at(26, 1, 0)):  # Monday 01:00 follows a weekend
            self.assertFalse(host.trading(moment), moment)

    def test_a_running_recorder_is_not_restarted_during_trading(self):
        with patch.object(host, 'active', return_value=True):
            with self.assertRaisesRegex(ValueError, 'trading hours'):
                host.outside_trading('An upgrade', at(26, 10, 0))
            host.outside_trading('An upgrade', at(26, 17, 0))
        with patch.object(host, 'active', return_value=False):
            host.outside_trading('An upgrade', at(26, 10, 0))


class Unit(unittest.TestCase):
    def test_systemd_runs_it_alone_and_never_stops_restarting(self):
        unit = host.unit_text(7450)
        for line in ('User=asterion-recorder\n', 'Restart=always\n', 'StartLimitIntervalSec=0\n',
                     'ImportCredential=asterion.ctp.*\n', 'StateDirectory=asterion-recorder\n',
                     'NoNewPrivileges=yes\n', 'CapabilityBoundingSet=\n'):
            self.assertIn(line, unit)
        self.assertIn('--bind 0.0.0.0 --port 7450 --tls-ca /etc/asterion-recorder/tls/ca.crt', unit)
        self.assertIn('/opt/asterion-recorder/current/asterion_recorder --directory /var/lib/asterion-recorder', unit)
        with self.assertRaises(ValueError):
            host.unit_text(80)

    def test_the_certificate_names_how_terminal_reaches_the_host(self):
        self.assertEqual(host.address('10.0.0.5'), 'IP:10.0.0.5')
        self.assertEqual(host.address('recorder.example'), 'DNS:recorder.example')
        for value in ('a b', 'x\nsubjectAltName=IP:1.1.1.1', ''):
            with self.assertRaises(ValueError):
                host.address(value)


class Bundle(unittest.TestCase):
    def test_every_file_must_match_its_manifest(self):
        manifest, files = host.read_bundle(bundle(PROGRAMS))
        self.assertEqual(set(files), set(host.FILES))
        tampered = dict(PROGRAMS)
        manifest = {'product_version': '0.1.0',
                    'files': {name: hashlib.sha256(content).hexdigest() for name, content in tampered.items()}}
        tampered['ctp-md.so'] = b'\x7fELF-other'
        with self.assertRaisesRegex(ValueError, 'checksum mismatch: ctp-md.so'):
            host.read_bundle(bundle(tampered, manifest))
        with self.assertRaisesRegex(ValueError, 'Unexpected bundle members'):
            host.read_bundle(bundle({**PROGRAMS, 'extra': b''}))

    def test_an_upgrade_during_trading_unpacks_but_does_not_switch(self):
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(host, 'install_release', return_value=host.Path(folder)), \
                patch.object(host, 'active', return_value=True), \
                patch.object(host, 'trading', return_value=True), \
                patch.object(host, 'switch') as switch, patch.object(host, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'trading hours'):
                host.upgrade(type('Args', (), {'bundle': 'x.zip'}))
            switch.assert_not_called()
            run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
