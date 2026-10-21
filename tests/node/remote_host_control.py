"""Restricted host commands on disposable unit definitions; no actual systemctl calls."""
import importlib.util
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

source = Path(__file__).resolve().parents[2]/'scripts/node/initialize-linux.py'
spec = importlib.util.spec_from_file_location('host_control', source)
host = importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
with tempfile.TemporaryDirectory(prefix='ast-host-') as directory:
    host.UNITS = Path(directory)
    unit = host.UNITS/'asterion-node-agent-research.service'
    unit.write_text(host.service_text('research', 46000, '0.0.0.0')); unit.chmod(0o644)
    calls = []
    def run(*args, **kwargs):
        calls.append(args)
        return SimpleNamespace(stdout='inactive\n')
    actual_stat = os.fstat
    def root_stat(fd):
        info = actual_stat(fd)
        return SimpleNamespace(st_mode=info.st_mode, st_size=info.st_size, st_uid=0)
    with patch.object(host, 'account'), patch.object(host, 'protected_directory'), patch.object(host, 'run', run), patch.object(host.os, 'fstat', root_stat):
        for command in (['stop', '../foreign'], ['start', 'research.service'], ['exec', 'research']):
            try: host.manage(command)
            except ValueError: pass
            else: raise AssertionError('unsafe host action accepted')
        assert not calls
        host.manage(['stop', 'research'])
        assert calls[0] == ('/usr/bin/systemctl', 'disable', '--now', unit.name)
        calls.clear()
        host.manage(['start', 'research'])
        assert calls == [('/usr/bin/systemctl', 'enable', '--now', unit.name)]
        calls.clear()
        original = unit.read_text(); unit.write_text(original + 'ExecStartPost=/bin/false\n')
        try: host.manage(['remove', 'research'])
        except ValueError: pass
        else: raise AssertionError('foreign definition accepted')
        assert not calls and unit.exists()
        unit.write_text(original)
        host.manage(['remove', 'research'])
        assert not unit.exists() and calls[-1] == ('/usr/bin/systemctl', 'daemon-reload')
        calls.clear(); host.manage(['remove', 'research'])
        assert calls == [('/usr/bin/systemctl', 'daemon-reload')]
        unit.symlink_to(source)
        try: host.manage(['start', 'research'])
        except OSError: pass
        else: raise AssertionError('symlink unit accepted')
print('Restricted host scope, fixed unit ownership/content, stop/start, explicit removal and interrupted removal verified')
